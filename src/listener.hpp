#ifndef STEAM_AUDIO_LISTENER_HPP
#define STEAM_AUDIO_LISTENER_HPP

#include "godot_cpp/classes/wrapped.hpp"
#include "godot_cpp/variant/packed_string_array.hpp"
#include <phonon.h>
#include <godot_cpp/classes/audio_stream_generator.hpp>
#include <godot_cpp/classes/audio_stream_generator_playback.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <vector>

#include "AudioStreamSteamAudioListener.h"
#include "lib/steamaudio/core/src/core/array.h"
#include "lib/steamaudio/core/src/core/vector.h"

class SteamAudioSource;

class SteamAudioListenerSensorSlot : public godot::RefCounted {
	GDCLASS(SteamAudioListenerSensorSlot, godot::RefCounted);
protected:
	static void _bind_methods();

	SteamAudioSource *source_node;
	godot::Vector3 position;
	float db_level;
public:
	void set_steam_audio_source(SteamAudioSource *p_source_node);
	SteamAudioSource *get_steam_audio_source();
	void set_position(const godot::Vector3 &p_position);
	godot::Vector3 get_position();
	void set_db_level(float db_level);
	float get_db_level();
};

class SteamAudioListener : public godot::Node3D {
	GDCLASS(SteamAudioListener, godot::Node3D);
protected:
	static void _bind_methods();

private:
	bool reflection_simulation_enabled = true;
	int num_refl_rays = 4096;
	int num_refl_bounces = 16;
	float refl_duration = 2.0f;
	int refl_ambisonics_order = 1;
	int refl_type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
	float irradiance_min_dist = 1.0f;
	uint32_t mask = 1;
	float range = 0.0f;
	godot::Ref<AudioStreamSteamAudioListener> generator;

	void ready_internal();
public:
	SteamAudioListener();
	~SteamAudioListener();

	void _notification(int p_what);

	int get_num_refl_rays();
	void set_num_refl_rays(int p_num_refl_rays);
	int get_num_refl_bounces();
	void set_num_refl_bounces(int p_num_refl_bounces);
	int get_refl_ambisonics_order();
	void set_refl_ambisonics_order(int p_refl_ambisonics_order);
	float get_refl_duration();
	void set_refl_duration(float p_refl_duration);
	float get_irradiance_min_dist();
	void set_irradiance_min_dist(float p_irradiance_min_dist);
	int get_refl_type();
	void set_refl_type(int p_refl_type);

	uint32_t get_mask() const { return mask; }
	void set_mask(uint32_t p_mask) { mask = p_mask; }

	float get_range() const { return range; }
	void set_range(float p_range) { range = p_range; }

	bool get_reflection_simulation_enabled() { return reflection_simulation_enabled; }
	void set_reflection_simulation_enabled(bool p_enabled) { reflection_simulation_enabled = p_enabled; }

	void set_num_source_db_sensor_slots(int p_num_source_db_sensor_slots);
	int get_num_source_db_sensor_slots();
	godot::Ref<SteamAudioListenerSensorSlot> get_sensor_slot(int p_sensor_slot);
	godot::Array get_sensor_slots() const;

	godot::Ref<AudioStreamSteamAudioListenerPlayback> play_on_audiostreamplayer(godot::Variant audiostreamplayer);

	godot::PackedStringArray _get_configuration_warnings() const override;

private:
	std::vector<godot::Ref<SteamAudioListenerSensorSlot>> sensor_slots;
};

#endif // STEAM_AUDIO_LISTENER_HPP
