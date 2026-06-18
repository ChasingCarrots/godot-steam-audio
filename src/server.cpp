#include "server.hpp"
#include "godot_cpp/classes/audio_effect.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/os.hpp"
#include "godot_cpp/classes/project_settings.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/memory.hpp"
#include "godot_cpp/classes/worker_thread_pool.hpp"
#include "godot_cpp/variant/array.hpp"
#include "godot_cpp/variant/dictionary.hpp"
#include "godot_cpp/variant/utility_functions.hpp"
#include <phonon.h>
#include <climits>

#include "profiling.h"
#include "steam_audio.hpp"

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

// Build an IPLStaticMesh from raw triangle data (no Godot node knowledge). The
// vertices are expected pre-transformed into the desired space. Triangles are
// given as a flat index array (3 indices per triangle), Godot CW winding (it is
// flipped to IPL CCW here).
static IPLStaticMesh create_ipl_mesh_from_raw(IPLScene scene, const PackedVector3Array &verts, const PackedInt32Array &tris, const IPLMaterial &material) {
	int num_verts = (int)verts.size();
	int num_tris = (int)tris.size() / 3;
	if (num_verts == 0 || num_tris == 0)
		return nullptr;

	std::vector<IPLVector3> ipl_verts(num_verts);
	std::vector<IPLTriangle> ipl_tris(num_tris);
	std::vector<IPLint32> ipl_mat_indices(num_tris);

	for (int j = 0; j < num_verts; j++) {
		ipl_verts[j] = ipl_vec3_from(verts[j]);
	}
	for (int j = 0; j < num_tris * 3; j += 3) {
		// godot tris are cw, ipl tris are ccw
		ipl_tris[j / 3].indices[0] = tris[j];
		ipl_tris[j / 3].indices[1] = tris[j + 2];
		ipl_tris[j / 3].indices[2] = tris[j + 1];
		ipl_mat_indices[j / 3] = 0;
	}

	IPLMaterial mats[1] = { material };
	IPLStaticMeshSettings static_mesh_cfg{};
	static_mesh_cfg.numVertices = num_verts;
	static_mesh_cfg.numTriangles = num_tris;
	static_mesh_cfg.numMaterials = 1;
	static_mesh_cfg.vertices = ipl_verts.data();
	static_mesh_cfg.triangles = ipl_tris.data();
	static_mesh_cfg.materialIndices = ipl_mat_indices.data();
	static_mesh_cfg.materials = mats;

	IPLStaticMesh ipl_mesh = nullptr;
	handleErr(iplStaticMeshCreate(scene, &static_mesh_cfg, &ipl_mesh), "Failed to create static mesh");
	return ipl_mesh;
}

static IPLMaterial ipl_material_from_floats(const PackedFloat32Array &m) {
	IPLMaterial mat{ { 0.f, 0.f, 0.f }, 0.f, { 0.f, 0.f, 0.f } };
	if (m.size() >= 7) {
		mat.absorption[0] = m[0];
		mat.absorption[1] = m[1];
		mat.absorption[2] = m[2];
		mat.scattering = m[3];
		mat.transmission[0] = m[4];
		mat.transmission[1] = m[5];
		mat.transmission[2] = m[6];
	}
	return mat;
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

static bool create_source_listener_data(SourceListenerData &sld, SourceData *sd, ListenerData *ld, IPLContext ctx, IPLAudioSettings *audio_settings, IPLHRTF hrtf) {
	PROFILE_FUNCTION();
	sld.listener = ld;
	sld.direct_simulated_once = false;

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

	if (sd->cfg.reflection_enabled && ld->cfg.reflection_simulation_enabled) {
		int max_order = ld->cfg.refl_ambisonics_order;
		int num_channels = ambisonic_channels_from(max_order);

		IPLReflectionEffectSettings refl_cfg{};
		refl_cfg.type = static_cast<IPLReflectionEffectType>(ld->cfg.refl_type);
		refl_cfg.numChannels = num_channels;
		refl_cfg.irSize = int(sd->cfg.reflection_duration * audio_settings->samplingRate);
		handleErr(iplReflectionEffectCreate(ctx, audio_settings, &refl_cfg, &sld.reflection_effect), "SteamAudio: Failed to create reflection effect");

		// hybrid reflection type doesn't support the reflection mixer, so we have to use
		// individual ambisonics decode effects
		if (ld->cfg.refl_type == IPL_REFLECTIONEFFECTTYPE_HYBRID) {
			IPLAmbisonicsDecodeEffectSettings decode_cfg{};
			decode_cfg.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
			decode_cfg.hrtf = hrtf;
			decode_cfg.maxOrder = max_order;
			handleErr(iplAmbisonicsDecodeEffectCreate(ctx, audio_settings, &decode_cfg, &sld.ambisonics_decode_effect), "SteamAudio: Failed to create ambisonics decode effect");
		}
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

	// Listener RID API
	ClassDB::bind_method(D_METHOD("listener_create"), &SteamAudioServer::listener_create);
	ClassDB::bind_method(D_METHOD("listener_free", "listener"), &SteamAudioServer::listener_free);
	ClassDB::bind_method(D_METHOD("listener_set_transform", "listener", "xform"), &SteamAudioServer::listener_set_transform);
	ClassDB::bind_method(D_METHOD("listener_set_mask", "listener", "mask"), &SteamAudioServer::listener_set_mask);
	ClassDB::bind_method(D_METHOD("listener_set_range", "listener", "range"), &SteamAudioServer::listener_set_range);
	ClassDB::bind_method(D_METHOD("listener_set_sensor", "listener", "enabled"), &SteamAudioServer::listener_set_sensor);
	ClassDB::bind_method(D_METHOD("listener_set_reflection", "listener", "enabled", "rays", "bounces", "duration", "ambisonics_order", "type", "irradiance_min_dist"), &SteamAudioServer::listener_set_reflection);
	ClassDB::bind_method(D_METHOD("listener_set_debug_name", "listener", "name"), &SteamAudioServer::listener_set_debug_name);
	ClassDB::bind_method(D_METHOD("listener_get_source_db_levels", "listener"), &SteamAudioServer::listener_get_source_db_levels);

	// Source RID API
	ClassDB::bind_method(D_METHOD("source_create"), &SteamAudioServer::source_create);
	ClassDB::bind_method(D_METHOD("source_free", "source"), &SteamAudioServer::source_free);
	ClassDB::bind_method(D_METHOD("source_set_transform", "source", "xform"), &SteamAudioServer::source_set_transform);
	ClassDB::bind_method(D_METHOD("source_set_layers", "source", "layers"), &SteamAudioServer::source_set_layers);
	ClassDB::bind_method(D_METHOD("source_set_volume_db", "source", "volume_db"), &SteamAudioServer::source_set_volume_db);
	ClassDB::bind_method(D_METHOD("source_set_doppler_factor", "source", "factor"), &SteamAudioServer::source_set_doppler_factor);
	ClassDB::bind_method(D_METHOD("source_set_direct_enabled", "source", "enabled"), &SteamAudioServer::source_set_direct_enabled);
	ClassDB::bind_method(D_METHOD("source_set_binaural", "source", "enabled", "interpolation", "spatial_blend"), &SteamAudioServer::source_set_binaural);
	ClassDB::bind_method(D_METHOD("source_set_distance_attenuation", "source", "enabled", "min", "max"), &SteamAudioServer::source_set_distance_attenuation);
	ClassDB::bind_method(D_METHOD("source_set_air_absorption", "source", "enabled"), &SteamAudioServer::source_set_air_absorption);
	ClassDB::bind_method(D_METHOD("source_set_occlusion", "source", "enabled", "type", "radius", "samples"), &SteamAudioServer::source_set_occlusion);
	ClassDB::bind_method(D_METHOD("source_set_transmission", "source", "enabled", "type", "rays"), &SteamAudioServer::source_set_transmission);
	ClassDB::bind_method(D_METHOD("source_set_reflection", "source", "enabled", "duration", "hybrid_delay"), &SteamAudioServer::source_set_reflection);
	ClassDB::bind_method(D_METHOD("source_set_effect_stack", "source", "stack"), &SteamAudioServer::source_set_effect_stack);
	ClassDB::bind_method(D_METHOD("source_set_debug_name", "source", "name"), &SteamAudioServer::source_set_debug_name);
	ClassDB::bind_method(D_METHOD("source_add_playback", "source", "playback", "volume_db", "pitch_scale"), &SteamAudioServer::source_add_playback, DEFVAL(0.0f), DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("source_set_playback_volume", "source", "playback", "volume_db"), &SteamAudioServer::source_set_playback_volume);
	ClassDB::bind_method(D_METHOD("source_set_playback_pitch", "source", "playback", "pitch_scale"), &SteamAudioServer::source_set_playback_pitch);
	ClassDB::bind_method(D_METHOD("source_get_num_active_playbacks", "source"), &SteamAudioServer::source_get_num_active_playbacks);

	// Geometry RID API
	ClassDB::bind_method(D_METHOD("geometry_create_static", "verts", "tris", "material"), &SteamAudioServer::geometry_create_static);
	ClassDB::bind_method(D_METHOD("geometry_create_dynamic", "verts", "tris", "material"), &SteamAudioServer::geometry_create_dynamic);
	ClassDB::bind_method(D_METHOD("geometry_set_transform", "geometry", "xform"), &SteamAudioServer::geometry_set_transform);
	ClassDB::bind_method(D_METHOD("geometry_free", "geometry"), &SteamAudioServer::geometry_free);

	ClassDB::bind_method(D_METHOD("get_mixing_thread_usage_pct"), &SteamAudioServer::get_mixing_thread_usage_pct);
	ClassDB::bind_method(D_METHOD("get_stress_mitigation"), &SteamAudioServer::get_stress_mitigation);
	ClassDB::bind_method(D_METHOD("get_sim_thread_avg_duration_ms"), &SteamAudioServer::get_sim_thread_avg_duration_ms);
	ClassDB::bind_method(D_METHOD("get_direct_job_avg_duration_ms"), &SteamAudioServer::get_direct_job_avg_duration_ms);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mixing_thread_usage_pct"), "", "get_mixing_thread_usage_pct");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stress_mitigation"), "", "get_stress_mitigation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sim_thread_avg_duration_ms"), "", "get_sim_thread_avg_duration_ms");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "direct_job_avg_duration_ms"), "", "get_direct_job_avg_duration_ms");
	ClassDB::bind_method(D_METHOD("get_source_count"), &SteamAudioServer::get_source_count);
	ClassDB::bind_method(D_METHOD("get_listener_count"), &SteamAudioServer::get_listener_count);
	ClassDB::bind_method(D_METHOD("get_source_name", "index"), &SteamAudioServer::get_source_name);
	ClassDB::bind_method(D_METHOD("get_listener_name", "index"), &SteamAudioServer::get_listener_name);
	ClassDB::bind_method(D_METHOD("get_source_debug_string", "index"), &SteamAudioServer::get_source_debug_string);
	ClassDB::bind_method(D_METHOD("get_listener_debug_string", "index"), &SteamAudioServer::get_listener_debug_string);
}

float SteamAudioServer::get_mixing_thread_usage_pct() const {
	return mixing_thread_usage_pct.load(std::memory_order_relaxed);
}

float SteamAudioServer::get_stress_mitigation() const {
	return stress_mitigation.load(std::memory_order_relaxed);
}

float SteamAudioServer::get_sim_thread_avg_duration_ms() const {
	return sim_thread_avg_duration_ms.load(std::memory_order_relaxed);
}

float SteamAudioServer::get_direct_job_avg_duration_ms() const {
	return direct_job_avg_duration_ms.load(std::memory_order_relaxed);
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
	return sources[index]->debug_name;
}

String SteamAudioServer::get_listener_name(int index) {
	std::shared_lock lock(collections_mutex);
	if (index < 0 || index >= (int)listeners.size())
		return "";
	return listeners[index]->debug_name;
}

String SteamAudioServer::get_source_debug_string(int index) {
	std::shared_lock lock(collections_mutex);
	if (index < 0 || index >= (int)sources.size())
		return "";
	SourceData *sd = sources[index];
	int frame_size = cached_audio_settings.frameSize;

	String s;
	s += "name: " + sd->debug_name + "\n";
	s += "mixed_frames_ready: " + String::num_int64(sd->mixed_frames_ready) + "/" + String::num_int64(frame_size) + "\n";
	s += "mix_generation: " + String::num_int64((int64_t)sd->mix_generation) + "\n";
	s += "debug_times_mixed: " + String::num_int64(sd->debug_times_mixed) + "\n";
	s += "is_skipping_mixing: " + String(sd->is_skipping_mixing ? "yes\n" : "no\n");
	s += "playbacks: " + String::num_int64((int)sd->playbacks.size()) + "\n";
	for (uint32_t i = 0; i < sd->playbacks.size(); ++i) {
		auto &pb = sd->playbacks[i];
		s += "  pb[" + String::num_int64(i) + "]: playing=" + String(pb.playback->is_playing() ? "yes" : "no");
		s += " vol=" + String::num(pb.volume_linear, 3);
		s += " pitch=" + String::num(pb.pitch_scale, 3);
		s += " num_mixed=" + String::num_int64(pb.debug_num_mixed);
		s += " mixed_in_mixed_frames=" + String::num_int64(pb.num_mixed_in_current_mixed_frames) + "\n";
	}
	s += "effect_instances: " + String::num_int64((int)sd->effect_instances.size()) + "\n";
	s += "listener_data: " + String::num_int64((int)sd->listener_data.size()) + "\n";
	for (uint32_t i = 0; i < sd->listener_data.size(); ++i) {
		auto &sld = sd->listener_data[i];
		String lname = sld.listener ? sld.listener->debug_name : "<null>";
		s += "  sld[" + String::num_int64(i) + "] listener=" + lname;
		s += " dist=" + String::num(sld.dist_to_listener, 2);
		s += " doppler=" + String::num(sld.doppler_pitch, 3);
		s += " OutOfRange=" + String(sld.out_of_range ? "yes" : "no");
		s += " last_gen=" + String::num_int64(sld.last_contributed_generation);
		s += " last_consumed_mix=" + String::num_int64((int64_t)sld.last_consumed_mix);
		s += " times_contributed=" + String::num_int64(sld.debug_times_contributed);
		if (sld.skip_reflection_applies > 0)
			s += " skipping refl=" + String::num_int64(sld.skip_reflection_applies);
		s += "\n";
	}
	return s;
}

String SteamAudioServer::get_listener_debug_string(int index) {
	std::shared_lock lock(collections_mutex);
	if (index < 0 || index >= (int)listeners.size())
		return "";
	ListenerData *ld = listeners[index];
	int frame_size = cached_audio_settings.frameSize;

	String s;
	s += "name: " + ld->debug_name + "\n";
	s += "times_pushed: " + String::num_int64(ld->debug_times_pushed) + "\n";
	s += "generation: " + String::num_int64(ld->generation) + "\n";
	// Contributors-pending and drains-pending are now derived; per-playback
	// "remaining" below shows drain state. Show pending drains as a quick summary.
	{
		std::lock_guard pb_lock(*ld->playbacks_mutex);
		int pending_drains = 0;
		for (const auto &pb : ld->playbacks) {
			if (pb.remaining_from_push_buffer > 0)
				pending_drains++;
		}
		s += "pending_drains (derived): " + String::num_int64(pending_drains) + "\n";
	}
	s += "push_buffer size: " + String::num_int64((int)ld->push_buffer.size()) + "/" + String::num_int64(frame_size) + "\n";
	{
		std::lock_guard pb_lock(*ld->playbacks_mutex);
		s += "playbacks: " + String::num_int64((int)ld->playbacks.size()) + "\n";
		for (uint32_t i = 0; i < ld->playbacks.size(); ++i) {
			auto &pb = ld->playbacks[i];
			s += "  pb[" + String::num_int64(i) + "]: playing=" + String(pb.playback->is_playing() ? "yes" : "no");
			s += " remaining=" + String::num_int64(pb.remaining_from_push_buffer);
			s += " times_drained=" + String::num_int64(pb.debug_times_drained);
			s += " avail=" + String::num_int64(pb.playback->get_free_buffer_size());
			s += " underruns=" + String::num_int64(pb.playback->get_num_underrun_samples()) + "\n";
		}
	}
	s += "source_db_levels: " + String::num_int64((int)ld->source_db_levels.size()) + "\n";
	s += "has_simulator: " + String(ld->simulator ? "yes" : "no") + "\n";
	s += "dirty: " + String(ld->dirty ? "yes" : "no") + "\n";
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

	// Wait for any in-flight direct-sim job before releasing simulators/sources.
	if (direct_job_pending) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(direct_task_id);
		direct_job_pending = false;
	}

	// Drain any remaining pending ops now that threads are stopped
	apply_pending_ops();

	for (auto *ld : listeners) {
		if (ld->simulator) {
			iplSimulatorRelease(&ld->simulator);
		}
		if (ld->reflection_mixer) {
			iplReflectionMixerRelease(&ld->reflection_mixer);
		}
		if (ld->ambisonics_decode_effect) {
			iplAmbisonicsDecodeEffectRelease(&ld->ambisonics_decode_effect);
		}
		iplAudioBufferFree(phonon_context, &ld->mixed_ambisonics_buffer);
		iplAudioBufferFree(phonon_context, &ld->decode_output_buffer);
		listener_owner.free(ld->self);
		memdelete(ld);
	}
	listeners.clear();

	for (auto *sd : sources) {
		for (auto &sld : sd->listener_data) {
			cleanup_source_listener_data(sld, phonon_context);
		}
		source_owner.free(sd->self);
		memdelete(sd);
	}
	sources.clear();

	for (auto *g : geometries) {
		if (g->dynamic) {
			if (g->instanced_mesh)
				iplInstancedMeshRelease(&g->instanced_mesh);
			for (auto &m : g->meshes)
				iplStaticMeshRelease(&m);
			if (g->sub_scene)
				iplSceneRelease(&g->sub_scene);
		} else {
			for (auto &m : g->meshes) {
				iplStaticMeshRemove(m, phonon_scene);
				iplStaticMeshRelease(&m);
			}
		}
		geometry_owner.free(g->self);
		memdelete(g);
	}
	geometries.clear();

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

	// ── STEP 1: Barrier ──────────────────────────────────────────────────────
	// Wait for the previous tick's direct-sim job before touching any simulator,
	// scene or output state. This should almost never actually block; if it does
	// regularly, the job is overrunning a tick (watch get_direct_job_avg_duration_ms).
	//
	// IMPORTANT: this barrier runs BEFORE acquiring collections_mutex. The job
	// holds a shared lock on collections_mutex while it runs; if we waited while
	// also holding a shared lock, a mixing-thread apply_pending_ops() queued for
	// the unique lock could block the job's shared re-acquisition (SRWLOCK does
	// not guarantee reader barging past a queued writer) — a three-way deadlock.
	// Waiting first guarantees the job has fully released its lock before we lock.
	if (direct_job_pending) {
		// During heavy loading the WorkerThreadPool can be saturated and our
		// direct-sim task may not even have started. Direct/reflection sim results
		// don't matter on the loading screen, so rather than blocking the main
		// thread until the pool catches up, skip this whole tick when the job
		// isn't done: don't consume results, don't submit a new job. We re-check
		// next tick. The task is still waited on (below) once it completes, so the
		// pool can clean it up.
		if (!WorkerThreadPool::get_singleton()->is_task_completed(direct_task_id)) {
			return;
		}
		PROFILE_FUNCTION_NAMED("waiting_for_direct_job");
		WorkerThreadPool::get_singleton()->wait_for_task_completion(direct_task_id);
		direct_job_pending = false;
	}

	std::shared_lock lock(collections_mutex);

	// ── STEP 2: Consume the previous job's results ───────────────────────────
	// The direct-sim job has finished iplSimulatorRunDirect, so iplSourceGetOutputs
	// is valid. Update per-source dB levels here, on the main thread. The sensor-slot
	// maintenance itself now lives on the SteamAudioListener node, which pulls these.
	for (auto *ld : listeners) {
		if (!ld->simulator)
			continue;

		PROFILE_FUNCTION_NAMED("updating_source_db_levels");
		ld->source_db_levels.clear();
		for (auto *sd : sources) {
			for (auto &sld : sd->listener_data) {
				if (sld.listener != ld)
					continue;
				if ((sd->cfg.layers & ld->cfg.mask) == 0)
					continue;
				if (sld.out_of_range)
					continue;
				// No IPLSource yet (not created by the job, or creation failed) —
				// nothing to read.
				if (!sld.source)
					continue;

				// Simulation has now run at least once for this source/listener
				// pair, so iplSourceGetOutputs data and cached coords are valid.
				sld.direct_simulated_once = true;

				IPLSimulationOutputs outputs{};
				iplSourceGetOutputs(sld.source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);

				ListenerSourceDBLevel source_db_level;
				source_db_level.source = sd->self;
				source_db_level.position = sd->has_transform ? sd->pending_transform.origin : sd->last_trf.origin;
				source_db_level.db_level = sd->current_db_level;
				IPLDirectEffectParams direct_params = outputs.direct;
				float total_direct_factor = 1.0f;

				if (sd->cfg.occlusion_enabled) {
					float occlusion = direct_params.occlusion;
					float transmission = 0.0f;
					if (sd->cfg.transmission_enabled) {
						transmission = (direct_params.transmission[0] + direct_params.transmission[1] + direct_params.transmission[2]) / 3.0f;
					}
					// Total direct sound = sound that goes around (occlusion) + sound that goes through (transmission)
					// We assume transmission only applies to the occluded part.
					total_direct_factor *= (occlusion + (1.0f - occlusion) * transmission);
				}

				if (sd->cfg.air_absorption_enabled) {
					IPLAirAbsorptionModel airAbsorptionModel{};
					airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
					float air_absorption[3];
					iplAirAbsorptionCalculate(phonon_context, sd->cached_coords.origin, ld->cached_coords.origin, &airAbsorptionModel, air_absorption);
					float avg_air_absorption = (air_absorption[0] + air_absorption[1] + air_absorption[2]) / 3.0f;
					total_direct_factor *= avg_air_absorption;
				}

				if (sd->cfg.distance_attenuation_enabled) {
					float dist = sld.dist_to_listener;
					float min_dist = sd->cfg.distance_attenuation_min;
					float max_dist = sd->cfg.distance_attenuation_max;
					float attenuation = calculate_attenuation(dist, min_dist, max_dist);
					total_direct_factor *= attenuation;
				}

				float final_db_level = source_db_level.db_level + 20.0f * Math::log(MAX(total_direct_factor, 1e-10f)) / Math::log(10.0f);
				source_db_level.db_level = final_db_level;
				ld->source_db_levels.push_back(source_db_level);
			}
		}
	}

	{
		PROFILE_FUNCTION_NAMED("update_dynamic_geometry");
		PROFILING_PLOT_NUMBER("NumSteamGeometries", (int64_t)geometries.size());
		// Update Dynamic Geometry (only mark dirty if transform changed)
		for (auto *g : geometries) {
			if (!g->dynamic || !g->instanced_mesh)
				continue;
			if (!g->has_transform)
				continue;
			Transform3D trf = g->pending_transform;
			if (!trf.is_equal_approx(g->last_trf)) {
				IPLMatrix4x4 m = ipl_mat4_from(trf);
				iplInstancedMeshUpdateTransform(g->instanced_mesh, phonon_scene, m);
				g->last_trf = trf;
				scene_dirty = true;
			}
		}
	}

	{
		PROFILE_FUNCTION_NAMED("update_listeners");
		for (auto *ld : listeners) {
			if (!ld->simulator)
				continue;

			float stress_mitigation_factor = 0.1f + 0.9f * (1.0f - stress_mitigation.load());

			// Update listener transform from pushed data
			if (ld->has_transform) {
				Transform3D trf = ld->pending_transform;
				if (!trf.is_equal_approx(ld->last_trf)) {
					ld->cached_coords = ipl_coords_from(trf);

					IPLSimulationSharedInputs shared_inputs{};
					shared_inputs.listener = ld->cached_coords;
					shared_inputs.numRays = ld->cfg.num_refl_rays;
					shared_inputs.numBounces = ld->cfg.num_refl_bounces;
					shared_inputs.duration = ld->cfg.refl_duration * stress_mitigation_factor;
					shared_inputs.order = ld->cfg.refl_ambisonics_order;
					shared_inputs.irradianceMinDistance = ld->cfg.irradiance_min_dist;

					IPLSimulationFlags flags = IPL_SIMULATIONFLAGS_DIRECT;
					if (ld->cfg.reflection_simulation_enabled) {
						flags = static_cast<IPLSimulationFlags>(flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
					}

					iplSimulatorSetSharedInputs(ld->simulator, flags, &shared_inputs);
					ld->last_trf = trf;
				}
			}

			// Update source transforms for this listener's simulator
			for (auto *sd : sources) {
				sd->volume_linear = UtilityFunctions::db_to_linear(sd->cfg.volume_db);

				if ((sd->cfg.layers & ld->cfg.mask) == 0)
					continue;

				// Find the SourceListenerData for this listener/source pair
				SourceListenerData *sld = nullptr;
				for (auto &entry : sd->listener_data) {
					if (entry.listener == ld) {
						sld = &entry;
						break;
					}
				}
				if (!sld)
					continue;

				// IPLSource creation is deferred to the direct-sim job (see below) —
				// it is allocation-heavy and should not run on the main thread.

				if (!sd->has_transform) {
					// No transform pushed yet — nothing meaningful to simulate.
					continue;
				}
				Transform3D src_trf = sd->pending_transform;
				sd->cached_coords = ipl_coords_from(src_trf);

				// Update dist_to_listener and doppler
				float prev_dist_to_listener = sld->dist_to_listener;
				sld->dist_to_listener = src_trf.origin.distance_to(ld->last_trf.origin);

				float doppler_factor = sd->cfg.doppler_factor;
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
				float listener_range = ld->cfg.range;
				// when the source doesn't have reflection enabled,
				// we can also use the max distance of that
				if (!sd->cfg.reflection_enabled)
					listener_range = MIN(listener_range, sd->cfg.distance_attenuation_max);
				if (listener_range > 0.0f) {
					if (!sld->out_of_range && sld->dist_to_listener > listener_range * 1.1f) {
						sld->out_of_range = true;
						if (sld->source) {
							iplSourceRemove(sld->source, ld->simulator);
							ld->dirty = true;
						}
						// No consumer bookkeeping needed: the mixing thread's mix-reset
						// gate recomputes consumers each cycle and simply skips
						// out_of_range pairs. (Previously this decremented
						// sd->pending_consumers here — a cross-thread RMW race.)
					} else if (sld->out_of_range && sld->dist_to_listener <= listener_range) {
						sld->out_of_range = false;
						if (sld->source) {
							iplSourceAdd(sld->source, ld->simulator);
							ld->dirty = true;
						}
					}
				} else if (sld->out_of_range) {
					// Range was disabled, bring back in range
					sld->out_of_range = false;
					if (sld->source) {
						iplSourceAdd(sld->source, ld->simulator);
						ld->dirty = true;
					}
				}

				if (sld->out_of_range) {
					sd->last_trf = src_trf;
					continue;
				}

				// Record a deferred IPLSource creation for the direct-sim job. We
				// only do this for in-range sources, so a source that spawns out of
				// range never gets created until it actually comes into range.
				if (!sld->source && !sld->pending_create) {
					sld->pending_create = true;
					sld->pending_source_settings = IPLSourceSettings{};
					sld->pending_source_settings.flags = static_cast<IPLSimulationFlags>(
							(sd->cfg.direct_enabled ? IPL_SIMULATIONFLAGS_DIRECT : 0) |
							(sd->cfg.reflection_enabled ? IPL_SIMULATIONFLAGS_REFLECTIONS : 0));
					ld->dirty = true;
				}

				IPLSimulationInputs inputs{};
				inputs.flags = static_cast<IPLSimulationFlags>(0);
				if (sd->cfg.direct_enabled) {
					inputs.flags = static_cast<IPLSimulationFlags>(inputs.flags | IPL_SIMULATIONFLAGS_DIRECT);
				}
				if (sd->cfg.reflection_enabled) {
					inputs.flags = static_cast<IPLSimulationFlags>(inputs.flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
				}

				inputs.directFlags = static_cast<IPLDirectSimulationFlags>(0);
				if (sd->cfg.direct_enabled) {
					if (sd->cfg.occlusion_enabled) {
						inputs.directFlags = static_cast<IPLDirectSimulationFlags>(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION);
					}
					if (sd->cfg.transmission_enabled) {
						inputs.directFlags = static_cast<IPLDirectSimulationFlags>(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
					}
					if (sd->cfg.distance_attenuation_enabled) {
						inputs.directFlags = static_cast<IPLDirectSimulationFlags>(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION);
					}
					if (sd->cfg.air_absorption_enabled) {
						inputs.directFlags = static_cast<IPLDirectSimulationFlags>(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION);
					}
				}

				inputs.source = sd->cached_coords;
				inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
				inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_EXPONENTIAL;

				inputs.occlusionType = static_cast<IPLOcclusionType>(sd->cfg.occlusion_type);
				inputs.occlusionRadius = sd->cfg.occlusion_radius;
				inputs.numOcclusionSamples = sd->cfg.occlusion_samples;
				inputs.numTransmissionRays = sd->cfg.transmission_rays;

				for (int i = 0; i < IPL_NUM_BANDS; ++i)
					inputs.reverbScale[i] = 1.0f;
				inputs.hybridReverbTransitionTime = sd->cfg.reflection_hybrid_delay;
				inputs.hybridReverbOverlapPercent = 0.25f;
				inputs.baked = IPL_FALSE;

				if (sld->source) {
					iplSourceSetInputs(sld->source, inputs.flags, &inputs);
				} else {
					// Source not created yet — stash the inputs so the job can apply
					// them right after iplSourceCreate (first run is already correct).
					sld->pending_inputs = inputs;
				}
				sd->last_trf = src_trf;
			}
		}
	}

	// ── STEP 5: Submit the direct-sim job ────────────────────────────────────
	// The job performs deferred IPLSource creation, the scene/simulator commit
	// (coordinated with the reflection thread), iplSimulatorRunDirect, and finally
	// wakes the reflection thread. It is fire-and-forget — the next tick's STEP 1
	// barrier waits on it.
	direct_task_id = WorkerThreadPool::get_singleton()->add_native_task(
			&SteamAudioServer::run_direct_job, this, true, "SteamAudio direct sim");
	direct_job_pending = true;
}

void SteamAudioServer::run_direct_job(void *p_self) {
	SteamAudioServer *self = static_cast<SteamAudioServer *>(p_self);
	PROFILE_FUNCTION_NAMED("direct_sim_job");
	auto job_start = std::chrono::steady_clock::now();

	// Shared lock keeps the collections stable for the job's duration —
	// apply_pending_ops() (mixing thread) unique-locks the same mutex, so it
	// cannot add/remove sources or listeners while the job runs.
	std::shared_lock lock(self->collections_mutex);

	// 1. Deferred IPLSource creation (the old "adding_ipl_source" work, moved off
	//    the main thread). Inputs were stashed by tick() so the first run is correct.
	{
		PROFILE_FUNCTION_NAMED("adding_ipl_source");
		for (auto *ld : self->listeners) {
			if (!ld->simulator)
				continue;
			for (auto *sd : self->sources) {
				for (auto &sld : sd->listener_data) {
					if (sld.listener != ld || !sld.pending_create)
						continue;
					if (handleErr(iplSourceCreate(ld->simulator, &sld.pending_source_settings, &sld.source), "SteamAudio: Failed to create source")) {
						iplSourceAdd(sld.source, ld->simulator);
						iplSourceSetInputs(sld.source, sld.pending_inputs.flags, &sld.pending_inputs);
					}
					sld.pending_create = false;
				}
			}
		}
	}

	// 2. Commit scene + simulators, coordinated with the reflection thread.
	//    Only commit/add conflict with a running simulation, and the reflection
	//    thread only starts a pass once new_inputs_set is true (set in step 4) —
	//    so during this step the reflection thread is either mid-pass (defer the
	//    commit) or blocked (safe to commit now).
	bool need_commit = self->scene_dirty;
	if (!need_commit) {
		for (auto *ld : self->listeners) {
			if (ld->dirty) {
				need_commit = true;
				break;
			}
		}
	}
	if (need_commit) {
		// Decide-and-commit under refl_mux so this is mutually exclusive with the
		// reflection thread's transition into a pass (simulation_thread_func takes
		// the same lock to set is_refl_thread_processing). While we hold the lock the
		// reflection thread cannot start a pass, so observing
		// is_refl_thread_processing == false guarantees it stays idle until we commit.
		std::unique_lock<std::mutex> lock_refl(self->refl_mux);
		// Hold off the reflection thread from starting a new pass until committed.
		self->refl_thread_wait_for_commit.store(true);
		if (!self->is_refl_thread_processing.load()) {
			PROFILE_FUNCTION_NAMED("committing_scene_and_simulators");
			if (self->scene_dirty) {
				iplSceneCommit(self->phonon_scene);
				self->scene_dirty = false;
			}
			for (auto *ld : self->listeners) {
				if (ld->dirty) {
					iplSimulatorCommit(ld->simulator);
					ld->dirty = false;
				}
			}
			self->refl_thread_wait_for_commit.store(false);
		}
		// else: reflection thread is mid-pass — leave refl_thread_wait_for_commit
		// set and the dirty flags pending; a later job commits when it is idle.
	}

	// 3. Run the direct simulation for every simulator.
	{
		PROFILE_FUNCTION_NAMED("running_direct_simulation");
		for (auto *ld : self->listeners) {
			if (!ld->simulator)
				continue;
			iplSimulatorRunDirect(ld->simulator);
		}
	}

	// 4. Inputs are committed/ready — wake the reflection thread. Set the flag under
	//    refl_mux so the store cannot interleave with the reflection thread's wait
	//    predicate evaluation and lose the wake-up.
	{
		std::unique_lock<std::mutex> lock_refl(self->refl_mux);
		self->new_inputs_set.store(true);
	}
	self->refl_cv.notify_one();

	auto job_end = std::chrono::steady_clock::now();
	float dur_ms = std::chrono::duration<float, std::milli>(job_end - job_start).count();
	// Exponential moving average (alpha = 0.1)
	float prev = self->direct_job_avg_duration_ms.load(std::memory_order_relaxed);
	self->direct_job_avg_duration_ms.store(prev * 0.9f + dur_ms * 0.1f, std::memory_order_relaxed);
}

void SteamAudioServer::simulation_thread_func() {
	LocalVector<IPLSimulator> simulators;
	while (is_running.load()) {
		// Wait for ready inputs and transition to "processing" atomically under
		// refl_mux. run_direct_job() takes the same lock to decide whether it may
		// commit (see the commit block there): because that decision and this
		// transition share refl_mux, a commit and a reflection pass can never run
		// concurrently on the same simulator. The long iplSimulatorRunReflections
		// calls below run with NO lock held, preserving the non-blocking design.
		{
			std::unique_lock<std::mutex> lock_refl(refl_mux);
			refl_cv.wait(lock_refl, [&] {
				return (!refl_thread_wait_for_commit.load() && new_inputs_set.load()) || !is_running.load();
			});

			if (!is_running.load())
				break;

			is_refl_thread_processing.store(true);
			new_inputs_set.store(false);
		}

		{
			auto sim_start = std::chrono::steady_clock::now();

			PROFILE_FUNCTION_NAMED("refl_sim");

			// Snapshot simulators with retain so we don't hold collections_mutex
			// during the potentially long iplSimulatorRunReflections calls.
			simulators.clear();
			{
				std::shared_lock lock(collections_mutex);
				for (auto *ld : listeners) {
					if (!ld->simulator || !ld->simulator_reflection_enabled)
						continue;
					simulators.push_back(iplSimulatorRetain(ld->simulator));
				}
			}

			for (auto &sim : simulators) {
				iplSimulatorRunReflections(sim);
			}

			for (auto &sim : simulators) {
				iplSimulatorRelease(&sim);
			}
			simulators.clear();

			auto sim_end = std::chrono::steady_clock::now();
			float dur_ms = std::chrono::duration<float, std::milli>(sim_end - sim_start).count();
			// Exponential moving average (alpha = 0.1)
			float prev = sim_thread_avg_duration_ms.load(std::memory_order_relaxed);
			sim_thread_avg_duration_ms.store(prev * 0.9f + dur_ms * 0.1f, std::memory_order_relaxed);

			// Clear "processing" under refl_mux so a direct job blocked on the lock
			// observes the cleared flag and may commit on its next pass.
			{
				std::unique_lock<std::mutex> lock_refl(refl_mux);
				is_refl_thread_processing.store(false);
			}
			refl_cv.notify_all();
		}
	}
}

void SteamAudioServer::mixing_thread_func() {
	using clock = std::chrono::steady_clock;

	using dseconds = std::chrono::duration<double>;
	const float STRESS_MITIGATION_SKIPS_PER_SECOND = 5.0f;
	float stress_mitigation_skips_accumulator = 0;
	auto last_stress_mitigation_check_time = clock::now();

	auto wait_time = std::chrono::milliseconds(
		1
	);
	auto spin_threshold = std::chrono::microseconds(500);
	auto prev_start = clock::now();
	while (is_running.load()) {
		auto start = clock::now();
		double dt = dseconds(start - prev_start).count();
		prev_start = start;
		process_audio(dt);
		auto work_end = clock::now();

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

		auto total = clock::now() - start;
		auto busy = work_end - start;
		float pct = (total.count() > 0) ? (static_cast<float>(busy.count()) / static_cast<float>(total.count()) * 100.0f) : 0.0f;
		// Exponential moving average (alpha = 0.1)
		float prev = mixing_thread_usage_pct.load(std::memory_order_relaxed);
		mixing_thread_usage_pct.store(prev * 0.9f + pct * 0.1f, std::memory_order_relaxed);

		// Update stress mitigation: start at 50% usage (0.0) to 100% usage (1.0).
		// Reacts fast to increase (alpha = 0.2), damp down more slowly (alpha = 0.01).
		float target_mitigation = CLAMP((mixing_thread_usage_pct - 50.0f) / 50.0f, 0.0f, 1.0f);
		float current_mitigation = stress_mitigation.load(std::memory_order_relaxed);
		float alpha = (target_mitigation > current_mitigation) ? 0.2f : 0.01f;
		stress_mitigation.store(current_mitigation * (1.0f - alpha) + target_mitigation * alpha, std::memory_order_relaxed);

		// and the most drastic stress mitigation: skip iplApplyReflectionEffect calls.
		// all the other measures didn't seem very effective, but this will reduce the
		// stress on this thread dramatically and "only" cost the reflection part of
		// some audio sources.
		dseconds stress_mitigation_check_duration = clock::now() - last_stress_mitigation_check_time;
		last_stress_mitigation_check_time = clock::now();
		stress_mitigation_skips_accumulator += STRESS_MITIGATION_SKIPS_PER_SECOND * stress_mitigation * stress_mitigation_check_duration.count();
		if (stress_mitigation_skips_accumulator > 1) {
			stress_mitigation_skips_accumulator -= 1;
			std::shared_lock lock(collections_mutex);
			for (int i = (int)sources.size() - 1; i >= 0; --i) {
				bool found = false;
				for (auto &sld : sources[i]->listener_data) {
					if (sld.skip_reflection_applies > 0 || !sld.reflection_effect)
						continue;
					// 200 is quite long, with normal settings this will result in
					// 2 seconds of skipping reflection for this source. but keeping
					// STRESS_MITIGATION_SKIPS_PER_SECOND low and therefore the skip
					// duration higher has proven to be more stable for the stress.
					sld.skip_reflection_applies = 200;
					found = true;
					break;
				}
				if (found)
					break;
			}
		}
	}
}

void SteamAudioServer::create_listener_ipl(ListenerData *ld) {
	ld->simulator_reflection_enabled = ld->cfg.reflection_simulation_enabled;
	ld->dirty = true;
	ld->push_buffer.resize(cached_audio_settings.frameSize);

	// Initialize Simulator for this listener
	IPLSimulationSettings sim_cfg{};
	sim_cfg.flags = IPL_SIMULATIONFLAGS_DIRECT;
	if (ld->cfg.reflection_simulation_enabled) {
		sim_cfg.flags = static_cast<IPLSimulationFlags>(sim_cfg.flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
	}
	sim_cfg.sceneType = static_cast<IPLSceneType>(get_project_int("steamaudio/scene_type", IPL_SCENETYPE_DEFAULT));
	sim_cfg.reflectionType = static_cast<IPLReflectionEffectType>(ld->cfg.refl_type);
	sim_cfg.maxNumOcclusionSamples = get_project_int("steamaudio/max_occlusion_samples", 64);
	sim_cfg.maxNumRays = ld->cfg.num_refl_rays;
	sim_cfg.numDiffuseSamples = 32;
	sim_cfg.maxDuration = ld->cfg.refl_duration;
	sim_cfg.maxOrder = ld->cfg.refl_ambisonics_order;
	sim_cfg.maxNumSources = 256;
	sim_cfg.numThreads = 2;
	sim_cfg.samplingRate = cached_audio_settings.samplingRate;
	sim_cfg.frameSize = cached_audio_settings.frameSize;

	if (handleErr(iplSimulatorCreate(phonon_context, &sim_cfg, &ld->simulator), "SteamAudio: Failed to create simulator for listener")) {
		iplSimulatorSetScene(ld->simulator, phonon_scene);
		iplSimulatorCommit(ld->simulator);
	}

	if (ld->cfg.reflection_simulation_enabled && ld->cfg.refl_type == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION) {
		int max_order = ld->cfg.refl_ambisonics_order;
		int num_channels = ambisonic_channels_from(max_order);

		IPLReflectionEffectSettings refl_cfg{};
		refl_cfg.type = static_cast<IPLReflectionEffectType>(ld->cfg.refl_type);
		refl_cfg.numChannels = num_channels;
		refl_cfg.irSize = int(ld->cfg.refl_duration * cached_audio_settings.samplingRate);
		handleErr(iplReflectionMixerCreate(phonon_context, &cached_audio_settings, &refl_cfg, &ld->reflection_mixer), "SteamAudio: Failed to create reflection mixer");

		IPLAmbisonicsDecodeEffectSettings decode_cfg{};
		decode_cfg.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
		decode_cfg.hrtf = phonon_hrtf;
		decode_cfg.maxOrder = max_order;
		handleErr(iplAmbisonicsDecodeEffectCreate(phonon_context, &cached_audio_settings, &decode_cfg, &ld->ambisonics_decode_effect), "SteamAudio: Failed to create listener ambisonics decode effect");

		iplAudioBufferAllocate(phonon_context, num_channels, cached_audio_settings.frameSize, &ld->mixed_ambisonics_buffer);
		iplAudioBufferAllocate(phonon_context, 2, cached_audio_settings.frameSize, &ld->decode_output_buffer);
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
				SourceData *sd = source_owner.get_or_null(pending.source);
				if (!sd)
					return;
				// Check for duplicates
				for (auto *existing : sources) {
					if (existing == sd)
						return;
				}

				// Create cross-references with all existing listeners
				for (auto *ld : listeners) {
					SourceListenerData sld;
					if (create_source_listener_data(sld, sd, ld, phonon_context, &cached_audio_settings, phonon_hrtf)) {
						// New source starts at mix_generation 0; align the pair so it
						// doesn't appear to owe a consumption of a mix that never existed.
						sld.last_consumed_mix = sd->mix_generation;
						sd->listener_data.push_back(sld);
					}
				}

				sources.push_back(sd);

			} else if constexpr (std::is_same_v<T, PendingRemoveSource>) {
				SourceData *sd = source_owner.get_or_null(pending.source);
				if (!sd)
					return;
				for (uint32_t i = 0; i < sources.size(); ++i) {
					if (sources[i] != sd)
						continue;
					for (auto &sld : sd->listener_data) {
						// No counter fix-up needed: removing the source removes it from
						// every listener's derived contributor scan next cycle.

						// Remove source from its simulator (reuse ld loop)
						for (auto *ld : listeners) {
							if (ld->simulator && sld.source && sld.listener == ld) {
								iplSourceRemove(sld.source, ld->simulator);
								ld->dirty = true;
							}
						}
						cleanup_source_listener_data(sld, phonon_context);
					}
					sources.remove_at(i);
					break;
				}
				source_owner.free(sd->self);
				memdelete(sd);

			} else if constexpr (std::is_same_v<T, PendingAddListener>) {
				ListenerData *ld = listener_owner.get_or_null(pending.listener);
				if (!ld)
					return;
				for (auto *existing : listeners) {
					if (existing == ld)
						return;
				}

				// Build the IPL simulator and per-listener IPL objects from cfg.
				create_listener_ipl(ld);

				// Create per-listener state for all existing sources
				for (auto *sd : sources) {
					SourceListenerData sld;
					if (create_source_listener_data(sld, sd, ld, phonon_context, &cached_audio_settings, phonon_hrtf)) {
						// Align the new pair with the source's current mix so it doesn't
						// force re-consumption of a mix this listener was never present for.
						sld.last_consumed_mix = sd->mix_generation;
						sd->listener_data.push_back(sld);
					}
				}

				// No contributor seeding needed: the new listener's generation (1) does
				// not match any pair's last_contributed_generation (0), so Phase 3's
				// derived scan treats every relevant source as a pending contributor.

				listeners.push_back(ld);

			} else if constexpr (std::is_same_v<T, PendingRemoveListener>) {
				ListenerData *ld = listener_owner.get_or_null(pending.listener);
				if (!ld)
					return;

				// Clean up per-listener state in all sources
				for (auto *sd : sources) {
					for (uint32_t i = 0; i < sd->listener_data.size(); ++i) {
						if (sd->listener_data[i].listener != ld)
							continue;
						SourceListenerData &sld = sd->listener_data[i];

						// No counter fix-up needed: removing this pair drops the listener
						// from each source's derived consumer scan and removes the source
						// from this (now-gone) listener's contributor scan automatically.

						// Remove source from simulator before releasing
						if (ld->simulator && sld.source) {
							iplSourceRemove(sld.source, ld->simulator);
						}
						cleanup_source_listener_data(sld, phonon_context);
						sd->listener_data.remove_at(i);
						break;
					}
				}

				for (uint32_t i = 0; i < listeners.size(); ++i) {
					if (listeners[i] != ld)
						continue;
					if (ld->simulator) {
						iplSimulatorRelease(&ld->simulator);
					}
					if (ld->reflection_mixer) {
						iplReflectionMixerRelease(&ld->reflection_mixer);
					}
					if (ld->ambisonics_decode_effect) {
						iplAmbisonicsDecodeEffectRelease(&ld->ambisonics_decode_effect);
					}
					iplAudioBufferFree(phonon_context, &ld->mixed_ambisonics_buffer);
					iplAudioBufferFree(phonon_context, &ld->decode_output_buffer);
					listeners.erase(listeners.begin() + i);
					break;
				}
				listener_owner.free(ld->self);
				memdelete(ld);

			} else if constexpr (std::is_same_v<T, PendingAddPlaybackToSource>) {
				SourceData *sd = source_owner.get_or_null(pending.source);
				if (!sd)
					return;
				SourcePlaybackEntry entry;
				entry.playback = pending.playback;
				entry.volume_linear = pending.volume_linear;
				entry.pitch_scale = pending.pitch_scale;
				sd->playbacks.push_back(entry);

			} else if constexpr (std::is_same_v<T, PendingAddPlaybackToListener>) {
				ListenerData *ld = listener_owner.get_or_null(pending.listener);
				if (!ld)
					return;
				bool was_empty;
				{
					std::lock_guard<std::mutex> pb_lock(*ld->playbacks_mutex);
					was_empty = ld->playbacks.is_empty();
					RID is_playback_of_source;
					for (auto *sd : sources) {
						for (const auto &spb : sd->playbacks) {
							if (spb.playback == pending.playback) {
								is_playback_of_source = sd->self;
								break;
							}
						}
						if (is_playback_of_source.is_valid()) {
							break;
						}
					}
					ld->playbacks.push_back({ pending.playback, 0, is_playback_of_source });
				}
				if (was_empty) {
					// Listener was inactive — start a fresh cycle. Clearing the buffer
					// and bumping the generation together keeps the invariant that a new
					// generation always starts from a zeroed push_buffer; the bump makes
					// stale last_contributed_generation values mismatch, re-arming Phase 3
					// to re-contribute every relevant source. No counter seeding: both
					// contributors and consumers are derived each cycle, so a pair whose
					// last_consumed_mix lags the source's mix_generation is picked up by
					// the mix-reset gate once the listener is active again.
					ld->push_buffer.fill(Vector2(0, 0));
					ld->generation++;
				}

			} else if constexpr (std::is_same_v<T, PendingAddGeometry>) {
				GeometryData *g = geometry_owner.get_or_null(pending.geometry);
				if (!g)
					return;
				for (auto *existing : geometries) {
					if (existing == g)
						return;
				}
				geometries.push_back(g);
				scene_dirty = true;

			} else if constexpr (std::is_same_v<T, PendingRemoveGeometry>) {
				GeometryData *g = geometry_owner.get_or_null(pending.geometry);
				if (!g)
					return;
				for (uint32_t i = 0; i < geometries.size(); ++i) {
					if (geometries[i] != g)
						continue;
					if (g->dynamic) {
						for (auto &m : g->meshes) {
							iplStaticMeshRemove(m, g->sub_scene);
							iplStaticMeshRelease(&m);
						}
						if (g->instanced_mesh) {
							iplInstancedMeshRemove(g->instanced_mesh, phonon_scene);
							iplInstancedMeshRelease(&g->instanced_mesh);
						}
						if (g->sub_scene)
							iplSceneRelease(&g->sub_scene);
					} else {
						for (auto &m : g->meshes) {
							iplStaticMeshRemove(m, phonon_scene);
							iplStaticMeshRelease(&m);
						}
					}
					geometries.remove_at(i);
					scene_dirty = true;
					break;
				}
				geometry_owner.free(g->self);
				memdelete(g);
			}
		}, op);
	}
}

void SteamAudioServer::process_audio(double dt) {
	PROFILE_FUNCTION();
	const int frame_size = cached_audio_settings.frameSize;

	// Apply any queued operations (briefly unique-locks collections_mutex)
	apply_pending_ops();

	std::shared_lock lock(collections_mutex);

	{
		PROFILE_FUNCTION_NAMED("process_audio_phase1");
		// =========================================================================
		// PHASE 1: Drain push_buffers into listener playbacks.
		// Push ready push_buffers to playbacks. When fully drained, clear and bump
		// the generation to arm the next contribution round.
		// =========================================================================
		for (auto *ld : listeners) {
			std::lock_guard pb_lock(*ld->playbacks_mutex);

			// Drain gate (derived): is any playback still holding data from the
			// current push_buffer? If not, there is nothing to drain this cycle.
			bool any_pending = false;
			for (const auto &pb : ld->playbacks) {
				if (pb.remaining_from_push_buffer > 0) {
					any_pending = true;
					break;
				}
			}
			if (!any_pending)
				continue;

			PROFILE_FUNCTION_NAMED("Listener draining");

			// Remove dead playbacks and push remaining data to active ones.
			for (int i = (int)ld->playbacks.size() - 1; i >= 0; --i) {
				if (!ld->playbacks[i].playback->is_playing()) {
					ld->playbacks.remove_at(i);
					continue;
				}
				auto &pb = ld->playbacks[i];
				if (pb.remaining_from_push_buffer <= 0)
					continue;
				int free_available_in_buffer = pb.playback->get_free_buffer_size();
				if (free_available_in_buffer == 0) {
					if (pb.is_playback_of_source.is_valid()) {
						// when this listener actually plays back via a source (e.g. walkie talkie),
						// we have to check if that source is skipping mixing (leading to a full buffer)
						for (auto *sd : sources) {
							if (sd->self == pb.is_playback_of_source) {
								if (sd->is_skipping_mixing) {
									pb.remaining_from_push_buffer = 0;
								}
								break;
							}
						}
					}
				}
				else if (pb.remaining_from_push_buffer <= free_available_in_buffer) {
					pb.playback->push_buffer(ld->push_buffer.slice(frame_size - pb.remaining_from_push_buffer));
					pb.remaining_from_push_buffer = 0;
					pb.debug_times_drained++;
				} else {
					int num_to_push = MIN(pb.remaining_from_push_buffer, free_available_in_buffer);
					if (num_to_push > 0) {
						int start = frame_size - pb.remaining_from_push_buffer;
						pb.playback->push_buffer(ld->push_buffer.slice(start, start + num_to_push));
						pb.remaining_from_push_buffer -= num_to_push;
					}
				}
			}

			// All playbacks drained — clear buffer and arm the next round.
			bool still_pending = false;
			for (const auto &pb : ld->playbacks) {
				if (pb.remaining_from_push_buffer > 0) {
					still_pending = true;
					break;
				}
			}
			if (!still_pending) {
				ld->debug_times_pushed += 1;
				ld->push_buffer.fill(Vector2(0, 0));
				// Bumping the generation re-arms Phase 3's derived contributor scan;
				// no explicit contributor count is maintained.
				ld->generation++;
			}
		}
	}

	{
		PROFILE_FUNCTION_NAMED("process_audio_phase2");
		// =========================================================================
		// PHASE 2: Mix source playbacks into mixed_frames.
		// For each source with no pending consumers, reset and pull new audio.
		// =========================================================================
		for (auto *sd : sources) {
			PROFILE_FUNCTION_NAMED("Source Pre-mixing");

			// A source with no currently-playing playbacks is silent. Force its
			// level to the noise floor so sensor-only listeners / dB meters stop
			// reading a stale level. current_db_level is otherwise only refreshed
			// while the source is being mixed (which requires an output listener),
			// so without this a stopped source stays pinned at its last level and
			// the sensor-slot decay can never clear it.
			{
				bool any_playing = false;
				for (auto &pb : sd->playbacks) {
					if (pb.playback->is_playing()) {
						any_playing = true;
						break;
					}
				}
				if (!any_playing)
					sd->current_db_level = -200;
			}

			// Determine who needs this source mixed this cycle.
			//   - has_output_consumer: an in-range, mask-matching, already-simulated
			//     listener with a playing playback. It consumes spatialized audio and its
			//     ring buffer provides realtime backpressure.
			//   - has_sensor_consumer: an in-range, mask-matching, already-simulated sensor
			//     listener (no playback). It does NOT consume audio, but it needs the source
			//     mixed so current_db_level stays fresh for its sensor slots. It provides no
			//     backpressure, so a source mixed solely for sensors must be paced (below).
			// The direct_simulated_once gate matters here too: until simulation has run once
			// for a pair, mixing would use invalid (zero-initialised) outputs.
			bool has_output_consumer = false;
			bool has_sensor_consumer = false;
			for (auto &sld : sd->listener_data) {
				if ((sd->cfg.layers & sld.listener->cfg.mask) == 0)
					continue;
				if (sld.out_of_range)
					continue;
				if (!sld.direct_simulated_once)
					continue;
				ListenerData *ld = sld.listener;
				if (ld->cfg.is_sensor)
					has_sensor_consumer = true;
				{
					std::lock_guard pb_lock(*ld->playbacks_mutex);
					for (auto &pb : ld->playbacks) {
						if (pb.playback->is_playing()) {
							has_output_consumer = true;
							break;
						}
					}
				}
				// An output consumer is sufficient and bypasses sensor pacing, so we can
				// stop scanning as soon as we find one.
				if (has_output_consumer)
					break;
			}
			sd->is_skipping_mixing = !(has_output_consumer || has_sensor_consumer);
			if (sd->is_skipping_mixing) {
				// Reset pacing so an idle source doesn't accumulate a catch-up burst.
				sd->sensor_pacing_accumulator = 0.0;
				continue;
			}

			// Pacing gate: when a source is mixed only for sensor listeners there is no
			// audio-device backpressure, so the mixing thread (spins ~1 ms) would race
			// through the playback far faster than realtime and short bursts would be
			// consumed before the per-frame tick() reader samples them. Throttle re-mixing
			// to ~realtime here. (Skipped entirely when an output consumer exists — its
			// ring-buffer backpressure already paces the source.)
			if (!has_output_consumer) {
				sd->sensor_pacing_accumulator += dt * cached_audio_settings.samplingRate;
				if (sd->sensor_pacing_accumulator < frame_size)
					continue; // not time for another frame yet — hold the current level
				// Cap so a stall (e.g. a long process_audio gap) doesn't burst-catch-up.
				if (sd->sensor_pacing_accumulator > 2.0 * frame_size)
					sd->sensor_pacing_accumulator = 2.0 * frame_size;
				sd->sensor_pacing_accumulator -= frame_size;
			}

			// Mix-reset gate (derived): if a completed mix is still owed to any
			// active consumer, don't reset/re-mix yet. A consumer is "active" when
			// its mask matches, it is in range, and the listener has a playing
			// playback; it still owes consumption while last_consumed_mix <
			// mix_generation. Recomputed each cycle, so a removed/inactive/out-of-range
			// listener can never strand the source.
			if (sd->mixed_frames_ready >= frame_size) {
				bool consumers_pending = false;
				for (auto &sld : sd->listener_data) {
					if ((sd->cfg.layers & sld.listener->cfg.mask) == 0)
						continue;
					if (sld.out_of_range)
						continue;
					if (sld.last_consumed_mix >= sd->mix_generation)
						continue;
					ListenerData *ld = sld.listener;
					std::lock_guard pb_lock(*ld->playbacks_mutex);
					for (auto &pb : ld->playbacks) {
						if (pb.playback->is_playing()) {
							consumers_pending = true;
							break;
						}
					}
					if (consumers_pending)
						break;
				}
				if (consumers_pending)
					continue;
			}

			// Reset completed+consumed mix
			if (sd->mixed_frames_ready >= frame_size) {
				sd->mixed_frames.fill(Vector2(0, 0));
				sd->mixed_frames_ready = 0;
				for (auto &pb : sd->playbacks) {
					pb.num_mixed_in_current_mixed_frames = 0;
				}

				sd->debug_times_mixed += 1;
			}

			// Clean up finished playbacks
			for (int i = (int)sd->playbacks.size() - 1; i >= 0; --i) {
				if (!sd->playbacks[i].playback->is_playing()) {
					sd->playbacks.remove_at(i);
				}
			}

			int prev_mixed_count = sd->mixed_frames_ready;
			int min_frames_ready = frame_size;
			for (auto &pb : sd->playbacks) {
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
					sd->mixed_frames[mixed_index] += frames[frames_index] * pb.volume_linear * sd->volume_linear;
					++mixed_index;
				}
				min_frames_ready = MIN(min_frames_ready, mixed_index);
				pb.num_mixed_in_current_mixed_frames = mixed_index;
			}
			sd->mixed_frames_ready = min_frames_ready;

			if (!sd->effect_instances.is_empty()) {
				PROFILE_FUNCTION_NAMED("Effect Stack Processing");
				// Apply effect stack in-place on mixed frames
				int num_newly_ready = sd->mixed_frames_ready - prev_mixed_count;
				if (num_newly_ready > 0) {
					PackedVector2Array new_frames(sd->mixed_frames.slice(prev_mixed_count, sd->mixed_frames_ready));
					for (auto &inst : sd->effect_instances) {
						new_frames = inst->process_audio(
								new_frames,
								frame_size);
					}
					int new_index = 0;
					for (int mixed_index = prev_mixed_count; mixed_index < sd->mixed_frames_ready; ++mixed_index) {
						sd->mixed_frames[mixed_index] = new_frames[new_index];
						new_index++;
					}
				}
			}
			if (sd->mixed_frames_ready == frame_size) {
				// Mix complete — advance the mix generation. Every active consumer now
				// has last_consumed_mix < mix_generation and so "owes" a consumption,
				// which the mix-reset gate above derives next cycle. This block runs
				// once per completed mix: the gate then blocks re-entry until the mix
				// is reset, and the reset only happens once all consumers have caught up.
				sd->mix_generation++;

				// Calculate dB level for current mix
				float sum_sq = 0.0f;
				for (int i = 0; i < frame_size; ++i) {
					sum_sq += sd->mixed_frames[i].x * sd->mixed_frames[i].x;
					sum_sq += sd->mixed_frames[i].y * sd->mixed_frames[i].y;
				}
				float rms = sqrtf(sum_sq / (frame_size * 2));
				if (rms > 0.000001f) {
					sd->current_db_level = 20.0f * log10f(rms);
				} else {
					sd->current_db_level = -200; // Silenced/Noise floor
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
		for (auto *ld : listeners) {
			bool has_active_playbacks;
			bool drains_pending = false;
			{
				std::lock_guard pb_lock(*ld->playbacks_mutex);
				// Remove dead playbacks
				for (int i = (int)ld->playbacks.size() - 1; i >= 0; --i) {
					if (!ld->playbacks[i].playback->is_playing()) {
						ld->playbacks.remove_at(i);
					}
				}
				has_active_playbacks = !ld->playbacks.is_empty();
				for (const auto &pb : ld->playbacks) {
					if (pb.remaining_from_push_buffer > 0) {
						drains_pending = true;
						break;
					}
				}
			}

			if (!has_active_playbacks) {
				// Inactive listeners are excluded from every source's derived consumer
				// scan (which requires a playing playback), so they can no longer stall a
				// source. No fake-consumption bookkeeping needed.
				continue;
			}

			// If the previous push_buffer is still draining to playbacks, leave it
			// untouched; Phase 1 finishes draining it and bumps the generation, which
			// re-arms a fresh (cleared) contribution round.
			if (drains_pending)
				continue;

			PROFILE_FUNCTION_NAMED("Listener mixing");

			// "Contributors done" is derived, not counted: a relevant, in-range,
			// already-simulated source whose mix isn't ready yet leaves the round
			// pending. When nothing remains pending, the push_buffer is complete.
			bool any_contributor_pending = false;
			for (auto *sd : sources) {
				if ((sd->cfg.layers & ld->cfg.mask) == 0)
					continue;

				SourceListenerData *sld = nullptr;
				for (auto &entry : sd->listener_data) {
					if (entry.listener == ld) {
						sld = &entry;
						break;
					}
				}

				if (!sld)
					continue;

				// Already contributed to this generation?
				if (sld->last_contributed_generation == ld->generation)
					continue;

				// Out of range — counts as done for this round, adds no audio, and must
				// NEVER block: an idle out-of-range source stops mixing and resets its
				// mix, so it sits at mixed_frames_ready < frame_size indefinitely. This
				// check must precede the readiness check below, or the round would stall
				// the moment a source leaves range. (Out-of-range pairs are excluded from
				// the source's consumer scan, so no last_consumed_mix stamp is needed.)
				if (sld->out_of_range) {
					sld->last_contributed_generation = ld->generation;
					continue;
				}

				// Not simulated yet: has no valid audio/effects. Mark it done for this
				// generation (and consumed) so it never blocks the round. Phase 2 also
				// withholds its mixing, so no audio is consumed meanwhile.
				if (!sld->direct_simulated_once) {
					sld->last_contributed_generation = ld->generation;
					sld->last_consumed_mix = sd->mix_generation;
					continue;
				}

				if (sd->mixed_frames_ready < frame_size) {
					// In range and simulated, but its mix isn't ready yet — the round
					// legitimately waits for this source to complete its mix.
					any_contributor_pending = true;
					continue;
				}

				// In range, simulated, ready — contribute.
				sld->last_contributed_generation = ld->generation;

				// Copy pre-mixed source audio into SteamAudio input buffer
				for (int s = 0; s < frame_size; ++s) {
					Vector2 f = sd->mixed_frames[s];
					sld->input_buffer.data[0][s] = (f.x + f.y) * 0.5f;
				}

				sld->debug_times_contributed += 1;

				// This pair has now consumed the source's current mix for this round.
				// (Stamped at the contribute point, not at playback drain, so a listener
				// whose ring buffer is full still releases the source's mix-reset gate.)
				sld->last_consumed_mix = sd->mix_generation;

				// Direct effects
				if (sd->cfg.direct_enabled && sld->source && sld->direct_effect) {
					PROFILE_FUNCTION_NAMED("direct effect processing")
					IPLSimulationOutputs outputs{};
					iplSourceGetOutputs(sld->source, IPL_SIMULATIONFLAGS_DIRECT, &outputs);

					IPLDirectEffectParams direct_params = outputs.direct;
					direct_params.flags = static_cast<IPLDirectEffectFlags>(0);

					if (sd->cfg.occlusion_enabled) {
						direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION);
					}

					if (sd->cfg.transmission_enabled) {
						direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
						direct_params.transmissionType = static_cast<IPLTransmissionType>(sd->cfg.transmission_type);
					}

					if (sd->cfg.air_absorption_enabled) {
						direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);

						IPLAirAbsorptionModel airAbsorptionModel{};
						airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;

						iplAirAbsorptionCalculate(phonon_context, sd->cached_coords.origin, ld->cached_coords.origin, &airAbsorptionModel, direct_params.airAbsorption);
					}

					if (sd->cfg.distance_attenuation_enabled) {
						direct_params.flags = static_cast<IPLDirectEffectFlags>(direct_params.flags | IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION);

						float dist = sld->dist_to_listener;
						float min_dist = sd->cfg.distance_attenuation_min;
						float max_dist = sd->cfg.distance_attenuation_max;

						float attenuation = calculate_attenuation(dist, min_dist, max_dist);
						direct_params.distanceAttenuation = attenuation;
					} else {
						direct_params.distanceAttenuation = 1.0f;
					}

					iplDirectEffectApply(sld->direct_effect, &direct_params, &sld->input_buffer, &sld->input_buffer);

					// Binaural spatialization
					if (sd->cfg.binaural_enabled) {
						IPLBinauralEffectParams params{};

						params.direction = iplCalculateRelativeDirection(phonon_context,
								sd->cached_coords.origin,
								ld->cached_coords.origin,
								ld->cached_coords.ahead,
								ld->cached_coords.up);
						params.interpolation = static_cast<IPLHRTFInterpolation>(sd->cfg.binaural_interpolation);
						params.spatialBlend = sd->cfg.binaural_spatial_blend;
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
							ld->push_buffer[s].x += sld->output_buffer.data[0][s];
							ld->push_buffer[s].y += sld->output_buffer.data[1][s];
						}
					} else {
						for (int s = 0; s < frame_size; ++s) {
							ld->push_buffer[s].x += sld->input_buffer.data[0][s];
							ld->push_buffer[s].y += sld->input_buffer.data[0][s];
						}
					}
				}

				// Reflections
				if (sd->cfg.reflection_enabled && sld->source && sld->reflection_effect) {
					PROFILE_FUNCTION_NAMED("Reflection Effect Processing");
					IPLSimulationOutputs outputs{};
					iplSourceGetOutputs(sld->source, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);

					IPLReflectionEffectParams refl_params = outputs.reflections;
					refl_params.type = static_cast<IPLReflectionEffectType>(ld->cfg.refl_type);
					refl_params.numChannels = ambisonic_channels_from(ld->cfg.refl_ambisonics_order);
					// changing the irSize here doesn't seem to do anything, unfortunately
					float stress_irsize_factor = 0.2f + 0.8f * (1.0f - stress_mitigation.load());
					refl_params.irSize = static_cast<int>(static_cast<float>(refl_params.irSize) * stress_irsize_factor);

					if (refl_params.irSize > 0) {
						// the only way to really mitigate the stress of iplReflectionEffectApply seems to
						// be to just not call it. but that has to be done en-block, otherwise heavy artifacting
						// will occur.
						if (sld->skip_reflection_applies > 0)
							sld->skip_reflection_applies--;
						else {
							iplReflectionEffectApply(sld->reflection_effect, &refl_params, &sld->input_buffer, &sld->ambisonics_buffer, ld->reflection_mixer);
							if (!ld->reflection_mixer && sld->ambisonics_decode_effect) {
								IPLAmbisonicsDecodeEffectParams decode_params{};
								decode_params.order = ld->cfg.refl_ambisonics_order;
								decode_params.hrtf = phonon_hrtf;
								decode_params.orientation = ld->cached_coords;
								decode_params.binaural = IPL_TRUE;

								iplAmbisonicsDecodeEffectApply(sld->ambisonics_decode_effect, &decode_params, &sld->ambisonics_buffer, &sld->output_buffer);

								for (int s = 0; s < frame_size; ++s) {
									ld->push_buffer[s].x += sld->output_buffer.data[0][s];
									ld->push_buffer[s].y += sld->output_buffer.data[1][s];
								}
							}
						}
					}
				}
			}

			// Deadline override: a source that can't complete a full frame_size mix (e.g. a
			// momentarily starved VoIP playback) would otherwise keep any_contributor_pending
			// set forever and freeze this listener's output. Once the most-drained playback's
			// ring buffer is down to ~one callback (frame_size) of runway, push what we have
			// rather than underrun. The starved source is skipped (not stamped, partial mix
			// kept) so it contributes in a later round once full.
			if (any_contributor_pending) {
				int min_avail = INT_MAX;
				{
					std::lock_guard pb_lock(*ld->playbacks_mutex);
					for (const auto &pb : ld->playbacks) {
						if (pb.playback->is_playing())
							min_avail = MIN(min_avail, pb.playback->get_available_buffer_size());
					}
				}
				if (min_avail != INT_MAX && min_avail <= frame_size)
					any_contributor_pending = false;
			}

			// If all contributors are done for this listener, finish reflections processing
			if (!any_contributor_pending && ld->reflection_mixer && ld->ambisonics_decode_effect) {
				PROFILE_FUNCTION_NAMED("Reflection Mixer Processing");
				IPLReflectionEffectParams apply_refl_params{};
				apply_refl_params.type = static_cast<IPLReflectionEffectType>(ld->cfg.refl_type);
				apply_refl_params.numChannels = ambisonic_channels_from(ld->cfg.refl_ambisonics_order);

				iplReflectionMixerApply(ld->reflection_mixer, &apply_refl_params, &ld->mixed_ambisonics_buffer);

				IPLAmbisonicsDecodeEffectParams decode_params{};
				decode_params.order = ld->cfg.refl_ambisonics_order;
				decode_params.hrtf = phonon_hrtf;
				decode_params.orientation = ld->cached_coords;
				decode_params.binaural = IPL_TRUE;

				iplAmbisonicsDecodeEffectApply(ld->ambisonics_decode_effect, &decode_params, &ld->mixed_ambisonics_buffer, &ld->decode_output_buffer);

				for (int s = 0; s < frame_size; ++s) {
					ld->push_buffer[s].x += ld->decode_output_buffer.data[0][s];
					ld->push_buffer[s].y += ld->decode_output_buffer.data[1][s];
				}
			}

			// All contributors done — start draining push_buffer to playbacks
			if (!any_contributor_pending) {
				std::lock_guard pb_lock(*ld->playbacks_mutex);

				for (int i = (int)ld->playbacks.size() - 1; i >= 0; --i) {
					if (!ld->playbacks[i].playback->is_playing()) {
						ld->playbacks.remove_at(i);
					}
				}

				for (int i = (int)ld->playbacks.size() - 1; i >= 0; --i) {
					auto &pb = ld->playbacks[i];
					pb.remaining_from_push_buffer = frame_size;
					int free_available_in_buffer = pb.playback->get_free_buffer_size();
					if (frame_size <= free_available_in_buffer) {
						pb.playback->push_buffer(ld->push_buffer);
						pb.remaining_from_push_buffer = 0;
						pb.debug_times_drained += 1;
					} else {
						int num_to_push = MIN(frame_size, free_available_in_buffer);
						if (num_to_push > 0) {
							pb.playback->push_buffer(ld->push_buffer.slice(0, num_to_push));
							pb.remaining_from_push_buffer -= num_to_push;
						}
					}
				}

				// All playbacks consumed immediately — clear and re-arm the next round.
				// (Bumping the generation makes every pair pending again next cycle.)
				bool still_pending = false;
				for (const auto &pb : ld->playbacks) {
					if (pb.remaining_from_push_buffer > 0) {
						still_pending = true;
						break;
					}
				}
				if (!still_pending) {
					ld->debug_times_pushed += 1;
					ld->push_buffer.fill(Vector2(0, 0));
					ld->generation++;
				}
			}
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Listener RID API
// ─────────────────────────────────────────────────────────────────────────────

RID SteamAudioServer::listener_create() {
	ListenerData *ld = memnew(ListenerData);
	RID rid = listener_owner.make_rid(ld);
	ld->self = rid;
	ld->debug_name = "listener_" + String::num_uint64(rid.get_id());
	{
		std::lock_guard lock(pending_ops_mutex);
		pending_ops.push_back(PendingAddListener{ rid });
	}
	return rid;
}

void SteamAudioServer::listener_free(RID listener) {
	std::lock_guard lock(pending_ops_mutex);
	pending_ops.push_back(PendingRemoveListener{ listener });
}

void SteamAudioServer::listener_set_transform(RID listener, const Transform3D &xform) {
	// No collections_mutex: these per-RID scalar setters are called every frame
	// from the owning node (main thread). The shared lock never excluded the
	// (also-shared-locking) readers anyway; it only blocked apply_pending_ops's
	// unique lock, starving it under per-frame setter traffic. get_or_null is
	// internally thread-safe and the owner won't free a struct its node still uses.
	ListenerData *ld = listener_owner.get_or_null(listener);
	if (!ld)
		return;
	ld->pending_transform = xform;
	ld->has_transform = true;
}

void SteamAudioServer::listener_set_mask(RID listener, uint32_t mask) {
	ListenerData *ld = listener_owner.get_or_null(listener);
	if (!ld)
		return;
	ld->cfg.mask = mask;
}

void SteamAudioServer::listener_set_range(RID listener, float range) {
	ListenerData *ld = listener_owner.get_or_null(listener);
	if (!ld)
		return;
	ld->cfg.range = range;
}

void SteamAudioServer::listener_set_sensor(RID listener, bool enabled) {
	ListenerData *ld = listener_owner.get_or_null(listener);
	if (!ld)
		return;
	ld->cfg.is_sensor = enabled;
}

void SteamAudioServer::listener_set_reflection(RID listener, bool enabled, int rays, int bounces, float duration, int ambisonics_order, int type, float irradiance_min_dist) {
	ListenerData *ld = listener_owner.get_or_null(listener);
	if (!ld)
		return;
	ld->cfg.reflection_simulation_enabled = enabled;
	ld->cfg.num_refl_rays = rays;
	ld->cfg.num_refl_bounces = bounces;
	ld->cfg.refl_duration = duration;
	ld->cfg.refl_ambisonics_order = ambisonics_order;
	ld->cfg.refl_type = type;
	ld->cfg.irradiance_min_dist = irradiance_min_dist;
}

void SteamAudioServer::listener_set_debug_name(RID listener, const String &name) {
	ListenerData *ld = listener_owner.get_or_null(listener);
	if (!ld)
		return;
	ld->debug_name = name;
}

void SteamAudioServer::listener_add_playback(RID listener, Ref<AudioStreamSteamAudioListenerPlayback> playback) {
	std::lock_guard lock(pending_ops_mutex);
	pending_ops.push_back(PendingAddPlaybackToListener{ listener, playback });
}

Array SteamAudioServer::listener_get_source_db_levels(RID listener) {
	std::shared_lock lock(collections_mutex);
	Array result;
	ListenerData *ld = listener_owner.get_or_null(listener);
	if (!ld)
		return result;
	for (const auto &lvl : ld->source_db_levels) {
		Dictionary d;
		d["source"] = lvl.source;
		d["position"] = lvl.position;
		d["db_level"] = lvl.db_level;
		result.push_back(d);
	}
	return result;
}

const LocalVector<ListenerSourceDBLevel> &SteamAudioServer::listener_get_source_db_levels_ref(RID listener) {
	ListenerData *ld = listener_owner.get_or_null(listener);
	if (ld)
		return ld->source_db_levels;
	static LocalVector<ListenerSourceDBLevel> empty;
	empty.clear();
	return empty;
}

// ─────────────────────────────────────────────────────────────────────────────
// Source RID API
// ─────────────────────────────────────────────────────────────────────────────

RID SteamAudioServer::source_create() {
	SourceData *sd = memnew(SourceData);
	RID rid = source_owner.make_rid(sd);
	sd->self = rid;
	sd->debug_name = "source_" + String::num_uint64(rid.get_id());
	sd->mixed_frames.resize(cached_audio_settings.frameSize);
	sd->mixed_frames.fill(Vector2(0, 0));
	sd->volume_linear = UtilityFunctions::db_to_linear(sd->cfg.volume_db);
	{
		std::lock_guard lock(pending_ops_mutex);
		pending_ops.push_back(PendingAddSource{ rid });
	}
	return rid;
}

void SteamAudioServer::source_free(RID source) {
	std::lock_guard lock(pending_ops_mutex);
	pending_ops.push_back(PendingRemoveSource{ source });
}

void SteamAudioServer::source_set_transform(RID source, const Transform3D &xform) {
	// No collections_mutex — see listener_set_transform for the rationale.
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->pending_transform = xform;
	sd->has_transform = true;
}

void SteamAudioServer::source_set_layers(RID source, uint32_t layers) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->cfg.layers = layers;
}

void SteamAudioServer::source_set_volume_db(RID source, float volume_db) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->cfg.volume_db = volume_db;
	sd->volume_linear = UtilityFunctions::db_to_linear(volume_db);
}

void SteamAudioServer::source_set_doppler_factor(RID source, float factor) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->cfg.doppler_factor = factor;
}

void SteamAudioServer::source_set_direct_enabled(RID source, bool enabled) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->cfg.direct_enabled = enabled;
}

void SteamAudioServer::source_set_binaural(RID source, bool enabled, int interpolation, float spatial_blend) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->cfg.binaural_enabled = enabled;
	sd->cfg.binaural_interpolation = interpolation;
	sd->cfg.binaural_spatial_blend = spatial_blend;
}

void SteamAudioServer::source_set_distance_attenuation(RID source, bool enabled, float min, float max) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->cfg.distance_attenuation_enabled = enabled;
	sd->cfg.distance_attenuation_min = min;
	sd->cfg.distance_attenuation_max = max;
}

void SteamAudioServer::source_set_air_absorption(RID source, bool enabled) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->cfg.air_absorption_enabled = enabled;
}

void SteamAudioServer::source_set_occlusion(RID source, bool enabled, int type, float radius, int samples) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->cfg.occlusion_enabled = enabled;
	sd->cfg.occlusion_type = type;
	sd->cfg.occlusion_radius = radius;
	sd->cfg.occlusion_samples = samples;
}

void SteamAudioServer::source_set_transmission(RID source, bool enabled, int type, int rays) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->cfg.transmission_enabled = enabled;
	sd->cfg.transmission_type = type;
	sd->cfg.transmission_rays = rays;
}

void SteamAudioServer::source_set_reflection(RID source, bool enabled, float duration, float hybrid_delay) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->cfg.reflection_enabled = enabled;
	sd->cfg.reflection_duration = duration;
	sd->cfg.reflection_hybrid_delay = hybrid_delay;
}

void SteamAudioServer::source_set_effect_stack(RID source, const TypedArray<AudioEffect> &stack) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	PROFILE_FUNCTION_NAMED("instantiate_AudioEffects");
	sd->effect_instances.clear();
	for (int i = 0; i < stack.size(); ++i) {
		Ref<AudioEffect> effect = stack[i];
		if (effect.is_valid()) {
			Ref<AudioEffectInstance> inst = effect->instantiate();
			if (inst.is_valid()) {
				sd->effect_instances.push_back(inst);
			}
		}
	}
}

void SteamAudioServer::source_set_debug_name(RID source, const String &name) {
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	sd->debug_name = name;
}

void SteamAudioServer::source_add_playback(RID source, Ref<AudioStreamPlayback> playback, float p_volume_db, float p_pitch_scale) {
	if (playback.is_null())
		return;

	float volume_linear = UtilityFunctions::db_to_linear(p_volume_db);
	playback->start();

	std::lock_guard lock(pending_ops_mutex);
	pending_ops.push_back(PendingAddPlaybackToSource{ source, playback, volume_linear, p_pitch_scale });
}

void SteamAudioServer::source_set_playback_volume(RID source, Ref<AudioStreamPlayback> p_playback, float p_volume_db) {
	std::shared_lock lock(collections_mutex);
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	for (auto &entry : sd->playbacks) {
		if (entry.playback == p_playback) {
			entry.volume_linear = UtilityFunctions::db_to_linear(p_volume_db);
			return;
		}
	}
}

void SteamAudioServer::source_set_playback_pitch(RID source, Ref<AudioStreamPlayback> p_playback, float p_pitch_scale) {
	std::shared_lock lock(collections_mutex);
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return;
	for (auto &entry : sd->playbacks) {
		if (entry.playback == p_playback) {
			entry.pitch_scale = p_pitch_scale;
			return;
		}
	}
}

int SteamAudioServer::source_get_num_active_playbacks(RID source) {
	std::shared_lock lock(collections_mutex);
	SourceData *sd = source_owner.get_or_null(source);
	if (!sd)
		return 0;
	// Count only playing playbacks. Finished playbacks linger in the list until a
	// process_audio phase cleans them up; if that cleanup is delayed/skipped, a
	// stale count here would keep a dynamic-registration source registered forever
	// (it never times out), leaking sources over time.
	int n = 0;
	for (auto &pb : sd->playbacks) {
		if (pb.playback.is_valid() && pb.playback->is_playing())
			n++;
	}
	return n;
}

// ─────────────────────────────────────────────────────────────────────────────
// Geometry RID API
// ─────────────────────────────────────────────────────────────────────────────

RID SteamAudioServer::geometry_create_internal(const PackedVector3Array &verts, const PackedInt32Array &tris, const PackedFloat32Array &material, bool dynamic) {
	if (!phonon_scene || !phonon_context) {
		UtilityFunctions::push_error("SteamAudio: scene is not initialized");
		return RID();
	}

	IPLMaterial mat = ipl_material_from_floats(material);

	GeometryData *g = memnew(GeometryData);
	g->dynamic = dynamic;

	if (dynamic) {
		IPLSceneSettings sub_scene_cfg{};
		sub_scene_cfg.radeonRaysDevice = nullptr;
		sub_scene_cfg.type = static_cast<IPLSceneType>(get_project_int("steamaudio/scene_type", IPL_SCENETYPE_DEFAULT));
		if (sub_scene_cfg.type == IPL_SCENETYPE_EMBREE) {
			if (embree_dev == nullptr) {
				memdelete(g);
				ERR_FAIL_V_MSG(RID(), "SteamAudio: geometry_create_dynamic with scene_type EMBREE and uninitialized embree device.");
			}
			sub_scene_cfg.embreeDevice = embree_dev;
		}
		if (!handleErr(iplSceneCreate(phonon_context, &sub_scene_cfg, &g->sub_scene), "SteamAudio: Failed to create sub-scene")) {
			memdelete(g);
			return RID();
		}

		IPLStaticMesh m = create_ipl_mesh_from_raw(g->sub_scene, verts, tris, mat);
		if (m) {
			g->meshes.push_back(m);
			iplStaticMeshAdd(m, g->sub_scene);
		}
		iplSceneCommit(g->sub_scene);

		IPLInstancedMeshSettings inst_cfg{};
		inst_cfg.subScene = g->sub_scene;
		inst_cfg.transform = ipl_mat4_from(Transform3D());
		handleErr(iplInstancedMeshCreate(phonon_scene, &inst_cfg, &g->instanced_mesh), "SteamAudio: Failed to create instanced mesh");
		iplInstancedMeshAdd(g->instanced_mesh, phonon_scene);
	} else {
		IPLStaticMesh m = create_ipl_mesh_from_raw(phonon_scene, verts, tris, mat);
		if (m) {
			g->meshes.push_back(m);
			iplStaticMeshAdd(m, phonon_scene);
		}
		if (g->meshes.empty()) {
			memdelete(g);
			return RID();
		}
	}

	RID rid = geometry_owner.make_rid(g);
	g->self = rid;

	{
		std::lock_guard lock(pending_ops_mutex);
		pending_ops.push_back(PendingAddGeometry{ rid });
	}
	return rid;
}

RID SteamAudioServer::geometry_create_static(const PackedVector3Array &verts, const PackedInt32Array &tris, const PackedFloat32Array &material) {
	return geometry_create_internal(verts, tris, material, false);
}

RID SteamAudioServer::geometry_create_dynamic(const PackedVector3Array &verts, const PackedInt32Array &tris, const PackedFloat32Array &material) {
	return geometry_create_internal(verts, tris, material, true);
}

void SteamAudioServer::geometry_set_transform(RID geometry, const Transform3D &xform) {
	GeometryData *g = geometry_owner.get_or_null(geometry);
	if (!g)
		return;
	g->pending_transform = xform;
	g->has_transform = true;
}

void SteamAudioServer::geometry_free(RID geometry) {
	std::lock_guard lock(pending_ops_mutex);
	pending_ops.push_back(PendingRemoveGeometry{ geometry });
}
