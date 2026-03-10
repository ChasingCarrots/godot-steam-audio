#include "server.hpp"
#include "geometry_common.hpp"
#include "godot_cpp/classes/audio_effect.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/os.hpp"
#include "godot_cpp/classes/project_settings.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/utility_functions.hpp"
#include <phonon.h>

#include "listener.hpp"
#include "profiling.h"
#include "source.hpp"

using namespace godot;

SteamAudioServer *SteamAudioServer::self = nullptr;

static float calculate_attenuation(float dist, float min_dist, float max_dist) {
	if (dist <= min_dist) {
		return 1.0f;
	} else if (dist >= max_dist) {
		return 0.0f;
	} else {
		// Inverse-distance falloff: min_dist / dist
		// At min_dist → 1.0, approaches 0 as dist grows
		// Normalized so it reaches 0 at max_dist
		float inv_falloff = min_dist / dist;
		float inv_at_max = min_dist / max_dist;
		// Remap [inv_at_max, 1.0] → [0.0, 1.0]
		return (inv_falloff - inv_at_max) / (1.0f - inv_at_max);
	}
}

static int get_project_int(const String &key, int def) {
	Variant v = ProjectSettings::get_singleton()->get_setting(key);
	if (v.get_type() == Variant::NIL)
		return def;
	return int(v);
}

static float get_project_float(const String &key, float def) {
	Variant v = ProjectSettings::get_singleton()->get_setting(key);
	if (v.get_type() == Variant::NIL)
		return def;
	return float(v);
}

void cleanup_source_listener_data(SourceListenerData &sld, IPLContext ctx) {
	PROFILE_FUNCTION();
	if (sld.source) {
		iplSourceRelease(&sld.source);
	}
	if (sld.binaural_effect)
		iplBinauralEffectRelease(&sld.binaural_effect);
	if (sld.direct_effect)
		iplDirectEffectRelease(&sld.direct_effect);
	if (sld.reflection_effect)
		iplReflectionEffectRelease(&sld.reflection_effect);
	if (sld.ambisonics_decode_effect)
		iplAmbisonicsDecodeEffectRelease(&sld.ambisonics_decode_effect);
	iplAudioBufferFree(ctx, &sld.input_buffer);
	iplAudioBufferFree(ctx, &sld.output_buffer);
	iplAudioBufferFree(ctx, &sld.ambisonics_buffer);
}

bool create_source_listener_data(SourceListenerData &sld, SteamAudioSource *source_node, SteamAudioListener *listener, IPLContext ctx, IPLAudioSettings *audio_settings, IPLHRTF hrtf) {
	PROFILE_FUNCTION();
	sld.listener = listener;

	IPLBinauralEffectSettings binaural_cfg{};
	binaural_cfg.hrtf = hrtf;
	if (!handleErr(iplBinauralEffectCreate(ctx, audio_settings, &binaural_cfg, &sld.binaural_effect), "SteamAudio: Failed to create binaural effect")) {
		return false;
	}

	IPLDirectEffectSettings direct_cfg{};
	direct_cfg.numChannels = 1;
	handleErr(iplDirectEffectCreate(ctx, audio_settings, &direct_cfg, &sld.direct_effect), "SteamAudio: Failed to create direct effect");

	iplAudioBufferAllocate(ctx, 1, audio_settings->frameSize, &sld.input_buffer);
	iplAudioBufferAllocate(ctx, 2, audio_settings->frameSize, &sld.output_buffer);

	if (source_node->get_reflection_enabled() && listener->get_reflection_simulation_enabled()) {
		int max_order = listener->get_refl_ambisonics_order();
		int num_channels = ambisonic_channels_from(max_order);

		IPLReflectionEffectSettings refl_cfg{};
		refl_cfg.type = static_cast<IPLReflectionEffectType>(listener->get_refl_type());
		refl_cfg.numChannels = num_channels;
		refl_cfg.irSize = int(source_node->get_reflection_duration() * audio_settings->samplingRate);
		handleErr(iplReflectionEffectCreate(ctx, audio_settings, &refl_cfg, &sld.reflection_effect), "SteamAudio: Failed to create reflection effect");

		IPLAmbisonicsDecodeEffectSettings decode_cfg{};
		decode_cfg.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
		decode_cfg.hrtf = hrtf;
		decode_cfg.maxOrder = max_order;
		handleErr(iplAmbisonicsDecodeEffectCreate(ctx, audio_settings, &decode_cfg, &sld.ambisonics_decode_effect), "SteamAudio: Failed to create ambisonics decode effect");

		iplAudioBufferAllocate(ctx, num_channels, audio_settings->frameSize, &sld.ambisonics_buffer);
	}

	return true;
}

void SteamAudioServer::register_settings() {
	auto ps = ProjectSettings::get_singleton();
	if (!ps->has_setting("steamaudio/max_ambisonics_order"))
		ps->set_setting("steamaudio/max_ambisonics_order", 1);
	if (!ps->has_setting("steamaudio/max_rays"))
		ps->set_setting("steamaudio/max_rays", 512);
	if (!ps->has_setting("steamaudio/num_bounces"))
		ps->set_setting("steamaudio/num_bounces", 8);
	if (!ps->has_setting("steamaudio/scene_type"))
		ps->set_setting("steamaudio/scene_type", IPL_SCENETYPE_EMBREE);
	if (!ps->has_setting("steamaudio/max_occlusion_samples"))
		ps->set_setting("steamaudio/max_occlusion_samples", 64);
}

SteamAudioServer::SteamAudioServer() {
	self = this;
	is_running.store(false);
	refl_thread_wait_for_commit.store(false);
	is_refl_thread_processing.store(false);
	new_inputs_set.store(false);
	register_settings();

	if (!Engine::get_singleton()->is_editor_hint()) {
		init();
	}
}

SteamAudioServer::~SteamAudioServer() {
	finish();
	self = nullptr;
}

SteamAudioServer *SteamAudioServer::get_singleton() { return self; }

void SteamAudioServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("simulation_thread_func"), &SteamAudioServer::simulation_thread_func);
	ClassDB::bind_method(D_METHOD("tick", "delta"), &SteamAudioServer::tick);
	ClassDB::bind_method(D_METHOD("mixing_thread_func"), &SteamAudioServer::mixing_thread_func);
	ClassDB::bind_static_method("SteamAudioServer", D_METHOD("get_singleton"), &SteamAudioServer::get_singleton);
	ClassDB::bind_method(D_METHOD("get_source_count"), &SteamAudioServer::get_source_count);
	ClassDB::bind_method(D_METHOD("get_listener_count"), &SteamAudioServer::get_listener_count);
	ClassDB::bind_method(D_METHOD("get_source_name", "index"), &SteamAudioServer::get_source_name);
	ClassDB::bind_method(D_METHOD("get_listener_name", "index"), &SteamAudioServer::get_listener_name);
	ClassDB::bind_method(D_METHOD("get_source_debug_string", "index"), &SteamAudioServer::get_source_debug_string);
	ClassDB::bind_method(D_METHOD("get_listener_debug_string", "index"), &SteamAudioServer::get_listener_debug_string);
}

int SteamAudioServer::get_source_count() {
	std::shared_lock lock(collections_mutex);
	return (int)sources.size();
}

int SteamAudioServer::get_listener_count() {
	std::shared_lock lock(collections_mutex);
	return (int)listeners.size();
}

String SteamAudioServer::get_source_name(int index) {
	std::shared_lock lock(collections_mutex);
	if (index < 0 || index >= (int)sources.size())
		return "";
	auto &sd = sources[index];
	if (!sd.source_node)
		return "<null>";
	return sd.source_node->get_name();
}

String SteamAudioServer::get_listener_name(int index) {
	std::shared_lock lock(collections_mutex);
	if (index < 0 || index >= (int)listeners.size())
		return "";
	auto &ld = listeners[index];
	if (!ld.listener)
		return "<null>";
	return ld.listener->get_name();
}

String SteamAudioServer::get_source_debug_string(int index) {
	std::shared_lock lock(collections_mutex);
	if (index < 0 || index >= (int)sources.size())
		return "";
	auto &sd = sources[index];
	int frame_size = cached_audio_settings.frameSize;

	String s;
	s += "mixed_frames_ready: " + String::num_int64(sd.mixed_frames_ready) + "/" + String::num_int64(frame_size) + "\n";
	s += "pending_consumers: " + String::num_int64(sd.pending_consumers) + "\n";
	s += "debug_times_mixed: " + String::num_int64(sd.debug_times_mixed) + "\n";
	s += "playbacks: " + String::num_int64((int)sd.playbacks.size()) + "\n";
	for (uint32_t i = 0; i < sd.playbacks.size(); ++i) {
		auto &pb = sd.playbacks[i];
		s += "  pb[" + String::num_int64(i) + "]: playing=" + String(pb.playback->is_playing() ? "yes" : "no");
		s += " vol=" + String::num(pb.volume_linear, 3);
		s += " pitch=" + String::num(pb.pitch_scale, 3);
		s += " num_mixed=" + String::num_int64(pb.debug_num_mixed);
		s += " mixed_in_mixed_frames=" + String::num_int64(pb.num_mixed_in_current_mixed_frames) + "\n";
	}
	s += "effect_instances: " + String::num_int64((int)sd.effect_instances.size()) + "\n";
	s += "listener_data: " + String::num_int64((int)sd.listener_data.size()) + "\n";
	for (uint32_t i = 0; i < sd.listener_data.size(); ++i) {
		auto &sld = sd.listener_data[i];
		String lname = sld.listener ? String(sld.listener->get_name()) : "<null>";
		s += "  sld[" + String::num_int64(i) + "] listener=" + lname;
		s += " dist=" + String::num(sld.dist_to_listener, 2);
		s += " doppler=" + String::num(sld.doppler_pitch, 3);
		s += " OutOfRange=" + String(sld.out_of_range ? "yes" : "no");
		s += " last_gen=" + String::num_int64(sld.last_contributed_generation);
		s += " times_contributed=" + String::num_int64(sld.debug_times_contributed);
 	s += "\n";
	}
	return s;
}

String SteamAudioServer::get_listener_debug_string(int index) {
	std::shared_lock lock(collections_mutex);
	if (index < 0 || index >= (int)listeners.size())
		return "";
	auto &ld = listeners[index];
	int frame_size = cached_audio_settings.frameSize;

	String s;
	s += "pending_contributors: " + String::num_int64(ld.pending_contributors) + "\n";
	s += "pending_drains: " + String::num_int64(ld.pending_drains) + "\n";
	s += "times_pushed: " + String::num_int64(ld.debug_times_pushed) + "\n";
	s += "generation: " + String::num_int64(ld.generation) + "\n";
	s += "push_buffer size: " + String::num_int64((int)ld.push_buffer.size()) + "/" + String::num_int64(frame_size) + "\n";
	{
		std::lock_guard pb_lock(*ld.playbacks_mutex);
		s += "playbacks: " + String::num_int64((int)ld.playbacks.size()) + "\n";
		for (uint32_t i = 0; i < ld.playbacks.size(); ++i) {
			auto &pb = ld.playbacks[i];
			s += "  pb[" + String::num_int64(i) + "]: playing=" + String(pb.playback->is_playing() ? "yes" : "no");
			s += " remaining=" + String::num_int64(pb.remaining_from_push_buffer);
			s += " times_drained=" + String::num_int64(pb.debug_times_drained);
			s += " avail=" + String::num_int64(pb.playback->get_free_buffer_size());
			s += " underruns=" + String::num_int64(pb.playback->get_num_underrun_samples()) + "\n";
		}
	}
	s += "source_db_levels: " + String::num_int64((int)ld.source_db_levels.size()) + "\n";
	for (uint32_t i = 0; i < ld.source_db_levels.size(); ++i) {
		auto &sldb = ld.source_db_levels[i];
		s += "  " + String(sldb.source->get_name()) + ": db_level=" + String::num_real(sldb.db_level) + "\n";
	}
	if (ld.listener->get_num_source_db_sensor_slots() > 0) {
		s += "source_db_sensor_slots: " + String::num_int64((int)ld.listener->get_num_source_db_sensor_slots()) + "\n";
		for (uint32_t i = 0; i < ld.listener->get_num_source_db_sensor_slots(); ++i) {
			const auto& slot = ld.listener->get_sensor_slot(i);
			String name = slot->get_steam_audio_source() ? String(slot->get_steam_audio_source()->get_name()) : "<null>";
			s += "  slot[" + String::num_int64(i) + "]: " + name + " db=" + String::num_real(slot->get_db_level()) + "\n";
		}
	}
	s += "has_simulator: " + String(ld.simulator ? "yes" : "no") + "\n";
	s += "dirty: " + String(ld.dirty ? "yes" : "no") + "\n";
	return s;
}

IPLAudioSettings SteamAudioServer::get_audio_settings() {
	int mix_rate = get_project_int("audio/driver/mix_rate", 48000);
	int out_latency_ms = get_project_int("audio/driver/output_latency", 15);
	int frame_size = 1;
	{
		int target = int((mix_rate * out_latency_ms) / 1000.0f);
		frame_size = 1;
		while (frame_size < target)
			frame_size <<= 1;
	}
	return IPLAudioSettings{ mix_rate, frame_size };
}

void SteamAudioServer::init() {
	if (is_initialized)
		return;
	is_initialized = true;

	if (is_running.load())
		return;

	IPLContextSettings ctx_cfg{};
	ctx_cfg.version = STEAMAUDIO_VERSION;
	if (!handleErr(iplContextCreate(&ctx_cfg, &phonon_context), "SteamAudio: Failed to create context")) {
		phonon_context = nullptr;
		return;
	}

	IPLSceneSettings scene_cfg{};
	scene_cfg.radeonRaysDevice = nullptr;
	scene_cfg.type = static_cast<IPLSceneType>(get_project_int("steamaudio/scene_type", IPL_SCENETYPE_DEFAULT));
	if (scene_cfg.type == IPL_SCENETYPE_EMBREE) {
		IPLEmbreeDeviceSettings embree_cfg{};
		iplEmbreeDeviceCreate(phonon_context, &embree_cfg, &embree_dev);
		scene_cfg.embreeDevice = embree_dev;
	}
	if (!handleErr(iplSceneCreate(phonon_context, &scene_cfg, &phonon_scene), "SteamAudio: Failed to create scene")) {
		phonon_scene = nullptr;
	}

	// Cache audio settings once
	cached_audio_settings = get_audio_settings();

	IPLHRTFSettings hrtf_cfg{};
	hrtf_cfg.type = IPL_HRTFTYPE_DEFAULT;
	hrtf_cfg.volume = 1.0f;
	if (!handleErr(iplHRTFCreate(phonon_context, &cached_audio_settings, &hrtf_cfg, &phonon_hrtf), "SteamAudio: Failed to create HRTF")) {
		phonon_hrtf = nullptr;
	}

	is_running.store(true);

	simulation_thread.instantiate();
	simulation_thread->start(Callable(this, "simulation_thread_func"));
	mixing_thread.instantiate();
	mixing_thread->start(Callable(this, "mixing_thread_func"), Thread::PRIORITY_HIGH);
}

void SteamAudioServer::finish() {
	if (!is_running.load())
		return;
	is_running.store(false);

	{
		std::lock_guard<std::mutex> lock(refl_mux);
		refl_cv.notify_all();
	}

	if (simulation_thread.is_valid() && simulation_thread->is_started()) {
		simulation_thread->wait_to_finish();
	}
	if (mixing_thread.is_valid() && mixing_thread->is_started()) {
		mixing_thread->wait_to_finish();
	}

	// Drain any remaining pending ops now that threads are stopped
	apply_pending_ops();

	for (auto &ld : listeners) {
		if (ld.simulator) {
			iplSimulatorRelease(&ld.simulator);
		}
	}
	listeners.clear();

	for (auto &sd : sources) {
		for (auto &sld : sd.listener_data) {
			cleanup_source_listener_data(sld, phonon_context);
		}
	}
	sources.clear();

	for (auto &dg : dynamic_geometry) {
		if (dg.instanced_mesh)
			iplInstancedMeshRelease(&dg.instanced_mesh);
		for (auto &m : dg.meshes)
			iplStaticMeshRelease(&m);
		if (dg.sub_scene)
			iplSceneRelease(&dg.sub_scene);
	}
	dynamic_geometry.clear();

	for (auto &sg : static_geometry) {
		for (auto &m : sg.meshes) {
			iplStaticMeshRemove(m, phonon_scene);
			iplStaticMeshRelease(&m);
		}
	}
	static_geometry.clear();

	if (phonon_hrtf) {
		iplHRTFRelease(&phonon_hrtf);
		phonon_hrtf = nullptr;
	}
	if (phonon_scene) {
		iplSceneRelease(&phonon_scene);
		phonon_scene = nullptr;
	}
	if (phonon_context) {
		iplContextRelease(&phonon_context);
		phonon_context = nullptr;
	}
}

void SteamAudioServer::tick(float delta) {
	if (Engine::get_singleton()->is_editor_hint())
		return;
	if (!is_running.load())
		return;
	PROFILE_FUNCTION();

	std::shared_lock lock(collections_mutex);

	{
		PROFILE_FUNCTION_NAMED("update_dynamic_geometry");
		PROFILING_PLOT_NUMBER("NumSteamDynamicGeometries", (int64_t)dynamic_geometry.size());
		// Update Dynamic Geometry (only mark dirty if transform changed)
		for (auto &dg : dynamic_geometry) {
			if (!dg.node || !dg.instanced_mesh)
				continue;
			Transform3D trf = dg.node->get_global_transform();
			if (!trf.is_equal_approx(dg.last_trf)) {
				IPLMatrix4x4 m = ipl_mat4_from(trf);
				iplInstancedMeshUpdateTransform(dg.instanced_mesh, phonon_scene, m);
				dg.last_trf = trf;
				scene_dirty = true;
			}
		}
	}

	{
		PROFILE_FUNCTION_NAMED("update_listeners");
		for (auto &ld : listeners) {
			if (!ld.listener)
				continue;
			if (!ld.simulator)
				continue;

			// Update listener transform from node directly
			Transform3D trf = ld.listener->get_global_transform();
			if (!trf.is_equal_approx(ld.last_trf)) {
				ld.cached_coords = ipl_coords_from(trf);

				IPLSimulationSharedInputs shared_inputs{};
				shared_inputs.listener = ld.cached_coords;
				shared_inputs.numRays = ld.listener->get_num_refl_rays();
				shared_inputs.numBounces = ld.listener->get_num_refl_bounces();
				shared_inputs.duration = ld.listener->get_refl_duration();
				shared_inputs.order = ld.listener->get_refl_ambisonics_order();
				shared_inputs.irradianceMinDistance = ld.listener->get_irradiance_min_dist();

				IPLSimulationFlags flags = IPL_SIMULATIONFLAGS_DIRECT;
				if (ld.listener->get_reflection_simulation_enabled()) {
					flags = static_cast<IPLSimulationFlags>(flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
				}

				iplSimulatorSetSharedInputs(ld.simulator, flags, &shared_inputs);
				ld.last_trf = trf;
			}

			// Update source transforms for this listener's simulator
			for (auto &sd : sources) {
				if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0)
					continue;

				// Find the SourceListenerData for this listener/source pair
				SourceListenerData *sld = nullptr;
				for (auto &entry : sd.listener_data) {
					if (entry.listener == ld.listener) {
						sld = &entry;
						break;
					}
				}
				if (!sld)
					continue;

				// Create IPLSource per SourceListenerData if needed
				if (!sld->source) {
					IPLSourceSettings source_settings{};
					source_settings.flags = static_cast<IPLSimulationFlags>(
							(sd.source_node->get_direct_enabled() ? IPL_SIMULATIONFLAGS_DIRECT : 0) | IPL_SIMULATIONFLAGS_REFLECTIONS);
					if (handleErr(iplSourceCreate(ld.simulator, &source_settings, &sld->source), "SteamAudio: Failed to create source")) {
						iplSourceAdd(sld->source, ld.simulator);
						ld.dirty = true;
					}
				}

				Transform3D src_trf = sd.source_node->get_global_transform();
				sd.cached_coords = ipl_coords_from(src_trf);

				// Update dist_to_listener and doppler
				float prev_dist_to_listener = sld->dist_to_listener;
				sld->dist_to_listener = src_trf.origin.distance_to(ld.last_trf.origin);

				float doppler_factor = sd.source_node->get_doppler_factor();
				// skip the very first update (dist is 0 and doppler_pitch is 1) so that we don't get
				// enormous speeds
				if (doppler_factor > 0.0f && prev_dist_to_listener != 0.0f) {
					if (delta > 0.0) {
						float radial_speed = (sld->dist_to_listener - prev_dist_to_listener) / delta;
						constexpr float speed_of_sound = 343.0f;
						float adjusted_speed = radial_speed * doppler_factor;
						float denominator = speed_of_sound + adjusted_speed;
						if (denominator > 0.0f) {
							sld->doppler_pitch = speed_of_sound / denominator;
						} else {
							sld->doppler_pitch = 4.0f; // cap when approaching/exceeding speed of sound
						}
						sld->doppler_pitch = CLAMP(sld->doppler_pitch, 0.25f, 4.0f);
					}
				} else {
					sld->doppler_pitch = 1.0f;
				}

				// Range check with hysteresis
				float listener_range = ld.listener->get_range();
				// when the source doesn't have reflection enabled,
				// we can also use the max distance of that
				if (!sd.source_node->get_reflection_enabled())
					listener_range = MIN(listener_range, sd.source_node->get_distance_attenuation_max());
				if (listener_range > 0.0f) {
					if (!sld->out_of_range && sld->dist_to_listener > listener_range * 1.1f) {
						sld->out_of_range = true;
						if (sld->source) {
							iplSourceRemove(sld->source, ld.simulator);
							ld.dirty = true;
						}
					} else if (sld->out_of_range && sld->dist_to_listener <= listener_range) {
						sld->out_of_range = false;
						if (sld->source) {
							iplSourceAdd(sld->source, ld.simulator);
							ld.dirty = true;
						}
					}
				} else if (sld->out_of_range) {
					// Range was disabled, bring back in range
					sld->out_of_range = false;
					if (sld->source) {
						iplSourceAdd(sld->source, ld.simulator);
						ld.dirty = true;
					}
				}

				if (sld->out_of_range) {
					sd.last_trf = src_trf;
					continue;
				}

				IPLSimulationInputs inputs{};
				inputs.flags = static_cast<IPLSimulationFlags>(0);
				if (sd.source_node->get_direct_enabled()) {
					inputs.flags = static_cast<IPLSimulationFlags>(inputs.flags | IPL_SIMULATIONFLAGS_DIRECT);
				}
				if (sd.source_node->get_reflection_enabled()) {
					inputs.flags = static_cast<IPLSimulationFlags>(inputs.flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
				}

				inputs.directFlags = static_cast<IPLDirectSimulationFlags>(0);
				if (sd.source_node->get_direct_enabled()) {
					if (sd.source_node->get_occlusion_enabled()) {
						inputs.directFlags = static_cast<IPLDirectSimulationFlags>(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION);
					}
					if (sd.source_node->get_transmission_enabled()) {
						inputs.directFlags = static_cast<IPLDirectSimulationFlags>(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
					}
					if (sd.source_node->get_distance_attenuation_enabled()) {
						inputs.directFlags = static_cast<IPLDirectSimulationFlags>(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION);
					}
					if (sd.source_node->get_air_absorption_enabled()) {
						inputs.directFlags = static_cast<IPLDirectSimulationFlags>(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION);
					}
				}

				inputs.source = sd.cached_coords;
				inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
				inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_EXPONENTIAL;

				inputs.occlusionType = static_cast<IPLOcclusionType>(sd.source_node->get_occlusion_type());
				inputs.occlusionRadius = sd.source_node->get_occlusion_radius();
				inputs.numOcclusionSamples = sd.source_node->get_occlusion_samples();
				inputs.numTransmissionRays = sd.source_node->get_transmission_rays();

				for (int i = 0; i < IPL_NUM_BANDS; ++i)
					inputs.reverbScale[i] = 1.0f;
				inputs.hybridReverbTransitionTime = sd.source_node->get_reflection_hybrid_delay();
				inputs.hybridReverbOverlapPercent = 0.25f;
				inputs.baked = IPL_FALSE;

				iplSourceSetInputs(sld->source, inputs.flags, &inputs);
				sd.last_trf = src_trf;
			}

			if (ld.dirty) {
				refl_thread_wait_for_commit.store(true);
			}
		}
	}

	if (scene_dirty) {
		refl_thread_wait_for_commit.store(true);
	}

	if (refl_thread_wait_for_commit.load() && !is_refl_thread_processing.load()) {
		PROFILE_FUNCTION_NAMED("committing_scene_and_simulators");
		if (scene_dirty) {
			iplSceneCommit(phonon_scene);
			scene_dirty = false;
		}
		for (auto &ld : listeners) {
			if (ld.dirty) {
				iplSimulatorCommit(ld.simulator);
				ld.dirty = false;
			}
		}
		refl_thread_wait_for_commit.store(false);
	}

	{
		PROFILE_FUNCTION_NAMED("running_direct_simulation");
		for (auto &ld : listeners) {
			if (!ld.simulator)
				continue;
			iplSimulatorRunDirect(ld.simulator);

			{
				PROFILE_FUNCTION_NAMED("updating_source_db_levels");
				static LocalVector<Ref<SteamAudioListenerSensorSlot>> temp_sensor_slots;
				temp_sensor_slots.resize(ld.listener->get_num_source_db_sensor_slots());
				for (int source_slot_index = 0; source_slot_index < ld.listener->get_num_source_db_sensor_slots(); ++source_slot_index) {
					temp_sensor_slots[source_slot_index] = ld.listener->get_sensor_slot(source_slot_index);
					// decay dB level
					float current_db = temp_sensor_slots[source_slot_index]->get_db_level();
					current_db -= delta * 30.0f; // decay by 30 dB per second
					if (current_db < -60.0f) {
						temp_sensor_slots[source_slot_index]->set_db_level(-500.0f);
						temp_sensor_slots[source_slot_index]->set_steam_audio_source(nullptr);
					} else {
						temp_sensor_slots[source_slot_index]->set_db_level(current_db);
					}
				}
				// update source db levels
				ld.source_db_levels.clear();
				for (auto &sd : sources) {
					for (auto &sld : sd.listener_data) {
						if (sld.listener != ld.listener)
							continue;
						if ((sd.source_node->get_layers() & sld.listener->get_mask()) == 0)
							continue;
						if (sld.out_of_range)
							continue;
						IPLSimulationOutputs outputs{};
						iplSourceGetOutputs(sld.source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);

						ListenerSourceDBLevel source_db_level;
						source_db_level.source = sd.source_node;
						source_db_level.position = sd.source_node->get_global_position();
						source_db_level.db_level = sd.current_db_level;
						IPLDirectEffectParams direct_params = outputs.direct;
						float total_direct_factor = 1.0f;

						if (sd.source_node->get_occlusion_enabled()) {
							float occlusion = direct_params.occlusion;
							float transmission = 0.0f;
							if (sd.source_node->get_transmission_enabled()) {
								transmission = (direct_params.transmission[0] + direct_params.transmission[1] + direct_params.transmission[2]) / 3.0f;
							}
							// Total direct sound = sound that goes around (occlusion) + sound that goes through (transmission)
							// We assume transmission only applies to the occluded part.
							total_direct_factor *= (occlusion + (1.0f - occlusion) * transmission);
						}

						if (sd.source_node->get_air_absorption_enabled()) {
							IPLAirAbsorptionModel airAbsorptionModel{};
							airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
							float air_absorption[3];
							iplAirAbsorptionCalculate(phonon_context, sd.cached_coords.origin, ld.cached_coords.origin, &airAbsorptionModel, air_absorption);
							float avg_air_absorption = (air_absorption[0] + air_absorption[1] + air_absorption[2]) / 3.0f;
							total_direct_factor *= avg_air_absorption;
						}

						if (sd.source_node->get_distance_attenuation_enabled()) {
							float dist = sld.dist_to_listener;
							float min_dist = sd.source_node->get_distance_attenuation_min();
							float max_dist = sd.source_node->get_distance_attenuation_max();
							float attenuation = calculate_attenuation(dist, min_dist, max_dist);
							total_direct_factor *= attenuation;
						}

						float final_db_level = source_db_level.db_level + 20.0f * Math::log(MAX(total_direct_factor, 1e-10f)) / Math::log(10.0f);
						source_db_level.db_level = final_db_level;
						ld.source_db_levels.push_back(source_db_level);
						
						if (final_db_level < -60.0f || temp_sensor_slots.is_empty())
							continue;

						// the following will update the sensor slots on the SteamAudioListener!

						// update/insert current source in sensor slots if it's loud enough
						int num_slots = (int)temp_sensor_slots.size();

						// first check if the source is already in a slot
						int existing_slot_index = -1;
						for (int slot_index = 0; slot_index < num_slots; ++slot_index) {
							if (temp_sensor_slots[slot_index]->get_steam_audio_source() == sd.source_node) {
								existing_slot_index = slot_index;
								break;
							}
						}

						if (existing_slot_index != -1) {
							// source already exists, update it if the new level is higher
							if (final_db_level > temp_sensor_slots[existing_slot_index]->get_db_level()) {
								temp_sensor_slots[existing_slot_index]->set_db_level(final_db_level);
								temp_sensor_slots[existing_slot_index]->set_position(sd.source_node->get_global_position());

								// check if it needs to move up (becoming louder)
								int current_slot = existing_slot_index;
								while (current_slot > 0 && temp_sensor_slots[current_slot]->get_db_level() > temp_sensor_slots[current_slot - 1]->get_db_level()) {
									// swap values with the slot above it
									SteamAudioSource *prev_source = temp_sensor_slots[current_slot - 1]->get_steam_audio_source();
									Vector3 prev_position = temp_sensor_slots[current_slot - 1]->get_position();
									float prev_db = temp_sensor_slots[current_slot - 1]->get_db_level();

									temp_sensor_slots[current_slot - 1]->set_steam_audio_source(temp_sensor_slots[current_slot]->get_steam_audio_source());
									temp_sensor_slots[current_slot - 1]->set_position(temp_sensor_slots[current_slot]->get_position());
									temp_sensor_slots[current_slot - 1]->set_db_level(temp_sensor_slots[current_slot]->get_db_level());

									temp_sensor_slots[current_slot]->set_steam_audio_source(prev_source);
									temp_sensor_slots[current_slot]->set_position(prev_position);
									temp_sensor_slots[current_slot]->set_db_level(prev_db);

									current_slot--;
								}
							}
						} else {
							for (int slot_index = 0; slot_index < num_slots; ++slot_index) {
								if (final_db_level > temp_sensor_slots[slot_index]->get_db_level()) {
									// shift down remaining
									for (int shift_index = num_slots - 1; shift_index > slot_index; --shift_index) {
										temp_sensor_slots[shift_index]->set_steam_audio_source(temp_sensor_slots[shift_index - 1]->get_steam_audio_source());
										temp_sensor_slots[shift_index]->set_position(temp_sensor_slots[shift_index - 1]->get_position());
										temp_sensor_slots[shift_index]->set_db_level(temp_sensor_slots[shift_index - 1]->get_db_level());
									}
									temp_sensor_slots[slot_index]->set_steam_audio_source(sd.source_node);
									temp_sensor_slots[slot_index]->set_position(sd.source_node->get_global_position());
									temp_sensor_slots[slot_index]->set_db_level(final_db_level);
									break;
								}
							}
						}

					}
				}
				temp_sensor_slots.clear();
			}
		}
	}

	new_inputs_set.store(true);
	if (!is_refl_thread_processing.load()) {
		std::unique_lock<std::mutex> lock_refl(refl_mux);
		refl_cv.notify_one();
	}
}

void SteamAudioServer::simulation_thread_func() {
	LocalVector<IPLSimulator> simulators;
	while (is_running.load()) {
		if (refl_thread_wait_for_commit.load() || !new_inputs_set.load()) {
			std::unique_lock<std::mutex> lock_refl(refl_mux);
			refl_cv.wait(lock_refl, [&] {
				return (!refl_thread_wait_for_commit.load() && new_inputs_set.load()) || !is_running.load();
			});

			if (!is_running.load())
				break;
		}

		{
			is_refl_thread_processing.store(true);
			new_inputs_set.store(false);

			PROFILE_FUNCTION_NAMED("refl_sim");

			// Snapshot simulators with retain so we don't hold collections_mutex
			// during the potentially long iplSimulatorRunReflections calls.
			simulators.clear();
			{
				std::shared_lock lock(collections_mutex);
				for (auto &ld : listeners) {
					if (!ld.simulator)
						continue;
					simulators.push_back(iplSimulatorRetain(ld.simulator));
				}
			}

			for (auto &sim : simulators) {
				iplSimulatorRunReflections(sim);
			}

			for (auto &sim : simulators) {
				iplSimulatorRelease(&sim);
			}
			simulators.clear();

			is_refl_thread_processing.store(false);
		}
	}
}

void SteamAudioServer::mixing_thread_func() {
	using clock = std::chrono::steady_clock;
	auto wait_time = std::chrono::milliseconds(
		1
	);
	auto spin_threshold = std::chrono::microseconds(500);
	while (is_running.load()) {
		auto start = clock::now();
		process_audio();

		// hybrid spin sleep (try to reduce CPU usage while still being responsive)
		auto target = start + wait_time;
		while (clock::now() < target) {
			if (clock::now() + spin_threshold < target) {
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			} else {
				while (clock::now() < target) {
					std::this_thread::yield();
				}
			}
		}
	}
}

void SteamAudioServer::apply_pending_ops() {
	PROFILE_FUNCTION();

	std::vector<PendingOp> ops;
	{
		std::lock_guard lock(pending_ops_mutex);
		ops.swap(pending_ops);
	}

	if (ops.empty())
		return;

	// unique_lock to mutate the collections
	std::unique_lock lock(collections_mutex);

	for (auto &op : ops) {
		std::visit([&](auto &pending) {
			using T = std::decay_t<decltype(pending)>;

			if constexpr (std::is_same_v<T, PendingAddSource>) {
				// Check for duplicates
				for (const auto &sd : sources) {
					if (sd.source_node == pending.source_node)
						return;
				}

				SourceData &sd = pending.source_data;

				// Create cross-references with all existing listeners
				for (const auto &ld : listeners) {
					SourceListenerData sld;
					if (create_source_listener_data(sld, sd.source_node, ld.listener, phonon_context, &cached_audio_settings, phonon_hrtf)) {
						sd.listener_data.push_back(sld);
					}
				}

				sources.push_back(std::move(sd));

			} else if constexpr (std::is_same_v<T, PendingRemoveSource>) {
				for (uint32_t i = 0; i < sources.size(); ++i) {
					if (sources[i].source_node == pending.source_node) {
						SourceData &sd = sources[i];
						for (auto &sld : sd.listener_data) {
							// Adjust listener pending_contributors if this source was counted
							// but hasn't contributed yet to the current generation.
 						for (auto &ld : listeners) {
 							if (sld.listener == ld.listener && !sld.out_of_range &&
 									(sd.source_node->get_layers() & ld.listener->get_mask()) != 0 &&
 									sld.last_contributed_generation != ld.generation &&
 									ld.pending_contributors > 0) {
 								ld.pending_contributors--;
 							}
 						}

 						// Remove source from its simulator (reuse ld loop)
 						for (auto &ld : listeners) {
								if (ld.simulator && sld.source && sld.listener == ld.listener) {
									iplSourceRemove(sld.source, ld.simulator);
									ld.dirty = true;
								}
							}
							cleanup_source_listener_data(sld, phonon_context);
						}
						sources.remove_at(i);
						break;
					}
				}

			} else if constexpr (std::is_same_v<T, PendingAddListener>) {
				ListenerData &ld = pending.listener_data;

				// Create per-listener state for all existing sources
				for (auto &sd : sources) {
					SourceListenerData sld;
					if (create_source_listener_data(sld, sd.source_node, ld.listener, phonon_context, &cached_audio_settings, phonon_hrtf)) {
						sd.listener_data.push_back(sld);
					}
				}

				// Initialize pending_contributors so Phase 3 can start immediately
				ld.pending_contributors = 0;
				for (auto &sd : sources) {
					if (!sd.source_node)
						continue;
					if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0)
						continue;
					for (auto &sld : sd.listener_data) {
						if (sld.listener == ld.listener && !sld.out_of_range) {
							ld.pending_contributors++;
							break;
						}
					}
				}

				listeners.push_back(std::move(ld));

			} else if constexpr (std::is_same_v<T, PendingRemoveListener>) {
				SteamAudioListener *listener = pending.listener;

				// Find the ListenerData for counter adjustments
				ListenerData *removing_ld = nullptr;
				for (auto &ld : listeners) {
					if (ld.listener == listener) {
						removing_ld = &ld;
						break;
					}
				}

				// Clean up per-listener state in all sources
				for (auto &sd : sources) {
					for (uint32_t i = 0; i < sd.listener_data.size(); ++i) {
						if (sd.listener_data[i].listener == listener) {
							SourceListenerData &sld = sd.listener_data[i];

 						if (removing_ld && !sld.out_of_range &&
								(sd.source_node->get_layers() & listener->get_mask()) != 0 &&
								sld.last_contributed_generation != removing_ld->generation &&
								removing_ld->pending_contributors > 0) {
							removing_ld->pending_contributors--;
						}
						// Also release pending_consumers if this listener was counted
						if (!sld.out_of_range &&
								(sd.source_node->get_layers() & listener->get_mask()) != 0 &&
								sd.mixed_frames_ready >= cached_audio_settings.frameSize &&
								sd.pending_consumers > 0) {
							sd.pending_consumers--;
						}

						// Remove source from simulator before releasing
							for (auto &ld_entry : listeners) {
								if (ld_entry.listener == listener && ld_entry.simulator && sld.source) {
									iplSourceRemove(sld.source, ld_entry.simulator);
								}
							}
							cleanup_source_listener_data(sld, phonon_context);
							sd.listener_data.remove_at(i);
							break;
						}
					}
				}

				for (auto it = listeners.begin(); it != listeners.end(); ++it) {
					if (it->listener == listener) {
						if (it->simulator) {
							iplSimulatorRelease(&it->simulator);
						}
						listeners.erase(it);
						break;
					}
				}

			} else if constexpr (std::is_same_v<T, PendingAddPlaybackToSource>) {
				for (auto &sd : sources) {
					if (sd.source_node == pending.source_node) {
						SourcePlaybackEntry entry;
						entry.playback = pending.playback;
						entry.volume_linear = pending.volume_linear;
						entry.pitch_scale = pending.pitch_scale;
						sd.playbacks.push_back(entry);
						return;
					}
				}

 			} else if constexpr (std::is_same_v<T, PendingAddPlaybackToListener>) {
				for (auto &ld : listeners) {
					if (ld.listener == pending.listener) {
						bool was_empty;
						{
							std::lock_guard<std::mutex> pb_lock(*ld.playbacks_mutex);
							was_empty = ld.playbacks.is_empty();
							ld.playbacks.push_back({ pending.playback, 0 });
						}
						if (was_empty) {
							// Listener was inactive — start a fresh cycle.
							// Bump generation so stale last_contributed_generation won't match.
							ld.generation++;
							ld.pending_contributors = 0;
							ld.pending_drains = 0;

							const int frame_size = cached_audio_settings.frameSize;
							for (auto &sd : sources) {
								if (!sd.source_node)
									continue;
								if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0)
									continue;
								for (auto &sld : sd.listener_data) {
									if (sld.listener == ld.listener && !sld.out_of_range) {
										ld.pending_contributors++;
										// If source has a ready mix, increment pending_consumers
										// so it waits for this listener to consume before resetting.
										if (sd.mixed_frames_ready >= frame_size) {
											sd.pending_consumers++;
										}
										break;
									}
								}
							}
						}
						return;
					}
				}
			}
		}, op);
	}
}

void SteamAudioServer::process_audio() {
	PROFILE_FUNCTION();
	const int frame_size = cached_audio_settings.frameSize;

	// Apply any queued operations (briefly unique-locks collections_mutex)
	apply_pending_ops();

	std::shared_lock lock(collections_mutex);

	{
		PROFILE_FUNCTION_NAMED("process_audio_phase1");
		// =========================================================================
		// PHASE 1: Drain push_buffers into listener playbacks.
		// Push ready push_buffers to playbacks. When fully drained, clear and
		// recount pending_contributors for the next round.
		// =========================================================================
		for (auto &ld : listeners) {
			if (!ld.listener)
				continue;
			if (ld.pending_drains <= 0)
				continue;

			PROFILE_FUNCTION_NAMED("Listener draining");
			std::lock_guard pb_lock(*ld.playbacks_mutex);

			// Remove dead playbacks and push remaining data to active ones.
			for (int i = (int)ld.playbacks.size() - 1; i >= 0; --i) {
				if (!ld.playbacks[i].playback->is_playing()) {
					if (ld.playbacks[i].remaining_from_push_buffer > 0) {
						ld.pending_drains--;
					}
					ld.playbacks.remove_at(i);
					continue;
				}
				auto &pb = ld.playbacks[i];
				if (pb.remaining_from_push_buffer <= 0)
					continue;
				int free_available_in_buffer = pb.playback->get_free_buffer_size();
				if (pb.remaining_from_push_buffer <= free_available_in_buffer) {
					pb.playback->push_buffer(ld.push_buffer.slice(frame_size - pb.remaining_from_push_buffer));
					pb.remaining_from_push_buffer = 0;
				} else {
					int num_to_push = MIN(pb.remaining_from_push_buffer, free_available_in_buffer);
					if (num_to_push > 0) {
						int start = frame_size - pb.remaining_from_push_buffer;
						pb.playback->push_buffer(ld.push_buffer.slice(start, start + num_to_push));
						pb.remaining_from_push_buffer -= num_to_push;
					}
				}
				if (pb.remaining_from_push_buffer <= 0)
					ld.pending_drains--;
			}

			// All playbacks drained — clear buffer and recount contributors.
			if (ld.pending_drains <= 0) {
				ld.debug_times_pushed += 1;
				ld.pending_drains = 0;
				ld.push_buffer.fill(Vector2(0, 0));
				ld.generation++;

				// Count relevant in-range sources as contributors
				ld.pending_contributors = 0;
				for (auto &sd : sources) {
					if (!sd.source_node)
						continue;
					if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0)
						continue;
					for (auto &sld : sd.listener_data) {
						if (sld.listener == ld.listener && !sld.out_of_range) {
							ld.pending_contributors++;
							break;
						}
					}
				}
			}
		}
	}

	{
		PROFILE_FUNCTION_NAMED("process_audio_phase2");
		// =========================================================================
		// PHASE 2: Mix source playbacks into mixed_frames.
		// For each source with no pending consumers, reset and pull new audio.
		// =========================================================================
		for (auto &sd : sources) {
			if (!sd.source_node)
				continue;
			PROFILE_FUNCTION_NAMED("Source Pre-mixing");

			// Skip if listeners still need the previous mix.
			if (sd.pending_consumers > 0)
				continue;

			// Reset completed+consumed mix
			if (sd.mixed_frames_ready >= frame_size) {
				sd.mixed_frames.fill(Vector2(0, 0));
				sd.mixed_frames_ready = 0;
				for (auto &pb : sd.playbacks) {
					pb.num_mixed_in_current_mixed_frames = 0;
				}

				sd.debug_times_mixed += 1;
			}

			// Clean up finished playbacks
			for (int i = (int)sd.playbacks.size() - 1; i >= 0; --i) {
				if (!sd.playbacks[i].playback->is_playing()) {
					sd.playbacks.remove_at(i);
				}
			}

			// quick check if there are any listeners that would actually
			// consume the mixed data (skip mixing otherwise!)
			bool skip_mixing = true;
			for (auto &sld : sd.listener_data) {
				if ((sd.source_node->get_layers() & sld.listener->get_mask()) == 0)
					continue;
				if (sld.out_of_range)
					continue;
				// Check if this listener has active (playing) playbacks
				for (auto &ld : listeners) {
					if (ld.listener == sld.listener) {
						std::lock_guard pb_lock(*ld.playbacks_mutex);
						for (auto &pb : ld.playbacks) {
							if (pb.playback->is_playing()) {
								skip_mixing = false;
								break;
							}
						}
						break;
					}
				}
				if (!skip_mixing)
					break;
			}
			if (skip_mixing)
				continue;

			int prev_mixed_count = sd.mixed_frames_ready;
			int min_frames_ready = frame_size;
			for (auto &pb : sd.playbacks) {
				if (pb.num_mixed_in_current_mixed_frames >= frame_size)
					continue;
				int to_pull = frame_size - pb.num_mixed_in_current_mixed_frames;
				const PackedVector2Array frames = pb.playback->mix_audio(pb.pitch_scale, to_pull);
				pb.debug_num_mixed += frames.size();

				int pulled = MIN((int)frames.size(), to_pull);

				int mixed_index = pb.num_mixed_in_current_mixed_frames;
				for (int frames_index = 0; frames_index < pulled; ++frames_index) {
					if (mixed_index >= frame_size)
						break; // precaution, but should really not happen...
					sd.mixed_frames[mixed_index] += frames[frames_index] * pb.volume_linear;
					++mixed_index;
				}
				min_frames_ready = MIN(min_frames_ready, mixed_index);
				pb.num_mixed_in_current_mixed_frames = mixed_index;
			}
			sd.mixed_frames_ready = min_frames_ready;

			if (!sd.effect_instances.is_empty()) {
				PROFILE_FUNCTION_NAMED("Effect Stack Processing");
				// Apply effect stack in-place on mixed frames
				int num_newly_ready = sd.mixed_frames_ready - prev_mixed_count;
				if (num_newly_ready > 0) {
					PackedVector2Array new_frames(sd.mixed_frames.slice(prev_mixed_count, sd.mixed_frames_ready));
					for (auto &inst : sd.effect_instances) {
						new_frames = inst->process_audio(
								new_frames,
								frame_size);
					}
					int new_index = 0;
					for (int mixed_index = prev_mixed_count; mixed_index < sd.mixed_frames_ready; ++mixed_index) {
						sd.mixed_frames[mixed_index] = new_frames[new_index];
						new_index++;
					}
				}
			}
			if (sd.mixed_frames_ready == frame_size) {
				// Mix complete — count listeners with active playbacks as consumers.
				sd.pending_consumers = 0;
				for (auto &sld : sd.listener_data) {
					if ((sd.source_node->get_layers() & sld.listener->get_mask()) == 0)
						continue;
					if (sld.out_of_range)
						continue;
					// Check if this listener has active (playing) playbacks
					for (auto &ld : listeners) {
						if (ld.listener == sld.listener) {
							std::lock_guard pb_lock(*ld.playbacks_mutex);
							bool has_playing = false;
							for (auto &pb : ld.playbacks) {
								if (pb.playback->is_playing()) {
									has_playing = true;
									break;
								}
							}
							if (has_playing) {
								sd.pending_consumers++;
							}
							break;
						}
					}
				}

				// Calculate dB level for current mix
				float sum_sq = 0.0f;
				for (int i = 0; i < frame_size; ++i) {
					sum_sq += sd.mixed_frames[i].x * sd.mixed_frames[i].x;
					sum_sq += sd.mixed_frames[i].y * sd.mixed_frames[i].y;
				}
				float rms = sqrtf(sum_sq / (frame_size * 2));
				if (rms > 0.000001f) {
					sd.current_db_level = 20.0f * log10f(rms);
				} else {
					sd.current_db_level = -200; // Silenced/Noise floor
				}
			}
		}
	}

	{
		PROFILE_FUNCTION_NAMED("process_audio_phase3");
		// =========================================================================
		// PHASE 3: Sources contribute mixed audio to listener push_buffers.
		// For each listener awaiting contributions, apply SteamAudio effects
		// from ready sources and accumulate into push_buffer.
		// =========================================================================
		for (auto &ld : listeners) {
			if (!ld.listener)
				continue;

			// Skip listeners without active playbacks — they would cycle through
			// generations instantly, consuming source mixes before real listeners can.
			bool has_active_playbacks;
			{
				std::lock_guard pb_lock(*ld.playbacks_mutex);
				// Remove dead playbacks
				for (int i = (int)ld.playbacks.size() - 1; i >= 0; --i) {
					if (!ld.playbacks[i].playback->is_playing()) {
						ld.playbacks.remove_at(i);
					}
				}
				has_active_playbacks = !ld.playbacks.is_empty();
			}

			if (!has_active_playbacks) {
				// No playbacks — release pending_consumers to prevent stalls.
				if (ld.pending_contributors > 0) {
					for (auto &sd : sources) {
						if (!sd.source_node)
							continue;
						if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0)
							continue;
						if (sd.mixed_frames_ready < frame_size)
							continue;
						for (auto &entry : sd.listener_data) {
							if (entry.listener == ld.listener && !entry.out_of_range &&
									entry.last_contributed_generation != ld.generation) {
								if (sd.pending_consumers > 0)
									sd.pending_consumers--;
								entry.last_contributed_generation = ld.generation;
								break;
							}
						}
					}
				}
				ld.pending_contributors = 0;
				ld.pending_drains = 0;
				continue;
			}

			if (ld.pending_contributors <= 0 && ld.pending_drains <= 0) {
				ld.push_buffer.fill(Vector2(0, 0));
				ld.generation++;
				ld.pending_contributors = 0;
				for (auto &sd : sources) {
					if (!sd.source_node)
						continue;
					if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0)
						continue;
					for (auto &sld : sd.listener_data) {
						if (sld.listener == ld.listener && !sld.out_of_range) {
							ld.pending_contributors++;
							break;
						}
					}
				}
			}

			if (ld.pending_contributors <= 0)
				continue;

			PROFILE_FUNCTION_NAMED("Listener mixing");

			for (auto &sd : sources) {
				if (!sd.source_node)
					continue;

				if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0)
					continue;

				if (sd.mixed_frames_ready < frame_size) {
					// Source not ready yet
					continue;
				}

				SourceListenerData *sld = nullptr;
				for (auto &entry : sd.listener_data) {
					if (entry.listener == ld.listener) {
						sld = &entry;
						break;
					}
				}

				if (!sld)
					continue;

				// Already contributed to this generation?
				if (sld->last_contributed_generation == ld.generation)
					continue;

				// Out of range — mark contributed and decrement counters, skip processing.
				if (sld->out_of_range) {
					sld->last_contributed_generation = ld.generation;
					if (ld.pending_contributors > 0)
						ld.pending_contributors--;
					if (sd.pending_consumers > 0)
						sd.pending_consumers--;
					continue;
				}

				// Copy pre-mixed source audio into SteamAudio input buffer
				for (int s = 0; s < frame_size; ++s) {
					Vector2 f = sd.mixed_frames[s];
					sld->input_buffer.data[0][s] = (f.x + f.y) * 0.5f;
				}
				// Mark contributed and decrement counters
				sld->last_contributed_generation = ld.generation;

				sld->debug_times_contributed += 1;

				if (ld.pending_contributors > 0)
					ld.pending_contributors--;
				if (sd.pending_consumers > 0)
					sd.pending_consumers--;

				// Direct effects
				if (sd.source_node->get_direct_enabled() && sld->source && sld->direct_effect) {
					PROFILE_FUNCTION_NAMED("direct effect processing")
					IPLSimulationOutputs outputs{};
					iplSourceGetOutputs(sld->source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);

					IPLDirectEffectParams direct_params = outputs.direct;
					direct_params.flags = static_cast<IPLDirectEffectFlags>(0);

					if (sd.source_node->get_occlusion_enabled()) {
						direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION);
					}

					if (sd.source_node->get_transmission_enabled()) {
						direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
						direct_params.transmissionType = static_cast<IPLTransmissionType>(sd.source_node->get_transmission_type());
					}

					if (sd.source_node->get_air_absorption_enabled()) {
						direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);

						IPLAirAbsorptionModel airAbsorptionModel{};
						airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;

						iplAirAbsorptionCalculate(phonon_context, sd.cached_coords.origin, ld.cached_coords.origin, &airAbsorptionModel, direct_params.airAbsorption);
					}

					if (sd.source_node->get_distance_attenuation_enabled()) {
						direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION);

						float dist = sld->dist_to_listener;
						float min_dist = sd.source_node->get_distance_attenuation_min();
						float max_dist = sd.source_node->get_distance_attenuation_max();

						float attenuation = calculate_attenuation(dist, min_dist, max_dist);
						direct_params.distanceAttenuation = attenuation;
					} else {
						direct_params.distanceAttenuation = 1.0f;
					}

					iplDirectEffectApply(sld->direct_effect, &direct_params, &sld->input_buffer, &sld->input_buffer);

					// Binaural spatialization
					if (sd.source_node->get_binaural_enabled()) {
						IPLBinauralEffectParams params{};

						params.direction = iplCalculateRelativeDirection(phonon_context,
								sd.cached_coords.origin,
								ld.cached_coords.origin,
								ld.cached_coords.ahead,
								ld.cached_coords.up);
						params.interpolation = static_cast<IPLHRTFInterpolation>(sd.source_node->get_binaural_interpolation());
						params.spatialBlend = sd.source_node->get_binaural_spatial_blend();
						params.hrtf = phonon_hrtf;

						if (phonon_hrtf) {
							iplBinauralEffectApply(sld->binaural_effect, &params, &sld->input_buffer, &sld->output_buffer);
						} else {
							for (int s = 0; s < frame_size; ++s) {
								sld->output_buffer.data[0][s] = sld->input_buffer.data[0][s];
								sld->output_buffer.data[1][s] = sld->input_buffer.data[0][s];
							}
						}

						for (int s = 0; s < frame_size; ++s) {
							ld.push_buffer[s].x += sld->output_buffer.data[0][s];
							ld.push_buffer[s].y += sld->output_buffer.data[1][s];
						}
					} else {
						for (int s = 0; s < frame_size; ++s) {
							ld.push_buffer[s].x += sld->input_buffer.data[0][s];
							ld.push_buffer[s].y += sld->input_buffer.data[0][s];
						}
					}
				}

				// Reflections
				if (sd.source_node->get_reflection_enabled() && sld->source && sld->reflection_effect && sld->ambisonics_decode_effect) {
					PROFILE_FUNCTION_NAMED("Reflection Effect Processing");
					IPLSimulationOutputs outputs{};
					iplSourceGetOutputs(sld->source, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);

					IPLReflectionEffectParams refl_params = outputs.reflections;
					refl_params.type = static_cast<IPLReflectionEffectType>(ld.listener->get_refl_type());
					refl_params.numChannels = ambisonic_channels_from(ld.listener->get_refl_ambisonics_order());

					if (outputs.reflections.irSize > 0) {
						iplReflectionEffectApply(sld->reflection_effect, &refl_params, &sld->input_buffer, &sld->ambisonics_buffer, nullptr);

						IPLAmbisonicsDecodeEffectParams decode_params{};
						decode_params.order = ld.listener->get_refl_ambisonics_order();
						decode_params.hrtf = phonon_hrtf;
						decode_params.orientation = ld.cached_coords;
						decode_params.binaural = IPL_TRUE;

						iplAmbisonicsDecodeEffectApply(sld->ambisonics_decode_effect, &decode_params, &sld->ambisonics_buffer, &sld->output_buffer);

						for (int s = 0; s < frame_size; ++s) {
							ld.push_buffer[s].x += sld->output_buffer.data[0][s];
							ld.push_buffer[s].y += sld->output_buffer.data[1][s];
						}
					}
				}
			}

			// All contributors done — start draining push_buffer to playbacks
			if (ld.pending_contributors <= 0) {
				ld.pending_contributors = 0;
				std::lock_guard pb_lock(*ld.playbacks_mutex);

				for (int i = (int)ld.playbacks.size() - 1; i >= 0; --i) {
					if (!ld.playbacks[i].playback->is_playing()) {
						ld.playbacks.remove_at(i);
					}
				}

				ld.pending_drains = 0;
				for (int i = (int)ld.playbacks.size() - 1; i >= 0; --i) {
					auto &pb = ld.playbacks[i];
					pb.remaining_from_push_buffer = frame_size;
					ld.pending_drains++;
					int free_available_in_buffer = pb.playback->get_free_buffer_size();
					if (frame_size <= free_available_in_buffer) {
						pb.playback->push_buffer(ld.push_buffer);
						pb.remaining_from_push_buffer = 0;
						ld.pending_drains--;
						pb.debug_times_drained += 1;
					} else {
						int num_to_push = MIN(frame_size, free_available_in_buffer);
						if (num_to_push > 0) {
							pb.playback->push_buffer(ld.push_buffer.slice(0, num_to_push));
							pb.remaining_from_push_buffer -= num_to_push;
						}
					}
				}

				// All playbacks consumed immediately — clear and recount contributors
				if (ld.pending_drains <= 0) {
					ld.debug_times_pushed += 1;
					ld.pending_drains = 0;
					ld.push_buffer.fill(Vector2(0, 0));
					ld.generation++;

					ld.pending_contributors = 0;
					for (auto &sd : sources) {
						if (!sd.source_node)
							continue;
						if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0)
							continue;
						for (auto &sld : sd.listener_data) {
							if (sld.listener == ld.listener && !sld.out_of_range) {
								ld.pending_contributors++;
								break;
							}
						}
					}
				}
			}
		}
	}
}

void SteamAudioServer::add_listener(SteamAudioListener *listener) {
	PROFILE_FUNCTION();

	// Pre-build ListenerData outside any lock
	ListenerData ld;
	ld.listener = listener;
	ld.dirty = true;
	ld.push_buffer.resize(cached_audio_settings.frameSize);

	// Initialize Simulator for this listener
	IPLSimulationSettings sim_cfg{};
	sim_cfg.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
	sim_cfg.sceneType = static_cast<IPLSceneType>(get_project_int("steamaudio/scene_type", IPL_SCENETYPE_DEFAULT));
	sim_cfg.reflectionType = static_cast<IPLReflectionEffectType>(listener->get_refl_type());
	sim_cfg.maxNumOcclusionSamples = get_project_int("steamaudio/max_occlusion_samples", 64);
	sim_cfg.maxNumRays = listener->get_num_refl_rays();
	sim_cfg.numDiffuseSamples = 32;
	sim_cfg.maxDuration = listener->get_refl_duration();
	sim_cfg.maxOrder = listener->get_refl_ambisonics_order();
	sim_cfg.maxNumSources = 256;
	sim_cfg.numThreads = OS::get_singleton()->get_processor_count();
	sim_cfg.samplingRate = cached_audio_settings.samplingRate;
	sim_cfg.frameSize = cached_audio_settings.frameSize;

	if (handleErr(iplSimulatorCreate(phonon_context, &sim_cfg, &ld.simulator), "SteamAudio: Failed to create simulator for listener")) {
		iplSimulatorSetScene(ld.simulator, phonon_scene);
		iplSimulatorCommit(ld.simulator);
	}

	// Enqueue — cross-references with sources will be created when applied
	{
		std::lock_guard lock(pending_ops_mutex);
		pending_ops.push_back(PendingAddListener{ listener, std::move(ld) });
	}
}

void SteamAudioServer::add_playback_to_listener(SteamAudioListener *listener, godot::Ref<AudioStreamSteamAudioListenerPlayback> playback) {
	PROFILE_FUNCTION();
	std::lock_guard lock(pending_ops_mutex);
	pending_ops.push_back(PendingAddPlaybackToListener{ listener, playback });
}

void SteamAudioServer::remove_listener(SteamAudioListener *listener) {
	PROFILE_FUNCTION();
	std::lock_guard lock(pending_ops_mutex);
	pending_ops.push_back(PendingRemoveListener{ listener });
}

const LocalVector<ListenerSourceDBLevel>& SteamAudioServer::get_source_db_levels_for_listener(SteamAudioListener *listener) {
	for (const auto& ld : listeners) {
		if ( ld.listener == listener )
			return ld.source_db_levels;
	}
	static LocalVector<ListenerSourceDBLevel> empty;
	return empty;
}

void SteamAudioServer::add_source(SteamAudioSource *source_node) {
	PROFILE_FUNCTION();

	// Pre-build SourceData outside any lock
	SourceData sd;
	sd.source_node = source_node;
	sd.mixed_frames.resize(cached_audio_settings.frameSize);
	sd.mixed_frames.fill(Vector2(0,0));

	{
		PROFILE_FUNCTION_NAMED("instantiate_AudioEffects");
		TypedArray<AudioEffect> effects = source_node->get_effect_stack();
		for (int i = 0; i < effects.size(); ++i) {
			Ref<AudioEffect> effect = effects[i];
			if (effect.is_valid()) {
				Ref<AudioEffectInstance> inst = effect->instantiate();
				if (inst.is_valid()) {
					sd.effect_instances.push_back(inst);
				}
			}
		}
	}

	// Enqueue — cross-references with listeners will be created when applied
	{
		std::lock_guard lock(pending_ops_mutex);
		pending_ops.push_back(PendingAddSource{ source_node, std::move(sd) });
	}
}

void SteamAudioServer::add_playback_to_source(const SteamAudioSource *source_node, Ref<AudioStreamPlayback> playback, float p_volume_db, float p_pitch_scale) {
	if (playback.is_null())
		return;

	float volume_linear = std::pow(10.0f, p_volume_db / 20.0f);
	playback->start();

	std::lock_guard lock(pending_ops_mutex);
	pending_ops.push_back(PendingAddPlaybackToSource{ source_node, playback, volume_linear, p_pitch_scale });
}

void SteamAudioServer::set_source_playback_volume(const SteamAudioSource *source_node, Ref<AudioStreamPlayback> p_playback, float p_volume_db) {
	std::shared_lock lock(collections_mutex);
	for (auto &sd : sources) {
		if (sd.source_node == source_node) {
			for (auto &entry : sd.playbacks) {
				if (entry.playback == p_playback) {
					entry.volume_linear = std::pow(10.0f, p_volume_db / 20.0f);
					return;
				}
			}
			// playback not found!
			return;
		}
	}
}

void SteamAudioServer::set_source_playback_pitch(const SteamAudioSource *source_node, Ref<AudioStreamPlayback> p_playback, float p_pitch_scale) {
	std::shared_lock lock(collections_mutex);
	for (auto &sd : sources) {
		if (sd.source_node == source_node) {
			for (auto &entry : sd.playbacks) {
				if (entry.playback == p_playback) {
					entry.pitch_scale = p_pitch_scale;
					return;
				}
			}
			// playback not found!
			return;
		}
	}
}

int SteamAudioServer::source_get_num_active_playbacks(const SteamAudioSource *source_node) {
	std::shared_lock lock(collections_mutex);
	for (auto &sd : sources) {
		if (sd.source_node == source_node)
			return sd.playbacks.size();
	}
	return 0;
}

void SteamAudioServer::remove_source(SteamAudioSource *source_node) {
	PROFILE_FUNCTION();
	std::lock_guard lock(pending_ops_mutex);
	pending_ops.push_back(PendingRemoveSource{ source_node });
}

void SteamAudioServer::add_static_geometry(Node *p_node, Ref<SteamAudioMaterial> p_material) {
	PROFILE_FUNCTION();
	if (!phonon_scene) {
		UtilityFunctions::push_error("Phonon scene is not initialized");
		return;
	}

	std::vector<IPLStaticMesh> meshes;
	if (auto *mi = Object::cast_to<MeshInstance3D>(p_node)) {
		meshes = create_meshes_from_mesh_inst_3d(mi, phonon_scene, p_material);
	} else if (auto *cs = Object::cast_to<CollisionShape3D>(p_node)) {
		meshes = create_meshes_from_coll_inst_3d(cs, phonon_scene, p_material);
	}

	for (auto m : meshes) {
		iplStaticMeshAdd(m, phonon_scene);
	}

	if (!meshes.empty()) {
		StaticGeometryData sg;
		sg.node = p_node;
		sg.meshes = meshes;
		static_geometry.push_back(sg);
		scene_dirty = true;
	}
}

void SteamAudioServer::remove_static_geometry(Node *p_node) {
	PROFILE_FUNCTION();
	for (uint32_t i = 0; i < static_geometry.size(); ++i) {
		if (static_geometry[i].node == p_node) {
			for (auto &m : static_geometry[i].meshes) {
				iplStaticMeshRemove(m, phonon_scene);
				iplStaticMeshRelease(&m);
			}
			static_geometry.remove_at(i);
			scene_dirty = true;
			break;
		}
	}
}

void SteamAudioServer::add_dynamic_geometry(Node *p_node, Ref<SteamAudioMaterial> p_material) {
	PROFILE_FUNCTION();
	if (!phonon_scene || !phonon_context)
		return;

	Node3D *node3d = Object::cast_to<Node3D>(p_node);
	if (!node3d)
		return;

	DynamicGeometryData dg;
	dg.node = node3d;

	IPLSceneSettings sub_scene_cfg{};
	sub_scene_cfg.radeonRaysDevice = nullptr;
	sub_scene_cfg.type = static_cast<IPLSceneType>(get_project_int("steamaudio/scene_type", IPL_SCENETYPE_DEFAULT));
	if (sub_scene_cfg.type == IPL_SCENETYPE_EMBREE) {
		ERR_FAIL_COND_MSG(embree_dev == nullptr, "ERROR: steam audio add_dynamic_geometry with scene_type IPL_SCENETYPE_EMBREE and uninitialized embree device.");
		sub_scene_cfg.embreeDevice = embree_dev;
	}
	if (!handleErr(iplSceneCreate(phonon_context, &sub_scene_cfg, &dg.sub_scene), "SteamAudio: Failed to create sub-scene")) {
		return;
	}

	if (auto *mi = Object::cast_to<MeshInstance3D>(p_node)) {
		dg.meshes = create_meshes_from_mesh_inst_3d(mi, dg.sub_scene, p_material, true);
	} else if (auto *cs = Object::cast_to<CollisionShape3D>(p_node)) {
		dg.meshes = create_meshes_from_coll_inst_3d(cs, dg.sub_scene, p_material, true);
	}

	for (auto m : dg.meshes) {
		iplStaticMeshAdd(m, dg.sub_scene);
	}
	iplSceneCommit(dg.sub_scene);

	IPLInstancedMeshSettings inst_cfg{};
	inst_cfg.subScene = dg.sub_scene;
	inst_cfg.transform = ipl_mat4_from(node3d->get_global_transform());

	handleErr(iplInstancedMeshCreate(phonon_scene, &inst_cfg, &dg.instanced_mesh), "SteamAudio: Failed to create instanced mesh");
	iplInstancedMeshAdd(dg.instanced_mesh, phonon_scene);

	scene_dirty = true;

	dynamic_geometry.push_back(dg);
}

void SteamAudioServer::remove_dynamic_geometry(Node *p_node) {
	PROFILE_FUNCTION();
	for (uint32_t i = 0; i < dynamic_geometry.size(); ++i) {
		if (dynamic_geometry[i].node == p_node) {
			for (auto &m : dynamic_geometry[i].meshes) {
				iplStaticMeshRemove(m, dynamic_geometry[i].sub_scene);
				iplStaticMeshRelease(&m);
			}
			iplInstancedMeshRemove(dynamic_geometry[i].instanced_mesh, phonon_scene);
			iplInstancedMeshRelease(&dynamic_geometry[i].instanced_mesh);
			iplSceneRelease(&dynamic_geometry[i].sub_scene);

			dynamic_geometry.remove_at(i);
			scene_dirty = true;
			break;
		}
	}
}