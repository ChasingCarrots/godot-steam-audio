#include "source.hpp"
#include "server.hpp"
#include <godot_cpp/classes/engine.hpp>

using namespace godot;

void SteamAudioSource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("play_stream", "stream", "volume_db", "pitch_scale"), &SteamAudioSource::play_stream, DEFVAL(0.0f), DEFVAL(1.0f));
	ClassDB::bind_method(D_METHOD("set_stream_volume", "stream_playback", "volume_db"), &SteamAudioSource::set_stream_volume);
	ClassDB::bind_method(D_METHOD("set_stream_pitch", "stream_playback", "pitch_scale"), &SteamAudioSource::set_stream_pitch);

	ClassDB::bind_method(D_METHOD("get_direct_enabled"), &SteamAudioSource::get_direct_enabled);
	ClassDB::bind_method(D_METHOD("set_direct_enabled", "enabled"), &SteamAudioSource::set_direct_enabled);

	ClassDB::bind_method(D_METHOD("get_binaural_enabled"), &SteamAudioSource::get_binaural_enabled);
	ClassDB::bind_method(D_METHOD("set_binaural_enabled", "enabled"), &SteamAudioSource::set_binaural_enabled);
	ClassDB::bind_method(D_METHOD("get_binaural_interpolation"), &SteamAudioSource::get_binaural_interpolation);
	ClassDB::bind_method(D_METHOD("set_binaural_interpolation", "interpolation"), &SteamAudioSource::set_binaural_interpolation);

	ClassDB::bind_method(D_METHOD("get_reflection_enabled"), &SteamAudioSource::get_reflection_enabled);
	ClassDB::bind_method(D_METHOD("set_reflection_enabled", "enabled"), &SteamAudioSource::set_reflection_enabled);
	ClassDB::bind_method(D_METHOD("get_reflection_duration"), &SteamAudioSource::get_reflection_duration);
	ClassDB::bind_method(D_METHOD("set_reflection_duration", "duration"), &SteamAudioSource::set_reflection_duration);
	ClassDB::bind_method(D_METHOD("get_reflection_hybrid_delay"), &SteamAudioSource::get_reflection_hybrid_delay);
	ClassDB::bind_method(D_METHOD("set_reflection_hybrid_delay", "delay"), &SteamAudioSource::set_reflection_hybrid_delay);

	ClassDB::bind_method(D_METHOD("get_occlusion_enabled"), &SteamAudioSource::get_occlusion_enabled);
	ClassDB::bind_method(D_METHOD("set_occlusion_enabled", "enabled"), &SteamAudioSource::set_occlusion_enabled);
	ClassDB::bind_method(D_METHOD("get_occlusion_type"), &SteamAudioSource::get_occlusion_type);
	ClassDB::bind_method(D_METHOD("set_occlusion_type", "type"), &SteamAudioSource::set_occlusion_type);
	ClassDB::bind_method(D_METHOD("get_occlusion_radius"), &SteamAudioSource::get_occlusion_radius);
	ClassDB::bind_method(D_METHOD("set_occlusion_radius", "radius"), &SteamAudioSource::set_occlusion_radius);
	ClassDB::bind_method(D_METHOD("get_occlusion_samples"), &SteamAudioSource::get_occlusion_samples);
	ClassDB::bind_method(D_METHOD("set_occlusion_samples", "samples"), &SteamAudioSource::set_occlusion_samples);

	ClassDB::bind_method(D_METHOD("get_transmission_enabled"), &SteamAudioSource::get_transmission_enabled);
	ClassDB::bind_method(D_METHOD("set_transmission_enabled", "enabled"), &SteamAudioSource::set_transmission_enabled);
	ClassDB::bind_method(D_METHOD("get_transmission_type"), &SteamAudioSource::get_transmission_type);
	ClassDB::bind_method(D_METHOD("set_transmission_type", "type"), &SteamAudioSource::set_transmission_type);
	ClassDB::bind_method(D_METHOD("get_transmission_rays"), &SteamAudioSource::get_transmission_rays);
	ClassDB::bind_method(D_METHOD("set_transmission_rays", "rays"), &SteamAudioSource::set_transmission_rays);
	ClassDB::bind_method(D_METHOD("get_transmission_low"), &SteamAudioSource::get_transmission_low);
	ClassDB::bind_method(D_METHOD("set_transmission_low", "val"), &SteamAudioSource::set_transmission_low);
	ClassDB::bind_method(D_METHOD("get_transmission_med"), &SteamAudioSource::get_transmission_med);
	ClassDB::bind_method(D_METHOD("set_transmission_med", "val"), &SteamAudioSource::set_transmission_med);
	ClassDB::bind_method(D_METHOD("get_transmission_high"), &SteamAudioSource::get_transmission_high);
	ClassDB::bind_method(D_METHOD("set_transmission_high", "val"), &SteamAudioSource::set_transmission_high);

	ClassDB::bind_method(D_METHOD("get_air_absorption_enabled"), &SteamAudioSource::get_air_absorption_enabled);
	ClassDB::bind_method(D_METHOD("set_air_absorption_enabled", "enabled"), &SteamAudioSource::set_air_absorption_enabled);
	ClassDB::bind_method(D_METHOD("get_air_absorption_low"), &SteamAudioSource::get_air_absorption_low);
	ClassDB::bind_method(D_METHOD("set_air_absorption_low", "val"), &SteamAudioSource::set_air_absorption_low);
	ClassDB::bind_method(D_METHOD("get_air_absorption_med"), &SteamAudioSource::get_air_absorption_med);
	ClassDB::bind_method(D_METHOD("set_air_absorption_med", "val"), &SteamAudioSource::set_air_absorption_med);
	ClassDB::bind_method(D_METHOD("get_air_absorption_high"), &SteamAudioSource::get_air_absorption_high);
	ClassDB::bind_method(D_METHOD("set_air_absorption_high", "val"), &SteamAudioSource::set_air_absorption_high);

	ClassDB::bind_method(D_METHOD("get_distance_attenuation_enabled"), &SteamAudioSource::get_distance_attenuation_enabled);
	ClassDB::bind_method(D_METHOD("set_distance_attenuation_enabled", "enabled"), &SteamAudioSource::set_distance_attenuation_enabled);
	ClassDB::bind_method(D_METHOD("get_distance_attenuation_min"), &SteamAudioSource::get_distance_attenuation_min);
	ClassDB::bind_method(D_METHOD("set_distance_attenuation_min", "min"), &SteamAudioSource::set_distance_attenuation_min);
	ClassDB::bind_method(D_METHOD("get_distance_attenuation_max"), &SteamAudioSource::get_distance_attenuation_max);
	ClassDB::bind_method(D_METHOD("set_distance_attenuation_max", "max"), &SteamAudioSource::set_distance_attenuation_max);

	ClassDB::bind_method(D_METHOD("get_layers"), &SteamAudioSource::get_layers);
	ClassDB::bind_method(D_METHOD("set_layers", "layers"), &SteamAudioSource::set_layers);

	ClassDB::bind_method(D_METHOD("get_dynamic_registration"), &SteamAudioSource::get_dynamic_registration);
	ClassDB::bind_method(D_METHOD("set_dynamic_registration", "enabled"), &SteamAudioSource::set_dynamic_registration);

	ClassDB::bind_method(D_METHOD("get_doppler_factor"), &SteamAudioSource::get_doppler_factor);
	ClassDB::bind_method(D_METHOD("set_doppler_factor", "factor"), &SteamAudioSource::set_doppler_factor);

	ClassDB::bind_method(D_METHOD("get_binaural_spatial_blend"), &SteamAudioSource::get_binaural_spatial_blend);
	ClassDB::bind_method(D_METHOD("set_binaural_spatial_blend", "blend"), &SteamAudioSource::set_binaural_spatial_blend);

	ClassDB::bind_method(D_METHOD("get_effect_stack"), &SteamAudioSource::get_effect_stack);
	ClassDB::bind_method(D_METHOD("set_effect_stack", "stack"), &SteamAudioSource::set_effect_stack);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "direct_enabled"), "set_direct_enabled", "get_direct_enabled");

	ADD_GROUP("Binaural", "binaural_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "binaural_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_binaural_enabled", "get_binaural_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "binaural_interpolation", PROPERTY_HINT_ENUM, "Nearest,Bilinear"), "set_binaural_interpolation", "get_binaural_interpolation");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "binaural_spatial_blend", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_binaural_spatial_blend", "get_binaural_spatial_blend");

	ADD_GROUP("Distance Attenuation", "distance_attenuation_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "distance_attenuation_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_distance_attenuation_enabled", "get_distance_attenuation_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "distance_attenuation_min", PROPERTY_HINT_RANGE, "0.0,100.0,0.1"), "set_distance_attenuation_min", "get_distance_attenuation_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "distance_attenuation_max", PROPERTY_HINT_RANGE, "0.0,100.0,0.1"), "set_distance_attenuation_max", "get_distance_attenuation_max");

	ADD_PROPERTY(PropertyInfo(Variant::INT, "layers", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_layers", "get_layers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dynamic_registration"), "set_dynamic_registration", "get_dynamic_registration");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "doppler_factor", PROPERTY_HINT_RANGE, "0.0,10.0,0.01"), "set_doppler_factor", "get_doppler_factor");

	ADD_GROUP("Air Absorption", "air_absorption_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "air_absorption_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_air_absorption_enabled", "get_air_absorption_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "air_absorption_low", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_air_absorption_low", "get_air_absorption_low");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "air_absorption_med", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_air_absorption_med", "get_air_absorption_med");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "air_absorption_high", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_air_absorption_high", "get_air_absorption_high");

	ADD_GROUP("Occlusion", "occlusion_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "occlusion_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_occlusion_enabled", "get_occlusion_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "occlusion_type", PROPERTY_HINT_ENUM, "Raycast,Volumetric"), "set_occlusion_type", "get_occlusion_type");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "occlusion_radius"), "set_occlusion_radius", "get_occlusion_radius");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "occlusion_samples"), "set_occlusion_samples", "get_occlusion_samples");

	ADD_GROUP("Transmission", "transmission_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "transmission_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_transmission_enabled", "get_transmission_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "transmission_type", PROPERTY_HINT_ENUM, "Frequency Independent,Frequency Dependent"), "set_transmission_type", "get_transmission_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "transmission_rays"), "set_transmission_rays", "get_transmission_rays");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_low", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_transmission_low", "get_transmission_low");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_med", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_transmission_med", "get_transmission_med");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_high", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_transmission_high", "get_transmission_high");

	ADD_GROUP("Reflection", "reflection_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reflection_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_reflection_enabled", "get_reflection_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reflection_duration"), "set_reflection_duration", "get_reflection_duration");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reflection_hybrid_delay", PROPERTY_HINT_RANGE, "0.0,10.0,0.01"), "set_reflection_hybrid_delay", "get_reflection_hybrid_delay");

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "effect_stack", PROPERTY_HINT_TYPE_STRING, String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":AudioEffect"), "set_effect_stack", "get_effect_stack");

	ADD_SIGNAL(MethodInfo("removed_from_simulation"));
}

SteamAudioSource::SteamAudioSource() {}

SteamAudioSource::~SteamAudioSource() {}

void SteamAudioSource::_notification(int p_what) {
	if (Engine::get_singleton()->is_editor_hint())
		return;

	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
			if (Engine::get_singleton()->is_editor_hint())
				return;
			if (!dynamic_registration) {
				SteamAudioServer::get_singleton()->add_source(this);
				is_registered = true;
			}
			set_process(dynamic_registration);
			break;
		case NOTIFICATION_EXIT_TREE:
			if (Engine::get_singleton()->is_editor_hint())
				return;
			if (is_registered) {
				SteamAudioServer::get_singleton()->remove_source(this);
				is_registered = false;
			}
			break;
		case NOTIFICATION_PROCESS: {
			if (!dynamic_registration || !is_registered)
				break;
			int num_active_playbacks = SteamAudioServer::get_singleton()->source_get_num_active_playbacks(this);
			if (num_active_playbacks == 0) {
				SteamAudioServer::get_singleton()->remove_source(this);
				is_registered = false;
				emit_signal("removed_from_simulation");
			}
			break;
		}
	}
}

void SteamAudioSource::set_stream_volume(Ref<AudioStreamPlayback> p_playback, float p_volume_db) {
	if (p_playback.is_null())
		return;

	SteamAudioServer::get_singleton()->set_source_playback_volume(this, p_playback, p_volume_db);
}

void SteamAudioSource::set_stream_pitch(Ref<AudioStreamPlayback> p_playback, float p_pitch_scale) {
	if (p_playback.is_null())
		return;

	SteamAudioServer::get_singleton()->set_source_playback_pitch(this, p_playback, p_pitch_scale);
}

Ref<AudioStreamPlayback> SteamAudioSource::play_stream(Ref<AudioStream> p_stream, float p_volume_db, float p_pitch_scale) {
	if (p_stream.is_null())
		return Ref<AudioStreamPlayback>();

	Ref<AudioStreamPlayback> playback = p_stream->instantiate_playback();
	if (playback.is_valid()) {
		if (dynamic_registration && !is_registered) {
			SteamAudioServer::get_singleton()->add_source(this);
			is_registered = true;
		}
		SteamAudioServer::get_singleton()->add_playback_to_source(this, playback, p_volume_db, p_pitch_scale);
	}
	return playback;
}
