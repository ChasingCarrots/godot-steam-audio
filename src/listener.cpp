#include "listener.hpp"
#include "godot_cpp/classes/audio_stream_player.hpp"
#include "godot_cpp/classes/audio_stream_player3d.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/project_settings.hpp"
#include "server.hpp"

using namespace godot;

void SteamAudioListener::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_generator"), &SteamAudioListener::get_generator);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "generator", PROPERTY_HINT_RESOURCE_TYPE, "AudioStreamGenerator"), "", "get_generator");

	ClassDB::bind_method(D_METHOD("play_on_audiostreamplayer", "audiostreamplayer"), &SteamAudioListener::play_on_audiostreamplayer);

	ClassDB::bind_method(D_METHOD("get_reflection_simulation_enabled"), &SteamAudioListener::get_reflection_simulation_enabled);
	ClassDB::bind_method(D_METHOD("set_reflection_simulation_enabled", "p_enabled"), &SteamAudioListener::set_reflection_simulation_enabled);
	ClassDB::bind_method(D_METHOD("get_refl_duration"), &SteamAudioListener::get_refl_duration);
	ClassDB::bind_method(D_METHOD("set_refl_duration", "p_refl_duration"), &SteamAudioListener::set_refl_duration);
	ClassDB::bind_method(D_METHOD("get_irradiance_min_dist"), &SteamAudioListener::get_irradiance_min_dist);
	ClassDB::bind_method(D_METHOD("set_irradiance_min_dist", "p_irradiance_min_dist"), &SteamAudioListener::set_irradiance_min_dist);
	ClassDB::bind_method(D_METHOD("get_num_refl_rays"), &SteamAudioListener::get_num_refl_rays);
	ClassDB::bind_method(D_METHOD("set_num_refl_rays", "p_num_refl_rays"), &SteamAudioListener::set_num_refl_rays);
	ClassDB::bind_method(D_METHOD("get_num_refl_bounces"), &SteamAudioListener::get_num_refl_bounces);
	ClassDB::bind_method(D_METHOD("set_num_refl_bounces", "p_num_refl_bounces"), &SteamAudioListener::set_num_refl_bounces);
	ClassDB::bind_method(D_METHOD("get_refl_ambisonics_order"), &SteamAudioListener::get_refl_ambisonics_order);
	ClassDB::bind_method(D_METHOD("set_refl_ambisonics_order", "p_refl_ambisonics_order"), &SteamAudioListener::set_refl_ambisonics_order);
	ClassDB::bind_method(D_METHOD("get_refl_type"), &SteamAudioListener::get_refl_type);
	ClassDB::bind_method(D_METHOD("set_refl_type", "p_refl_type"), &SteamAudioListener::set_refl_type);
	ClassDB::bind_method(D_METHOD("get_mask"), &SteamAudioListener::get_mask);
	ClassDB::bind_method(D_METHOD("set_mask", "mask"), &SteamAudioListener::set_mask);
	ClassDB::bind_method(D_METHOD("get_range"), &SteamAudioListener::get_range);
	ClassDB::bind_method(D_METHOD("set_range", "range"), &SteamAudioListener::set_range);
	ClassDB::bind_method(D_METHOD("get_buffer_length"), &SteamAudioListener::get_buffer_length);
	ClassDB::bind_method(D_METHOD("set_buffer_length", "buffer_length"), &SteamAudioListener::set_buffer_length);

	ADD_GROUP("Reflection Simulation", "reflection_simulation_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reflection_simulation_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_reflection_simulation_enabled", "get_reflection_simulation_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reflection_simulation_duration", PROPERTY_HINT_RANGE, "0.1,10.0,0.1"), "set_refl_duration", "get_refl_duration");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reflection_simulation_irradiance_min_distance", PROPERTY_HINT_RANGE, "0.1,5.0,0.1"), "set_irradiance_min_dist", "get_irradiance_min_dist");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflection_simulation_rays", PROPERTY_HINT_RANGE, "1,8192,1"), "set_num_refl_rays", "get_num_refl_rays");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflection_simulation_bounces", PROPERTY_HINT_RANGE, "1,64,1"), "set_num_refl_bounces", "get_num_refl_bounces");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflection_simulation_ambisonics_order", PROPERTY_HINT_RANGE, "0,5,1"), "set_refl_ambisonics_order", "get_refl_ambisonics_order");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflection_simulation_type", PROPERTY_HINT_ENUM, "Convolution,Parametric,Hybrid"), "set_refl_type", "get_refl_type");

	ADD_PROPERTY(PropertyInfo(Variant::INT, "mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_mask", "get_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "range", PROPERTY_HINT_RANGE, "0.0,10000.0,0.1,or_greater"), "set_range", "get_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "buffer_length", PROPERTY_HINT_RANGE, "0.01,1.0,0.01"), "set_buffer_length", "get_buffer_length");
}

void SteamAudioListener::ready_internal() {
	generator.instantiate();
	generator->set_mix_rate(ProjectSettings::get_singleton()->get_setting("audio/driver/mix_rate"));
	// we need at least the frame size of the SteamAudioServer as our buffer length, but
	// to be on the safe side, we'll reserve more.
	auto sas = SteamAudioServer::get_singleton();
	float frame_size_seconds = (float)sas->get_frame_size() / (float)sas->get_sampling_rate();
	generator->set_buffer_length(MAX(buffer_length, frame_size_seconds * 1.25f));

	sas->add_listener(this);
}

void SteamAudioListener::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
			if (Engine::get_singleton()->is_editor_hint())
				return;
			ready_internal();
			break;
		case NOTIFICATION_EXIT_TREE:
			if (Engine::get_singleton()->is_editor_hint())
				return;
			SteamAudioServer::get_singleton()->remove_listener(this);
			break;
		case NOTIFICATION_READY: {
			if (Engine::get_singleton()->is_editor_hint())
				return;
			if (get_child_count() > 0) {
				Node *child = get_child(0);
				if (child && (child->is_class("AudioStreamPlayer") || child->is_class("AudioStreamPlayer3D") || child->is_class("AudioStreamPlayer2D") || child->is_class("SteamAudioSource"))) {
					play_on_audiostreamplayer(child);
				}
			}
		} break;
	}
}

Ref<AudioStreamGeneratorPlayback> SteamAudioListener::play_on_audiostreamplayer(godot::Variant audiostreamplayer) {
	Ref<AudioStreamGeneratorPlayback> playback;
	if (!generator.is_valid()) {
		ERR_PRINT("SteamAudioListener: generator is invalid, the SteamAudioListener has to be added to the tree before calling play_on_audiostreamplayer.");
		return playback;
	}
	if (audiostreamplayer.get_type() != Variant::OBJECT || (!audiostreamplayer.has_method("set_stream") && !audiostreamplayer.has_method("play_stream"))) {
		ERR_PRINT("SteamAudioListener: audiostreamplayer is not an AudioStreamPlayer/2D/3D or SteamAudioSource.");
		return playback;
	}
	if (audiostreamplayer.has_method("set_stream")) {
		audiostreamplayer.call("set_stream", generator);
		audiostreamplayer.call("play");
		playback = audiostreamplayer.call("get_stream_playback");
	}
	else if (audiostreamplayer.has_method("play_stream")) {
		playback = audiostreamplayer.call("play_stream", generator);
	}
	if (playback.is_valid()) {
		SteamAudioServer::get_singleton()->add_playback_to_listener(this, playback);
	}
	else {
		ERR_PRINT("SteamAudioListener: play_on_audiostreamplayer failed.");
	}
	return playback;
}

SteamAudioListener::SteamAudioListener() {}
SteamAudioListener::~SteamAudioListener() {}

int SteamAudioListener::get_num_refl_rays() { return num_refl_rays; }
void SteamAudioListener::set_num_refl_rays(int p_num_refl_rays) { num_refl_rays = p_num_refl_rays; }
int SteamAudioListener::get_num_refl_bounces() { return num_refl_bounces; }
void SteamAudioListener::set_num_refl_bounces(int p_num_refl_bounces) { num_refl_bounces = p_num_refl_bounces; }
int SteamAudioListener::get_refl_ambisonics_order() { return refl_ambisonics_order; }
void SteamAudioListener::set_refl_ambisonics_order(int p_refl_ambisonics_order) { refl_ambisonics_order = p_refl_ambisonics_order; }
float SteamAudioListener::get_refl_duration() { return refl_duration; }
void SteamAudioListener::set_refl_duration(float p_refl_duration) { refl_duration = p_refl_duration; }
float SteamAudioListener::get_irradiance_min_dist() { return irradiance_min_dist; }
void SteamAudioListener::set_irradiance_min_dist(float p_irradiance_min_dist) { irradiance_min_dist = p_irradiance_min_dist; }
int SteamAudioListener::get_refl_type() { return refl_type; }
void SteamAudioListener::set_refl_type(int p_refl_type) { refl_type = p_refl_type; }

PackedStringArray SteamAudioListener::_get_configuration_warnings() const {
	PackedStringArray res;
	return res;
}
