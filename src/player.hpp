
#ifndef STEAM_AUDIO_PLAYER_H
#define STEAM_AUDIO_PLAYER_H

#include "godot_cpp/classes/audio_stream.hpp"
#include "godot_cpp/classes/wrapped.hpp"
#include "steam_audio.hpp"
#include <phonon.h>
#include <godot_cpp/classes/audio_stream_player3d.hpp>

class SteamAudioStream;
using namespace godot;

class SteamAudioPlayer : public AudioStreamPlayer3D {
	GDCLASS(SteamAudioPlayer, AudioStreamPlayer3D);

private:
	// This ref is kept because at destruction we can't get the playback
	// since the player has stopped (even though the playback still mixes...)
	Ref<SteamAudioStream> pb;
	LocalSteamAudioState local_state;
	std::atomic<bool> is_source_in_simulation = false;
	std::atomic<bool> is_local_state_init;
	std::atomic<bool> can_load_local_state;

	void init_local_state();

protected:
	static void _bind_methods();

public:
	SteamAudioPlayer();
	~SteamAudioPlayer();
	void ready_internal();
	void process_internal(double delta);
	void _notification(int p_what);

	LocalSteamAudioState *get_local_state();

	float get_occlusion_radius();
	void set_occlusion_radius(float p_occlusion_radius);
	int get_occlusion_samples();
	void set_occlusion_samples(int p_occlusion_samples);
	int get_transmission_rays();
	void set_transmission_rays(int p_transmission_rays);
	float get_min_attenuation_dist();
	void set_min_attenuation_dist(float p_min_attenuation_dist);
	float get_max_attenuation_dist();
	void set_max_attenuation_dist(float p_max_attenuation_dist);
	int get_ambisonics_order();
	void set_ambisonics_order(int p_ambisonics_order);
	float get_max_reflection_dist();
	void set_max_reflection_dist(float p_max_reflection_dist);
	float get_hybrid_reverb_transition_time();
	void set_hybrid_reverb_transition_time(float p_hybrid_reverb_transition_time);
	float get_hybrid_reverb_overlap_percent();
	void set_hybrid_reverb_overlap_percent(float p_hybrid_reverb_overlap_percent);
	float get_directivity_dipole_weight();
	void set_directivity_dipole_weight(float p_directivity_dipole_weight);
	float get_directivity_dipole_power();
	void set_directivity_dipole_power(float p_directivity_dipole_power);
	int get_transmission_type();
	void set_transmission_type(int p_transmission_type);

	bool is_dist_attn_on();
	void set_dist_attn_on(bool p_dist_attn_on);
	bool is_reflection_on();
	void set_reflection_on(bool p_reflection_on);
	bool is_occlusion_on();
	void set_occlusion_on(bool p_occlusion_on);
	bool is_air_absorption_on();
	void set_air_absorption_on(bool p_air_absorption_on);
	bool is_directivity_on();
	void set_directivity_on(bool p_directivity_on);
	bool is_transmission_on();
	void set_transmission_on(bool p_transmission_on);
	bool is_binaural_on();
	void set_binaural_on(bool p_binaural_on);
	bool is_binaural_interpolation_on();
	void set_binaural_interpolation_on(bool p_binaural_on);
	bool SteamAudioPlayer::is_skip_direct_audio_on();
	void SteamAudioPlayer::set_skip_direct_audio_on(bool p_direct_on);

	void play_stream(const Ref<AudioStream> &p_stream, float p_from_offset, float p_volume_db, float p_pitch_scale);
	Ref<AudioStream> get_inner_stream();
	Ref<AudioStreamPlayback> get_inner_stream_playback();

	PackedStringArray _get_configuration_warnings() const override;
};

#endif // STEAM_AUDIO_PLAYER_H
