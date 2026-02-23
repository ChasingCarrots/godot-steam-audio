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
	if (!ps->has_setting("steamaudio/reflections/max_rays"))
		ps->set_setting("steamaudio/reflections/max_rays", 512);
	if (!ps->has_setting("steamaudio/reflections/num_bounces"))
		ps->set_setting("steamaudio/reflections/num_bounces", 1);
	if (!ps->has_setting("steamaudio/scene_type"))
		ps->set_setting("steamaudio/scene_type", IPL_SCENETYPE_DEFAULT);
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
		IPLEmbreeDevice embree_dev;
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

	PROFILING_PLOT_NUMBER("NumSteamListeners", (int64_t)listeners.size());
	int total_number_of_active_sources = 0;
	int total_reflection_sources = 0;
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
			total_number_of_active_sources += 1;

			IPLSimulationInputs inputs{};
			inputs.flags = static_cast<IPLSimulationFlags>(0);
			if (sd.source_node->get_direct_enabled()) {
				inputs.flags = static_cast<IPLSimulationFlags>(inputs.flags | IPL_SIMULATIONFLAGS_DIRECT);
			}
			if (sd.source_node->get_reflection_enabled()) {
				total_reflection_sources += 1;
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

	if (scene_dirty) {
		refl_thread_wait_for_commit.store(true);
	}

	if (refl_thread_wait_for_commit.load() && !is_refl_thread_processing.load()) {
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

	for (auto &ld : listeners) {
		if (!ld.simulator)
			continue;
		iplSimulatorRunDirect(ld.simulator);
	}

	new_inputs_set.store(true);
	if (!is_refl_thread_processing.load()) {
		std::unique_lock<std::mutex> lock_refl(refl_mux);
		refl_cv.notify_one();
	}
	PROFILING_PLOT_NUMBER("NumActiveSteamAudioSources", (int64_t)total_number_of_active_sources);
	PROFILING_PLOT_NUMBER("NumActiveSteamAudioReflectionSources", (int64_t)total_reflection_sources);
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
			// this lock might be problematic. the iplSimulatorRunReflections
			// call can take a long time and the unique_lock when adding/removing
			// a listener has to wait for that... But might be ok to wait 300ms
			// for an event that doesn't happen often.
			std::shared_lock lock(collections_mutex);
			for (auto &ld : listeners) {
				if (!ld.simulator)
					continue;
				iplSimulatorRunReflections(ld.simulator);
			}

			is_refl_thread_processing.store(false);
		}
	}
}

void SteamAudioServer::mixing_thread_func() {
	using clock = std::chrono::steady_clock;
	auto wait_time = std::chrono::milliseconds(
		5
	);
	auto spin_threshold = std::chrono::milliseconds(1);
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

void SteamAudioServer::process_audio() {
	PROFILE_FUNCTION();
	const int frame_size = cached_audio_settings.frameSize;

	std::shared_lock lock(collections_mutex);

	// Pre-mix source playbacks so that mix_audio is called only once per source,
	// rather than once per listener (mix_audio consumes the buffer).
	// If mixed_frames is still populated from a previous call (no listener
	// consumed it yet), skip fetching to avoid losing audio data.
	int total_num_playbacks = 0;
	for (auto &sd : sources) {
		if (!sd.source_node)
			continue;
		PROFILE_FUNCTION_NAMED("Source Pre-mixing");

		bool all_listeners_consumed_current_mix = true;
		for (const auto& sld : sd.listener_data) {
			if ((sd.source_node->get_layers() & sld.listener->get_mask()) == 0)
				continue;
			if (sld.pushed_to_listener_buffer || sld.out_of_range)
				continue;
			if (!sld.consumed_source_mix) {
				all_listeners_consumed_current_mix = false;
				break;
			}
		}

		if (all_listeners_consumed_current_mix) {
			sd.mixed_frames.fill(Vector2(0,0));
			sd.mixed_frames_ready = 0;
		}
		else {
			// we can't create a new mix, since there are still listeners
			// wanting to consume our old mix.
			continue;
		}

		// Does the mixed_frames buffer have space?
		if (sd.mixed_frames_ready >= frame_size)
			continue;

		{
			// Clean up finished playbacks
			for (int i = (int)sd.playbacks.size() -1; i >= 0; --i) {
				if (!sd.playbacks[i].playback->is_playing()) {
					sd.playbacks.remove_at(i);
				}
			}
		}

		int num_already_ready_before = sd.mixed_frames_ready;
		{
			int pull_num_frames = frame_size - sd.mixed_frames_ready;
			int min_frames_ready = frame_size;
			for (auto &pb : sd.playbacks) {
				total_num_playbacks += 1;
				if (pb.num_mixed_too_much_last_round >= pull_num_frames)
					continue;
				int to_pull = pull_num_frames - pb.num_mixed_too_much_last_round;
				const PackedVector2Array frames = pb.playback->mix_audio(pb.pitch_scale, to_pull);
				int pulled = MIN((int)frames.size(), to_pull);
				if (pulled != to_pull) {
					UtilityFunctions::print("playback should have pulled ", to_pull, " but got ", pulled);
				}

				int mixed_index = sd.mixed_frames_ready + pb.num_mixed_too_much_last_round;
				for (int frames_index = 0; frames_index < pulled; ++frames_index) {
					sd.mixed_frames[mixed_index] += frames[frames_index] * pb.volume_linear;
					++mixed_index;
				}
				min_frames_ready = MIN(min_frames_ready, mixed_index);
				// we save the number of our frames in the mixed_frames array
				// temporarily in the pb.num_mixed_too_much_last_round (the real
				// number will then be calculated in a second for loop!)
				pb.num_mixed_too_much_last_round = mixed_index;
			}
			sd.mixed_frames_ready = min_frames_ready;
			for (auto &pb : sd.playbacks) {
				pb.num_mixed_too_much_last_round -= min_frames_ready;
				if (pb.num_mixed_too_much_last_round > 0) {
					UtilityFunctions::print("num mixed too much: ", pb.num_mixed_too_much_last_round);
				}
				else if (pb.num_mixed_too_much_last_round < 0) {
					UtilityFunctions::print("num_mixed_too_much was < 0! value: ", pb.num_mixed_too_much_last_round);
					pb.num_mixed_too_much_last_round = 0;
				}
			}
		}

		if (!sd.effect_instances.is_empty()) {
			PROFILE_FUNCTION_NAMED("Effect Stack Processing");
			// Apply effect stack in-place on mixed frames
			int num_newly_ready = sd.mixed_frames_ready - num_already_ready_before;
			if (num_newly_ready > 0) {
				PackedVector2Array new_frames(sd.mixed_frames.slice(num_already_ready_before, sd.mixed_frames_ready));
				for (auto &inst : sd.effect_instances) {
					new_frames = inst->process_audio(
							new_frames,
							frame_size);
				}
				int new_index = 0;
				for (int mixed_index = num_already_ready_before; mixed_index < sd.mixed_frames_ready; ++mixed_index) {
					sd.mixed_frames[mixed_index] = new_frames[new_index];
					new_index++;
				}
			}
		}
		if (sd.mixed_frames_ready == frame_size) {
			// all sources are mixed! mark the listeners as not having consumed the mix
			for (auto& sld : sd.listener_data) {
				if ((sd.source_node->get_layers() & sld.listener->get_mask()) == 0)
					continue;
				sld.consumed_source_mix = false;
			}
		}
	}
	PROFILING_PLOT_NUMBER("NumActiveSteamAudioPlaybacks", (int64_t)total_num_playbacks);

	for (auto &ld : listeners) {
		if (!ld.listener)
			continue;

		PROFILE_FUNCTION_NAMED("Listener mixing");
		// Lock playbacks and check if there are any active ones
		std::lock_guard pb_lock(*ld.playbacks_mutex);

		// Remove playbacks that are no longer playing and
		// push remains from the push_buffer (we only want to fill
		// the push_buffer if no playback needs this data anymore!)
		bool all_playbacks_done_with_pushbuffer = true;
		for (int i = (int)ld.playbacks.size() -1; i >= 0; --i) {
			if (!ld.playbacks[i].playback->is_playing()) {
				ld.playbacks.remove_at(i);
			}
			else if (ld.push_buffer_ready && ld.playbacks[i].remaining_from_push_buffer > 0) {
				auto& pb = ld.playbacks[i];
				if (pb.playback->can_push_buffer(pb.remaining_from_push_buffer)) {
					pb.playback->push_buffer(ld.push_buffer.slice(frame_size - pb.remaining_from_push_buffer));
					pb.remaining_from_push_buffer = 0;
				}
				else {
					int num_to_push = MIN(pb.remaining_from_push_buffer, pb.playback->get_frames_available());
					if (num_to_push > 0) {
						int start = frame_size - pb.remaining_from_push_buffer;
						pb.playback->push_buffer(ld.push_buffer.slice(start, start + num_to_push));
						pb.remaining_from_push_buffer -= num_to_push;
					}
					all_playbacks_done_with_pushbuffer = false;
				}
			}
		}

		if (ld.push_buffer_ready && all_playbacks_done_with_pushbuffer) {
			ld.push_buffer.fill(Vector2(0,0));
			ld.push_buffer_ready = false;
			// reset the pushed_to_listener_buffer flag
			for (auto &sd : sources) {
				for (auto &entry : sd.listener_data) {
					if (entry.listener == ld.listener) {
						entry.pushed_to_listener_buffer = false;
					}
				}
			}
		}

		if (!ld.push_buffer_ready) {
			// fill the push_buffer from relevant and ready sources
			// the push_buffer_ready flag will be cleared by sources
			// that are not ready.
			ld.push_buffer_ready = true;
			for (auto &sd : sources) {
				if (!sd.source_node)
					continue;

				if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0)
					continue;

				if (sd.mixed_frames_ready < frame_size) {
					// this source is relevant, but it is not ready!
					ld.push_buffer_ready = false;
					continue;
				}

				SourceListenerData *sld = nullptr;
				for (auto &entry : sd.listener_data) {
					if (entry.listener == ld.listener) {
						sld = &entry;
						break;
					}
				}

				if (!sld || sld->pushed_to_listener_buffer || sld->out_of_range)
					continue;

				// Use pre-mixed source audio frames
				for (int s = 0; s < frame_size; ++s) {
					Vector2 f = sd.mixed_frames[s];
					sld->input_buffer.data[0][s] = (f.x + f.y) * 0.5f;
				}
				// we consumed the sourcedata mixed_frames, we mark that, so that new data can be created
				sld->consumed_source_mix = true;

				// Apply Direct Effects
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
						direct_params.transmission[0] = sd.source_node->get_transmission_low();
						direct_params.transmission[1] = sd.source_node->get_transmission_med();
						direct_params.transmission[2] = sd.source_node->get_transmission_high();
					}

					if (sd.source_node->get_air_absorption_enabled()) {
						direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
						direct_params.airAbsorption[0] = sd.source_node->get_air_absorption_low();
						direct_params.airAbsorption[1] = sd.source_node->get_air_absorption_med();
						direct_params.airAbsorption[2] = sd.source_node->get_air_absorption_high();
					}

					if (sd.source_node->get_distance_attenuation_enabled()) {
						direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION);

						float dist = sld->dist_to_listener;
						float min_dist = sd.source_node->get_distance_attenuation_min();
						float max_dist = sd.source_node->get_distance_attenuation_max();

						float attenuation = 1.0f - CLAMP(Math::inverse_lerp(min_dist, max_dist, dist), 0.0f, 1.0f);
						direct_params.distanceAttenuation = attenuation * attenuation;
					} else {
						direct_params.distanceAttenuation = 1.0f;
					}

					iplDirectEffectApply(sld->direct_effect, &direct_params, &sld->input_buffer, &sld->input_buffer);

					// Apply Binaural Spatialization for direct audio
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

				// Apply Reflections
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

				sld->pushed_to_listener_buffer = true;
			}

			// the push_buffer wasn't ready, but now is. so we go through all playbacks again
			// and prepare them to receive the push_buffer (and try to push what we can)
			if (ld.push_buffer_ready) {
				bool all_playbacks_done_with_pushbuffer = true;
				for (int i = (int)ld.playbacks.size() -1; i >= 0; --i) {
					auto& pb = ld.playbacks[i];
					pb.remaining_from_push_buffer = frame_size;
					if (pb.playback->can_push_buffer(frame_size)) {
						pb.playback->push_buffer(ld.push_buffer);
						pb.remaining_from_push_buffer = 0;
					}
					else {
						int num_to_push = MIN(frame_size, pb.playback->get_frames_available());
						if (num_to_push > 0) {
							pb.playback->push_buffer(ld.push_buffer.slice(0, num_to_push));
							pb.remaining_from_push_buffer -= num_to_push;
						}
						all_playbacks_done_with_pushbuffer = false;
					}
				}

				// unlikely, but it can happen that we directly pushed the whole buffer to all playbacks
				if (all_playbacks_done_with_pushbuffer) {
					ld.push_buffer.fill(Vector2(0,0));
					ld.push_buffer_ready = false;
				}
			}
		}

	}
}

void SteamAudioServer::add_listener(SteamAudioListener *listener) {
	PROFILE_FUNCTION();

	// but a unique lock for adding listeners to the list of listeners
	std::unique_lock lock(collections_mutex);
	ListenerData ld;
	ld.listener = listener;
	ld.dirty = true;

	ld.push_buffer.resize(cached_audio_settings.frameSize);

	// Create per-listener state for all existing sources
	for (auto &sd : sources) {
		SourceListenerData sld;
		if (create_source_listener_data(sld, sd.source_node, listener, phonon_context, &cached_audio_settings, phonon_hrtf)) {
			sd.listener_data.push_back(sld);
		}
	}

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

	listeners.push_back(std::move(ld));
}

void SteamAudioServer::add_playback_to_listener(SteamAudioListener *listener, godot::Ref<godot::AudioStreamGeneratorPlayback> playback) {
	// we only need a shared lock for the adding of playbacks to the listener
	std::shared_lock lock(collections_mutex);
	for (auto &ld : listeners) {
		if (ld.listener == listener) {
			std::lock_guard<std::mutex> pb_lock(*ld.playbacks_mutex);
			ld.playbacks.push_back({
				playback, 0
			});
			return;
		}
	}
}

void SteamAudioServer::remove_listener(SteamAudioListener *listener) {
	PROFILE_FUNCTION();
	std::unique_lock lock(collections_mutex);

	// Clean up per-listener state in all sources
	for (auto &sd : sources) {
		for (uint32_t i = 0; i < sd.listener_data.size(); ++i) {
			if (sd.listener_data[i].listener == listener) {
				SourceListenerData &sld = sd.listener_data[i];
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
}

void SteamAudioServer::add_source(SteamAudioSource *source_node) {
	PROFILE_FUNCTION();
	std::unique_lock lock(collections_mutex);

	for (const auto &sd : sources) {
		if (sd.source_node == source_node)
			return;
	}

	SourceData sd;
	sd.source_node = source_node;
	sd.mixed_frames.resize(cached_audio_settings.frameSize);
	sd.mixed_frames.fill(Vector2(0,0));

	// Instantiate AudioEffectInstances from the source's effect stack
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

	// Initialize effects and buffers for all existing listeners
	for (const auto &ld : listeners) {
		SourceListenerData sld;
		if (create_source_listener_data(sld, source_node, ld.listener, phonon_context, &cached_audio_settings, phonon_hrtf)) {
			sd.listener_data.push_back(sld);
		}
	}

	sources.push_back(sd);
}

void SteamAudioServer::add_playback_to_source(const SteamAudioSource *source_node, Ref<AudioStreamPlayback> playback, float p_volume_db, float p_pitch_scale) {
	if (playback.is_null())
		return;

	std::unique_lock lock(collections_mutex);
	for (auto &sd : sources) {
		if (sd.source_node == source_node) {
			SourcePlaybackEntry entry;
			entry.playback = playback;
			entry.volume_linear = std::pow(10.0f, p_volume_db / 20.0f);
			entry.pitch_scale = p_pitch_scale;
			playback->start();
			sd.playbacks.push_back(entry);
			return;
		}
	}
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
	std::unique_lock lock(collections_mutex);

	for (uint32_t i = 0; i < sources.size(); ++i) {
		if (sources[i].source_node == source_node) {
			SourceData &sd = sources[i];
			for (auto &sld : sd.listener_data) {
				// Remove source from its simulator
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
		std::unique_lock lock(collections_mutex);
		static_geometry.push_back(sg);
		scene_dirty = true;
	}
}

void SteamAudioServer::remove_static_geometry(Node *p_node) {
	PROFILE_FUNCTION();
	std::unique_lock lock(collections_mutex);
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
	sub_scene_cfg.type = static_cast<IPLSceneType>(get_project_int("steamaudio/scene_type", IPL_SCENETYPE_DEFAULT));
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

	std::unique_lock lock(collections_mutex);
	dynamic_geometry.push_back(dg);
}

void SteamAudioServer::remove_dynamic_geometry(Node *p_node) {
	PROFILE_FUNCTION();
	std::unique_lock lock(collections_mutex);
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