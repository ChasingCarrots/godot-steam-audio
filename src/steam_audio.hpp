#ifndef STEAM_AUDIO_H
#define STEAM_AUDIO_H

#include "godot_cpp/variant/transform3d.hpp"
#include <phonon.h>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <shared_mutex>

using namespace godot;

class SteamAudio {
public:
	typedef enum {
		log_debug,
		log_info,
		log_warn,
		log_error
	} GodotSteamAudioLogLevel;

	static void log(GodotSteamAudioLogLevel lvl, const char *str);
};

struct GlobalSteamAudioState {
	IPLScene scene;
	IPLAudioSettings audio_cfg;
	IPLContext ctx;
	IPLHRTF hrtf;
	IPLAmbisonicsEncodeEffect ambi_enc_effect;
	IPLAmbisonicsDecodeEffect ambi_dec_effect;
	IPLSimulationSettings sim_cfg;
	IPLSimulator sim;
	IPLCoordinateSpace3 listener_coords;
	std::mutex simulation_lock;
};

struct SteamAudioSource {
	AudioStreamPlayer3D *player = nullptr;
	IPLSource simulationSource;
};

struct SteamAudioSourceConfig {
	float occ_radius;
	int occ_samples;
	int transm_rays;
	float min_attn_dist;
	float max_attn_dist;
	int ambisonics_order;
	float max_refl_dist;
	float hybrid_reverb_transition_time;
	float hybrid_reverb_overlap_percent;
	bool is_dist_attn_on;
	bool is_ambisonics_on;
	bool is_occlusion_on;
	bool is_reflection_on;
	bool is_air_absorption_on;
	bool is_directivity_on;
	float directivity_dipole_weight;
	float directivity_dipole_power;
	bool is_transmission_on;
	int transmission_type;
	bool is_binaural_on;
	bool is_binaural_interpolation_on;
	bool skip_direct_audio;
};

struct SteamAudioEffects {
	IPLPanningEffect panning;
	IPLDirectEffect direct;
	IPLBinauralEffect binaural;
	IPLReflectionEffect refl;
	IPLAmbisonicsDecodeEffect ambisonics;
};

struct LocalSteamAudioBuffers {
	IPLAudioBuffer in;
	IPLAudioBuffer direct;
	IPLAudioBuffer mono;
	IPLAudioBuffer refl;
	IPLAudioBuffer refl_out;
	IPLAudioBuffer out;
};

struct LocalSteamAudioState {
	SteamAudioSource src;
	LocalSteamAudioBuffers bufs;
	SteamAudioEffects fx;
	SteamAudioSourceConfig cfg {
		4.0f,
		32,
		16,
		1.0f,
		10.0f,
		1,
		10000.0f,
		1.0f,
		0.25f,
		false,
		true,
		true,
		false,
		false,
		false,
		0.0f,
		0.0f,
		false,
		0,
		false,
		false,
		false
	};
	IPLHRTFInterpolation hrtfInterpolation;
	std::shared_mutex mux;
};

inline int ambisonic_channels_from(int order) {
	return (order + 1) * (order + 1);
}

inline IPLVector3 ipl_vec3_from(Vector3 v) { return IPLVector3{ v.x, v.y, v.z }; }

inline IPLCoordinateSpace3 ipl_coords_from(Transform3D trf) {
	auto orig = trf.origin;
	auto right = trf.get_basis().get_column(0);
	auto up = trf.get_basis().get_column(1);
	auto fwd = -trf.get_basis().get_column(2);

	IPLCoordinateSpace3 coords;
	coords.origin = ipl_vec3_from(orig);
	coords.right = ipl_vec3_from(right);
	coords.up = ipl_vec3_from(up);
	coords.ahead = ipl_vec3_from(fwd);

	return coords;
}

inline void handleErr(IPLerror err) {
	switch (err) {
		case IPL_STATUS_SUCCESS:
			return;
		case IPL_STATUS_FAILURE:
			SteamAudio::log(SteamAudio::log_error, "Unspecified error in init");
			return;
		case IPL_STATUS_OUTOFMEMORY:
			SteamAudio::log(SteamAudio::log_error, "Out of memory in init");
			return;
		case IPL_STATUS_INITIALIZATION:
			SteamAudio::log(SteamAudio::log_error, "Failed to handle external dependency in init");
			return;
	}
}

#endif
