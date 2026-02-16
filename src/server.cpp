#include "server.hpp"
#include "geometry_common.hpp"
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
	if (v.get_type() == Variant::NIL) return def;
	return int(v);
}

static float get_project_float(const String &key, float def) {
	Variant v = ProjectSettings::get_singleton()->get_setting(key);
	if (v.get_type() == Variant::NIL) return def;
	return float(v);
}

void cleanup_source_listener_data(SourceListenerData &sld, IPLContext ctx) {
	if (sld.source) {
		iplSourceRelease(&sld.source);
	}
	if (sld.binaural_effect) iplBinauralEffectRelease(&sld.binaural_effect);
	if (sld.direct_effect) iplDirectEffectRelease(&sld.direct_effect);
	if (sld.reflection_effect) iplReflectionEffectRelease(&sld.reflection_effect);
	if (sld.ambisonics_decode_effect) iplAmbisonicsDecodeEffectRelease(&sld.ambisonics_decode_effect);
	iplAudioBufferFree(ctx, &sld.input_buffer);
	iplAudioBufferFree(ctx, &sld.output_buffer);
	iplAudioBufferFree(ctx, &sld.ambisonics_buffer);
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
		while (frame_size < target) frame_size <<= 1;
	}
	return IPLAudioSettings{ mix_rate, frame_size };
}

void SteamAudioServer::init() {
	if (is_initialized) return;
	is_initialized = true;

	if (is_running.load()) return;

	IPLContextSettings ctx_cfg{};
	ctx_cfg.version = STEAMAUDIO_VERSION;
	if (!handleErr(iplContextCreate(&ctx_cfg, &phonon_context), "SteamAudio: Failed to create context")) {
		phonon_context = nullptr;
		return;
	}

	IPLSceneSettings scene_cfg{};
	scene_cfg.type = static_cast<IPLSceneType>(get_project_int("steamaudio/scene_type", IPL_SCENETYPE_DEFAULT));
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
	mixing_thread->start(Callable(this, "mixing_thread_func"));
}

void SteamAudioServer::finish() {
	if (!is_running.load()) return;
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
		if (dg.instanced_mesh) iplInstancedMeshRelease(&dg.instanced_mesh);
		for (auto &m : dg.meshes) iplStaticMeshRelease(&m);
		if (dg.sub_scene) iplSceneRelease(&dg.sub_scene);
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
	if (Engine::get_singleton()->is_editor_hint()) return;
	if (!is_running.load()) return;
	PROFILE_FUNCTION();

	std::shared_lock<std::shared_mutex> lock(collections_mutex);

	// Update Dynamic Geometry (only mark dirty if transform changed)
	for (auto &dg : dynamic_geometry) {
		if (!dg.node || !dg.instanced_mesh) continue;
		Transform3D trf = dg.node->get_global_transform();
		if (!trf.is_equal_approx(dg.last_trf)) {
			IPLMatrix4x4 m = ipl_mat4_from(trf);
			iplInstancedMeshUpdateTransform(dg.instanced_mesh, phonon_scene, m);
			dg.last_trf = trf;
			scene_dirty = true;
		}
	}

	for (auto &ld : listeners) {
		if (!ld.listener) continue;
		if (!ld.simulator) continue;

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
			if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0) continue;

			// Find or create the SourceListenerData for this listener
			SourceListenerData *sld = nullptr;
			for (auto &entry : sd.listener_data) {
				if (entry.listener == ld.listener) {
					sld = &entry;
					break;
				}
			}
			if (!sld) continue;

			// Create IPLSource per SourceListenerData if needed
			if (!sld->source) {
				IPLSourceSettings source_settings{};
				source_settings.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
				if (handleErr(iplSourceCreate(ld.simulator, &source_settings, &sld->source), "SteamAudio: Failed to create source")) {
					iplSourceAdd(sld->source, ld.simulator);
					ld.dirty = true;
				}
			}

			Transform3D src_trf = sd.source_node->get_global_transform();
			if (!src_trf.is_equal_approx(sd.last_trf)) {
				sd.cached_coords = ipl_coords_from(src_trf);

 				// Update dist_to_listener and doppler
				float prev_dist_to_listener = sld->dist_to_listener;
				sld->dist_to_listener = src_trf.origin.distance_to(ld.last_trf.origin);

				float doppler_factor = sd.source_node->get_doppler_factor();
				// skip the very first update (dist is 0 and doppler_pitch is 1) so that we don't get
				// enormous speeds
				if (doppler_factor > 0.0f && prev_dist_to_listener != 0.0f && sld->doppler_pitch != 1.0f) {
					if (delta > 0.0) {
						float radial_speed = (sld->dist_to_listener - prev_dist_to_listener) * (float)delta;
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

				IPLSimulationInputs inputs{};
				inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
				if (sd.source_node->get_reflection_enabled()) {
					inputs.flags = static_cast<IPLSimulationFlags>(inputs.flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
				}

				inputs.directFlags = static_cast<IPLDirectSimulationFlags>(0);
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

				inputs.source = sd.cached_coords;
				inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
				inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;

				inputs.occlusionType = static_cast<IPLOcclusionType>(sd.source_node->get_occlusion_type());
				inputs.occlusionRadius = sd.source_node->get_occlusion_radius();
				inputs.numOcclusionSamples = sd.source_node->get_occlusion_samples();
				inputs.numTransmissionRays = sd.source_node->get_transmission_rays();

				for (int i = 0; i < IPL_NUM_BANDS; ++i) inputs.reverbScale[i] = 1.0f;
				inputs.hybridReverbTransitionTime = 1.0f;
				inputs.hybridReverbOverlapPercent = 0.25f;
				inputs.baked = IPL_FALSE;

				iplSourceSetInputs(sld->source, static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS), &inputs);
				sd.last_trf = src_trf;
			}
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
		if (!ld.simulator) continue;
		iplSimulatorRunDirect(ld.simulator);
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

			if (!is_running.load()) break;
		}

		{
			is_refl_thread_processing.store(true);
			new_inputs_set.store(false);

			PROFILE_FUNCTION_NAMED("refl_sim");
			// this lock might be problematic. the iplSimulatorRunReflections
			// call can take a long time and the unique_lock when adding/removing
			// a listener has to wait for that... But might be ok to wait 300ms
			// for an event that doesn't happen often.
			std::shared_lock<std::shared_mutex> lock(collections_mutex);
			for (auto &ld : listeners) {
				if (!ld.simulator) continue;
				iplSimulatorRunReflections(ld.simulator);
			}

			is_refl_thread_processing.store(false);
		}
	}
}

void SteamAudioServer::mixing_thread_func() {
	while (is_running.load()) {
		process_audio();
		OS::get_singleton()->delay_msec(1);
	}
}

void SteamAudioServer::process_audio() {
	PROFILE_FUNCTION();
	const int frame_size = cached_audio_settings.frameSize;

	std::shared_lock<std::shared_mutex> lock(collections_mutex);

	for (auto &ld : listeners) {
		if (!ld.listener) continue;
		Ref<AudioStreamGeneratorPlayback> playback = ld.listener->get_generator_playback();
		if (playback.is_null()) continue;

		if (playback->get_frames_available() < frame_size) continue;

		// Clear listener mix buffer
		for (int i = 0; i < frame_size; ++i) {
			ld.mix_buffer[i].left = 0.0f;
			ld.mix_buffer[i].right = 0.0f;
		}

		for (auto &sd : sources) {
			if (!sd.source_node) continue;

			if ((sd.source_node->get_layers() & ld.listener->get_mask()) == 0) continue;

			SourceListenerData *sld = nullptr;
			for (auto &entry : sd.listener_data) {
				if (entry.listener == ld.listener) {
					sld = &entry;
					break;
				}
			}

			if (!sld || !sld->binaural_effect || sld->out_of_range) continue;

			// Pull PCM from source playbacks
			for (int s = 0; s < frame_size; ++s) sld->input_buffer.data[0][s] = 0.0f;

			{
				std::lock_guard<std::mutex> pb_lock(sd.source_node->get_playbacks_mutex());
				auto &playbacks = sd.source_node->get_playbacks();

 				// Clean up finished playbacks
				for (int i = 0; i < (int)playbacks.size(); ++i) {
					if (!playbacks[i].playback->is_playing()) {
						playbacks.remove_at(i);
						i--;
					}
				}

				for (auto &pb : playbacks) {
					float effective_pitch = pb.pitch_scale * sld->doppler_pitch;
 					const PackedVector2Array frames = pb.playback->mix_audio(effective_pitch, frame_size);
					int pulled = MIN((int)frames.size(), frame_size);

					for (int s = 0; s < pulled; ++s) {
						Vector2 v = frames[s];
						sld->input_buffer.data[0][s] += (v.x + v.y) * 0.5f * pb.volume_linear;
					}
				}
			}

			// Apply Direct Effects
			if (sld->source && sld->direct_effect) {
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
			}

			// Apply Binaural Spatialization
			if (sd.source_node->get_binaural_enabled()) {
				IPLBinauralEffectParams params{};

				params.direction = iplCalculateRelativeDirection(phonon_context,
					sd.cached_coords.origin,
					ld.cached_coords.origin,
					ld.cached_coords.ahead,
					ld.cached_coords.up);
				params.interpolation = static_cast<IPLHRTFInterpolation>(sd.source_node->get_binaural_interpolation());
				params.spatialBlend = 1.0f;
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
					ld.mix_buffer[s].left += sld->output_buffer.data[0][s];
					ld.mix_buffer[s].right += sld->output_buffer.data[1][s];
				}
			} else {
				for (int s = 0; s < frame_size; ++s) {
					ld.mix_buffer[s].left += sld->input_buffer.data[0][s];
					ld.mix_buffer[s].right += sld->input_buffer.data[0][s];
				}
			}

			// Apply Reflections
			if (sd.source_node->get_reflection_enabled() && sld->source && sld->reflection_effect && sld->ambisonics_decode_effect) {
				IPLSimulationOutputs outputs{};
				iplSourceGetOutputs(sld->source, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);

				IPLReflectionEffectParams refl_params = outputs.reflections;
				refl_params.type = static_cast<IPLReflectionEffectType>(sd.source_node->get_reflection_type());
				refl_params.numChannels = ambisonic_channels_from(ld.listener->get_refl_ambisonics_order());
				refl_params.irSize = int(sd.source_node->get_reflection_duration() * cached_audio_settings.samplingRate);
				refl_params.delay = sd.source_node->get_reflection_hybrid_delay();

				if (outputs.reflections.irSize > 0) {
					iplReflectionEffectApply(sld->reflection_effect, &refl_params, &sld->input_buffer, &sld->ambisonics_buffer, nullptr);

					IPLAmbisonicsDecodeEffectParams decode_params{};
					decode_params.order = ld.listener->get_refl_ambisonics_order();
					decode_params.hrtf = phonon_hrtf;
					decode_params.orientation = ld.cached_coords;
					decode_params.binaural = IPL_TRUE;

					iplAmbisonicsDecodeEffectApply(sld->ambisonics_decode_effect, &decode_params, &sld->ambisonics_buffer, &sld->output_buffer);

					for (int s = 0; s < frame_size; ++s) {
						ld.mix_buffer[s].left += sld->output_buffer.data[0][s];
						ld.mix_buffer[s].right += sld->output_buffer.data[1][s];
					}
				}
			}
		}

		// Push to Generator
		for (int i = 0; i < frame_size; ++i) {
			ld.push_buffer[i] = Vector2(ld.mix_buffer[i].left, ld.mix_buffer[i].right);
		}
		playback->push_buffer(ld.push_buffer);
	}
}

void SteamAudioServer::add_listener(SteamAudioListener *listener) {
	PROFILE_FUNCTION();
	std::unique_lock<std::shared_mutex> lock(collections_mutex);

	for (const auto &ld : listeners) {
		if (ld.listener == listener) return;
	}

	ListenerData ld;
	ld.listener = listener;
	ld.dirty = true;

	ld.mix_buffer.resize(cached_audio_settings.frameSize);
	ld.push_buffer.resize(cached_audio_settings.frameSize);

	// Create per-listener state for all existing sources
	for (auto &sd : sources) {
		SourceListenerData sld;
		sld.listener = listener;

		IPLBinauralEffectSettings binaural_cfg{};
		binaural_cfg.hrtf = phonon_hrtf;
		if (handleErr(iplBinauralEffectCreate(phonon_context, &cached_audio_settings, &binaural_cfg, &sld.binaural_effect), "SteamAudio: Failed to create binaural effect")) {
			IPLDirectEffectSettings direct_cfg{};
			direct_cfg.numChannels = 1;
			handleErr(iplDirectEffectCreate(phonon_context, &cached_audio_settings, &direct_cfg, &sld.direct_effect), "SteamAudio: Failed to create direct effect");

			iplAudioBufferAllocate(phonon_context, 1, cached_audio_settings.frameSize, &sld.input_buffer);
			iplAudioBufferAllocate(phonon_context, 2, cached_audio_settings.frameSize, &sld.output_buffer);

			int max_order = listener->get_refl_ambisonics_order();
			int num_channels = ambisonic_channels_from(max_order);

			IPLReflectionEffectSettings refl_cfg{};
			refl_cfg.type = static_cast<IPLReflectionEffectType>(sd.source_node->get_reflection_type());
			refl_cfg.numChannels = num_channels;
			refl_cfg.irSize = int(sd.source_node->get_reflection_duration() * cached_audio_settings.samplingRate);
			handleErr(iplReflectionEffectCreate(phonon_context, &cached_audio_settings, &refl_cfg, &sld.reflection_effect), "SteamAudio: Failed to create reflection effect");

			IPLAmbisonicsDecodeEffectSettings decode_cfg{};
			decode_cfg.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
			decode_cfg.hrtf = phonon_hrtf;
			decode_cfg.maxOrder = max_order;
			handleErr(iplAmbisonicsDecodeEffectCreate(phonon_context, &cached_audio_settings, &decode_cfg, &sld.ambisonics_decode_effect), "SteamAudio: Failed to create ambisonics decode effect");

			iplAudioBufferAllocate(phonon_context, num_channels, cached_audio_settings.frameSize, &sld.ambisonics_buffer);

			sd.listener_data.push_back(sld);
		}
	}

	// Initialize Simulator for this listener
	IPLSimulationSettings sim_cfg{};
	sim_cfg.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
	sim_cfg.sceneType = static_cast<IPLSceneType>(get_project_int("steamaudio/scene_type", IPL_SCENETYPE_DEFAULT));
	sim_cfg.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
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

	listeners.push_back(ld);
}

void SteamAudioServer::remove_listener(SteamAudioListener *listener) {
	PROFILE_FUNCTION();
	std::unique_lock<std::shared_mutex> lock(collections_mutex);

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

	for (uint32_t i = 0; i < listeners.size(); ++i) {
		if (listeners[i].listener == listener) {
			if (listeners[i].simulator) {
				iplSimulatorRelease(&listeners[i].simulator);
			}
			listeners.remove_at(i);
			break;
		}
	}
}

void SteamAudioServer::add_source(SteamAudioSource *source_node) {
	PROFILE_FUNCTION();
	std::unique_lock<std::shared_mutex> lock(collections_mutex);

	for (const auto &sd : sources) {
		if (sd.source_node == source_node) return;
	}

	SourceData sd;
	sd.source_node = source_node;

	// Initialize effects and buffers for all existing listeners
	for (const auto &ld : listeners) {
		SourceListenerData sld;
		sld.listener = ld.listener;

		IPLBinauralEffectSettings binaural_cfg{};
		binaural_cfg.hrtf = phonon_hrtf;
		if (handleErr(iplBinauralEffectCreate(phonon_context, &cached_audio_settings, &binaural_cfg, &sld.binaural_effect), "SteamAudio: Failed to create binaural effect")) {
			IPLDirectEffectSettings direct_cfg{};
			direct_cfg.numChannels = 1;
			handleErr(iplDirectEffectCreate(phonon_context, &cached_audio_settings, &direct_cfg, &sld.direct_effect), "SteamAudio: Failed to create direct effect");

			iplAudioBufferAllocate(phonon_context, 1, cached_audio_settings.frameSize, &sld.input_buffer);
			iplAudioBufferAllocate(phonon_context, 2, cached_audio_settings.frameSize, &sld.output_buffer);

			int max_order = ld.listener->get_refl_ambisonics_order();
			int num_channels = ambisonic_channels_from(max_order);

			IPLReflectionEffectSettings refl_cfg{};
			refl_cfg.type = static_cast<IPLReflectionEffectType>(source_node->get_reflection_type());
			refl_cfg.numChannels = num_channels;
			refl_cfg.irSize = int(source_node->get_reflection_duration() * cached_audio_settings.samplingRate);
			handleErr(iplReflectionEffectCreate(phonon_context, &cached_audio_settings, &refl_cfg, &sld.reflection_effect), "SteamAudio: Failed to create reflection effect");

			IPLAmbisonicsDecodeEffectSettings decode_cfg{};
			decode_cfg.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
			decode_cfg.hrtf = phonon_hrtf;
			decode_cfg.maxOrder = max_order;
			handleErr(iplAmbisonicsDecodeEffectCreate(phonon_context, &cached_audio_settings, &decode_cfg, &sld.ambisonics_decode_effect), "SteamAudio: Failed to create ambisonics decode effect");

			iplAudioBufferAllocate(phonon_context, num_channels, cached_audio_settings.frameSize, &sld.ambisonics_buffer);

			sd.listener_data.push_back(sld);
		}
	}

	sources.push_back(sd);
}

void SteamAudioServer::remove_source(SteamAudioSource *source_node) {
	PROFILE_FUNCTION();
	std::unique_lock<std::shared_mutex> lock(collections_mutex);

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
		std::unique_lock<std::shared_mutex> lock(collections_mutex);
		static_geometry.push_back(sg);
		scene_dirty = true;
	}
}

void SteamAudioServer::remove_static_geometry(Node *p_node) {
	PROFILE_FUNCTION();
	std::unique_lock<std::shared_mutex> lock(collections_mutex);
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
	if (!phonon_scene || !phonon_context) return;

	Node3D *node3d = Object::cast_to<Node3D>(p_node);
	if (!node3d) return;

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

	std::unique_lock<std::shared_mutex> lock(collections_mutex);
	dynamic_geometry.push_back(dg);
}

void SteamAudioServer::remove_dynamic_geometry(Node *p_node) {
	PROFILE_FUNCTION();
	std::unique_lock<std::shared_mutex> lock(collections_mutex);
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