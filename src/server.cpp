#include "server.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/project_settings.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/core/memory.hpp"
#include "godot_cpp/variant/callable_method_pointer.hpp"
#include "phonon.h"
#include "server_init.hpp"
#include "steam_audio.hpp"
#include <algorithm>
#include <godot_cpp/variant/utility_functions.hpp>
#include "profiling.h"

void SteamAudioServer::tick() {
	PROFILE_FUNCTION()
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	if (!self->is_global_state_init.load()) {
		return;
	}
	if (self->listener == nullptr || !self->listener->is_inside_tree()) {
		return;
	}

	SteamAudio::log(SteamAudio::log_debug, "tick");

	self->global_state.listener_coords =
			ipl_coords_from(self->listener->get_global_transform());

	for (auto ls : self->local_states) {
		if (ls->src.player == nullptr) {
			UtilityFunctions::push_warning(
					"local state has empty player, not updating simulation state");
		}
		if (!ls->src.player->is_playing() || !ls->src.player->is_inside_tree()) {
			continue;
		}

		IPLSimulationInputs inputs{};

		if (ls->cfg.is_dist_attn_on) {
			IPLDistanceAttenuationModel attn_model{};
			attn_model.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
			attn_model.minDistance = ls->cfg.min_attn_dist;
			inputs.distanceAttenuationModel = attn_model;
		}

		IPLCoordinateSpace3 src_coords = ipl_coords_from(ls->src.player->get_global_transform());

		inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
		if (ls->cfg.is_dist_attn_on)
			inputs.directFlags = static_cast<IPLDirectSimulationFlags>( inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION);
		if (ls->cfg.is_occlusion_on) {
			inputs.directFlags = static_cast<IPLDirectSimulationFlags>( inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION);
			inputs.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
			inputs.occlusionRadius = ls->cfg.occ_radius;
			inputs.numOcclusionSamples = ls->cfg.occ_samples;
			inputs.numTransmissionRays = ls->cfg.transm_rays;
		}
		if (ls->cfg.is_transmission_on)
			inputs.directFlags = static_cast<IPLDirectSimulationFlags>( inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);

		inputs.source = src_coords;

		SteamAudio::log(SteamAudio::log_debug, "tick: setting inputs");
		iplSourceSetInputs(ls->src.simulationSource, IPL_SIMULATIONFLAGS_DIRECT, &inputs);
	}
	SteamAudio::log(SteamAudio::log_debug, "tick: direct inputs set");

	IPLSimulationSharedInputs shared_inputs{};
	shared_inputs.listener = self->global_state.listener_coords;
	iplSimulatorSetSharedInputs(self->global_state.sim,
			IPL_SIMULATIONFLAGS_DIRECT, &shared_inputs);

	for (auto ls : self->local_states) {
		if (ls->src.player == nullptr) {
			UtilityFunctions::push_warning(
					"local state has empty player, not updating simulation state");
		}
		if (!ls->src.player->is_playing()) {
			continue;
		}
		if (!ls->cfg.is_reflection_on) {
			continue;
		}
		if (ls->src.player->get_global_position().distance_to(listener->get_global_position()) > ls->cfg.max_refl_dist) {
			continue;
		}

		Vector3 src_pos = ls->src.player->get_global_position();
		IPLCoordinateSpace3 src_coords;
		src_coords.ahead = IPLVector3{};
		src_coords.up = IPLVector3{};
		src_coords.right = IPLVector3{};
		src_coords.origin = ipl_vec3_from(src_pos);

		IPLSimulationInputs inputs{};
		inputs.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
		inputs.source = src_coords;

		iplSourceSetInputs(ls->src.simulationSource, IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);
	}

	shared_inputs = IPLSimulationSharedInputs{};
	shared_inputs.listener = global_state.listener_coords;
	shared_inputs.numRays = listener->get_num_refl_rays();
	shared_inputs.numBounces = listener->get_num_refl_bounces();
	shared_inputs.duration = listener->get_refl_duration();
	shared_inputs.order = listener->get_refl_ambisonics_order();
	shared_inputs.irradianceMinDistance = listener->get_irradiance_min_dist();
	iplSimulatorSetSharedInputs(global_state.sim, IPL_SIMULATIONFLAGS_REFLECTIONS, &shared_inputs);

	if (refl_thread_wait_for_commit.load() && !is_refl_thread_processing.load()) {
		if (scene_dirty) {
			PROFILE_FUNCTION_NAMED(ipl_Scene_Commit);
			iplSceneCommit(self->global_state.scene);
			scene_dirty = false;
		}
		if (simulator_dirty) {
			PROFILE_FUNCTION_NAMED(ipl_Simulator_Commit);
			iplSimulatorCommit(global_state.sim);
			simulator_dirty = false;
		}
		refl_thread_wait_for_commit.store(false);
		// do not notify the thread right away, but only after
		// we set the inputs (which requires notifying it anyways)
	}

	SteamAudio::log(SteamAudio::log_debug, "tick: committed scene");

	{
		PROFILE_FUNCTION_NAMED(run_simulator_direct)
		iplSimulatorRunDirect(self->global_state.sim);
	}

	SteamAudio::log(SteamAudio::log_debug, "tick: direct sim complete");

	new_inputs_set.store(true);
	if (!is_refl_thread_processing.load()) {
		// notify the thread of new inputs, so that it runs for another round
		std::unique_lock<std::mutex> lock(refl_mux);
		cv.notify_one();
	}

	SteamAudio::log(SteamAudio::log_debug, "tick: done");
}

GlobalSteamAudioState *SteamAudioServer::get_global_state(bool should_init) {
	self->init_mux.lock();
	if (self->is_global_state_init.load()) {
		self->init_mux.unlock();
		return &self->global_state;
	}

	if (!should_init) {
		self->init_mux.unlock();
		return nullptr;
	}

	SteamAudio::log(SteamAudio::log_info, "Initializing SteamAudioServer global state");

	global_state.audio_cfg = create_audio_cfg();
	global_state.ctx = create_ctx();

	IPLSceneSettings scene_cfg = create_scene_cfg(global_state.ctx);
	IPLerror err = iplSceneCreate(global_state.ctx, &scene_cfg, &global_state.scene);
	handleErr(err);
	global_state.scene = iplSceneRetain(global_state.scene);
	for (auto m : static_meshes_to_add) {
		iplStaticMeshAdd(m, global_state.scene);
	}

	global_state.sim = create_simulator(
			global_state.ctx, global_state.audio_cfg, scene_cfg);
	global_state.hrtf = create_hrtf(global_state.ctx, global_state.audio_cfg);
	global_state.ambi_enc_effect = create_ambisonics_encode_effect(
			global_state.ctx, global_state.audio_cfg);
	global_state.ambi_dec_effect = create_ambisonics_decode_effect(
			global_state.ctx, global_state.audio_cfg, global_state.hrtf);

	iplSimulatorSetScene(global_state.sim, global_state.scene);
	iplSimulatorCommit(global_state.sim);

	is_global_state_init.store(true);
	init_mux.unlock();

	SteamAudio::log(SteamAudio::log_info, "Initialized SteamAudioServer global state");
	start_refl_sim();
	return &global_state;
}

void SteamAudioServer::start_refl_sim() {
	if(refl_thread.is_valid()) {
		SteamAudio::log(SteamAudio::log_error, "Refl thread already running");
		return;
	}
	SteamAudio::log(SteamAudio::log_info, "Creating and starting reflection simulation thread.");
	refl_thread.instantiate();
	refl_thread->start(callable_mp(this, &SteamAudioServer::run_refl_sim));
}

void SteamAudioServer::run_refl_sim() {
	PROFILING_THREAD("run_refl_sim")
	while (this->is_running.load()) {
		if(refl_thread_wait_for_commit.load() || !new_inputs_set.load()) {
			std::unique_lock<std::mutex> lock(this->refl_mux);
			cv.wait(lock, [&] { return
				(!refl_thread_wait_for_commit.load() && new_inputs_set.load()) ||
				!is_running.load(); });
			if(!is_running.load())
				break;
		}
		{
			PROFILE_FUNCTION_NAMED(run_simulation_refl)
			is_refl_thread_processing.store(true);
			new_inputs_set.store(false);
			SteamAudio::log(SteamAudio::log_debug, "running reflection sim");
			iplSimulatorRunReflections(global_state.sim);
			is_refl_thread_processing.store(false);
		}
	}
}

void SteamAudioServer::add_listener(SteamAudioListener *lis) {
	self->listener = lis;
	simulator_dirty = true;
	refl_thread_wait_for_commit.store(true);
}

void SteamAudioServer::add_local_state(LocalSteamAudioState *ls) {
	self->local_states.push_back(ls);
	simulator_dirty = true;
	refl_thread_wait_for_commit.store(true);
}

void SteamAudioServer::remove_local_state(LocalSteamAudioState *ls) {
	auto it = std::find(local_states.begin(), local_states.end(), ls);
	if (it == local_states.end()) {
		return;
	}
	local_states.erase(it);
	simulator_dirty = true;
	refl_thread_wait_for_commit.store(true);
}

void SteamAudioServer::add_static_mesh(IPLStaticMesh mesh) {
	if (is_global_state_init.load()) {
		iplStaticMeshAdd(mesh, global_state.scene);
		scene_dirty = true;
		refl_thread_wait_for_commit.store(true);
	} else {
		static_meshes_to_add.push_back(mesh);
	}
}

void SteamAudioServer::remove_static_mesh(IPLStaticMesh mesh) {
	if (is_global_state_init.load()) {
		iplStaticMeshRemove(mesh, global_state.scene);
		scene_dirty = true;
		refl_thread_wait_for_commit.store(true);
	} else {
		// Probably won't happen?
		auto it = std::find(static_meshes_to_add.begin(), static_meshes_to_add.end(), mesh);
		if (it != static_meshes_to_add.end()) {
			static_meshes_to_add.erase(it);
		}
	}
}

void SteamAudioServer::add_dynamic_mesh(IPLInstancedMesh mesh) {
	if (is_global_state_init.load()) {
		iplInstancedMeshAdd(mesh, global_state.scene);
		scene_dirty = true;
		refl_thread_wait_for_commit.store(true);
	} else {
		SteamAudio::log(SteamAudio::log_error, "Adding a dynamic mesh, but SteamAudio is not initialized. Probably crashing soon.");
	}
}

void SteamAudioServer::remove_dynamic_mesh(IPLInstancedMesh mesh) {
	if (!is_global_state_init.load()) {
		return; // We've probably already deleted the scene.
	}

	iplInstancedMeshRemove(mesh, global_state.scene);
	scene_dirty = true;
	refl_thread_wait_for_commit.store(true);
}

void SteamAudioServer::add_source_to_sim(IPLSource source) {
	iplSourceAdd(source, global_state.sim);
	simulator_dirty = true;
	refl_thread_wait_for_commit.store(true);
	num_sources_in_sim += 1;
	// print_line("adding source to sim. num_in_sim: ", num_sources_in_sim);
}

void SteamAudioServer::remove_source_from_sim(IPLSource source) {
	iplSourceRemove(source, global_state.sim);
	simulator_dirty = true;
	refl_thread_wait_for_commit.store(true);
	num_sources_in_sim -= 1;
	// print_line("removing source from sim. num_in_sim: ", num_sources_in_sim);
}

SteamAudioServer::SteamAudioServer() {
	self = this;
	is_global_state_init.store(false);
	is_refl_thread_processing.store(false);
	is_running.store(true);
	refl_thread_wait_for_commit.store(true);
	new_inputs_set.store(false);
}

SteamAudioServer::~SteamAudioServer() {
	is_running.store(false);
	if(refl_thread.is_valid()) {
		{
			std::unique_lock<std::mutex> lock(refl_mux);
			cv.notify_one();
		}
		refl_thread->wait_to_finish();
		refl_thread.unref();
	}

	if (!self->is_global_state_init.load()) {
		return;
	}
	SteamAudio::log(SteamAudio::log_debug, "destroying steam audio server");

	iplAmbisonicsDecodeEffectRelease(&self->global_state.ambi_dec_effect);
	iplAmbisonicsEncodeEffectRelease(&self->global_state.ambi_enc_effect);
	iplHRTFRelease(&self->global_state.hrtf);
	iplSimulatorRelease(&self->global_state.sim);
	iplSceneRelease(&self->global_state.scene);
	iplContextRelease(&self->global_state.ctx);
	self = nullptr;
}

void SteamAudioServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("tick"), &SteamAudioServer::tick);
	ClassDB::bind_static_method("SteamAudioServer", D_METHOD("get_singleton"),
			&SteamAudioServer::get_singleton);
}

SteamAudioServer *SteamAudioServer::get_singleton() {
	return self;
}

SteamAudioServer *SteamAudioServer::self = nullptr;
