#ifndef STEAM_AUDIO_H
#define STEAM_AUDIO_H

#include "godot_cpp/variant/transform3d.hpp"
#include <phonon.h>
#include <godot_cpp/core/class_db.hpp>

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

inline int ambisonic_channels_from(int order) {
	return (order + 1) * (order + 1);
}

inline IPLVector3 ipl_vec3_from(godot::Vector3 v) { return IPLVector3{ v.x, v.y, v.z }; }

inline IPLMatrix4x4 ipl_mat4_from(godot::Transform3D trf) {
	IPLMatrix4x4 m;
	godot::Basis b = trf.basis;
	godot::Vector3 o = trf.origin;

	m.elements[0][0] = b[0][0]; m.elements[0][1] = b[0][1]; m.elements[0][2] = b[0][2]; m.elements[0][3] = o.x;
	m.elements[1][0] = b[1][0]; m.elements[1][1] = b[1][1]; m.elements[1][2] = b[1][2]; m.elements[1][3] = o.y;
	m.elements[2][0] = b[2][0]; m.elements[2][1] = b[2][1]; m.elements[2][2] = b[2][2]; m.elements[2][3] = o.z;
	m.elements[3][0] = 0.0f;    m.elements[3][1] = 0.0f;    m.elements[3][2] = 0.0f;    m.elements[3][3] = 1.0f;

	return m;
}

inline IPLCoordinateSpace3 ipl_coords_from(godot::Transform3D trf) {
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

inline bool handleErr(IPLerror err, const char *context = nullptr) {
	if (err == IPL_STATUS_SUCCESS) return true;
	const char *msg = "Unknown error";
	switch (err) {
		case IPL_STATUS_FAILURE: msg = "Unspecified error"; break;
		case IPL_STATUS_OUTOFMEMORY: msg = "Out of memory"; break;
		case IPL_STATUS_INITIALIZATION: msg = "Failed to handle external dependency"; break;
		default: break;
	}
	if (context) {
		SteamAudio::log(SteamAudio::log_error, context);
	} else {
		SteamAudio::log(SteamAudio::log_error, msg);
	}
	return false;
}

#endif
