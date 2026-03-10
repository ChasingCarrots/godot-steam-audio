#ifndef STEAM_AUDIO_SOURCE_HPP
#define STEAM_AUDIO_SOURCE_HPP

#include <phonon.h>
#include <cmath>
#include <godot_cpp/classes/audio_effect.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/local_vector.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <shared_mutex>

class SteamAudioSource : public godot::Node3D {
	GDCLASS(SteamAudioSource, godot::Node3D);

private:
	bool dynamic_registration = false;
	bool is_registered = false;
	uint64_t timeout_registration_at;

	bool direct_enabled = true;

	bool binaural_enabled = true;
	int binaural_interpolation = IPL_HRTFINTERPOLATION_NEAREST;
	float binaural_spatial_blend = 1.0f;

	bool distance_attenuation_enabled = true;
	float distance_attenuation_min = 1.0f;
	float distance_attenuation_max = 120.0f;
	uint32_t layers = 1;

	bool air_absorption_enabled = true;

	bool occlusion_enabled = false;
	int occlusion_type = IPL_OCCLUSIONTYPE_RAYCAST;
	float occlusion_radius = 1.0f;
	int occlusion_samples = 16;

	bool transmission_enabled = false;
	int transmission_type = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
	int transmission_rays = 16;

	bool reflection_enabled = false;
	float reflection_duration = 2.0f;
	float reflection_hybrid_delay = 0.5f;

	float doppler_factor = 1.0f;

	godot::TypedArray<godot::AudioEffect> effect_stack;

protected:
	static void _bind_methods();

public:
	SteamAudioSource();
	~SteamAudioSource();

	void _notification(int p_what);

	godot::Ref<godot::AudioStreamPlayback> play_stream(godot::Ref<godot::AudioStream> p_stream, float p_volume_db = 0.0f, float p_pitch_scale = 1.0f);
	void set_stream_volume(godot::Ref<godot::AudioStreamPlayback> p_playback, float p_volume_db);
	void set_stream_pitch(godot::Ref<godot::AudioStreamPlayback> p_playback, float p_pitch_scale);

	bool get_direct_enabled() const { return direct_enabled; }
	void set_direct_enabled(bool p_enabled) { direct_enabled = p_enabled; }

	bool get_binaural_enabled() const { return binaural_enabled; }
	void set_binaural_enabled(bool p_enabled) { binaural_enabled = p_enabled; }
	int get_binaural_interpolation() const { return binaural_interpolation; }
	void set_binaural_interpolation(int p_interpolation) { binaural_interpolation = p_interpolation; }

	bool get_reflection_enabled() const { return reflection_enabled; }
	void set_reflection_enabled(bool p_enabled) { reflection_enabled = p_enabled; }
	float get_reflection_duration() const { return reflection_duration; }
	void set_reflection_duration(float p_duration) { reflection_duration = p_duration; }
	float get_reflection_hybrid_delay() const { return reflection_hybrid_delay; }
	void set_reflection_hybrid_delay(float p_delay) { reflection_hybrid_delay = p_delay; }

	bool get_occlusion_enabled() const { return occlusion_enabled; }
	void set_occlusion_enabled(bool p_enabled) { occlusion_enabled = p_enabled; }
	int get_occlusion_type() const { return occlusion_type; }
	void set_occlusion_type(int p_type) { occlusion_type = p_type; }
	float get_occlusion_radius() const { return occlusion_radius; }
	void set_occlusion_radius(float p_radius) { occlusion_radius = p_radius; }
	int get_occlusion_samples() const { return occlusion_samples; }
	void set_occlusion_samples(int p_samples) { occlusion_samples = p_samples; }

	bool get_transmission_enabled() const { return transmission_enabled; }
	void set_transmission_enabled(bool p_enabled) { transmission_enabled = p_enabled; }
	int get_transmission_type() const { return transmission_type; }
	void set_transmission_type(int p_type) { transmission_type = p_type; }
	int get_transmission_rays() const { return transmission_rays; }
	void set_transmission_rays(int p_rays) { transmission_rays = p_rays; }

	bool get_air_absorption_enabled() const { return air_absorption_enabled; }
	void set_air_absorption_enabled(bool p_enabled) { air_absorption_enabled = p_enabled; }

	bool get_distance_attenuation_enabled() const { return distance_attenuation_enabled; }
	void set_distance_attenuation_enabled(bool p_enabled) { distance_attenuation_enabled = p_enabled; }
	float get_distance_attenuation_min() const { return distance_attenuation_min; }
	void set_distance_attenuation_min(float p_min) { distance_attenuation_min = p_min; }
	float get_distance_attenuation_max() const { return distance_attenuation_max; }
	void set_distance_attenuation_max(float p_max) { distance_attenuation_max = p_max; }

	uint32_t get_layers() const { return layers; }
	void set_layers(uint32_t p_layers) { layers = p_layers; }

	bool get_dynamic_registration() const { return dynamic_registration; }
	void set_dynamic_registration(bool p_enabled) { dynamic_registration = p_enabled; }

	float get_doppler_factor() const { return doppler_factor; }
	void set_doppler_factor(float p_factor) { doppler_factor = p_factor; }

	float get_binaural_spatial_blend() const { return binaural_spatial_blend; }
	void set_binaural_spatial_blend(float p_blend) { binaural_spatial_blend = p_blend; }

	godot::TypedArray<godot::AudioEffect> get_effect_stack() const { return effect_stack; }
	void set_effect_stack(const godot::TypedArray<godot::AudioEffect> &p_stack) { effect_stack = p_stack; }
};

#endif // STEAM_AUDIO_SOURCE_HPP
