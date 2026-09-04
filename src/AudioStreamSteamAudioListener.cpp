#include "AudioStreamSteamAudioListener.h"
#include "resampler/MultiChannelResampler.h"
#include "server.hpp"

#include <cstring>
#include <godot_cpp/core/class_db.hpp>

// --- AudioStreamSteamAudioListenerPlayback ---

void AudioStreamSteamAudioListenerPlayback::_bind_methods() {
	godot::ClassDB::bind_method(godot::D_METHOD("set_rates", "steam_sampling_rate", "godot_mix_rate"), &AudioStreamSteamAudioListenerPlayback::set_rates);
}

AudioStreamSteamAudioListenerPlayback::AudioStreamSteamAudioListenerPlayback()
{
    ring_buffer.resize( godot::nearest_shift( 1024 ) );
    ring_buffer.clear();
}

AudioStreamSteamAudioListenerPlayback::~AudioStreamSteamAudioListenerPlayback() = default;

void AudioStreamSteamAudioListenerPlayback::set_rates( int p_steam_sampling_rate, int p_godot_mix_rate )
{
	if (steam_sampling_rate == p_steam_sampling_rate && godot_mix_rate == p_godot_mix_rate)
		return;
	steam_sampling_rate = p_steam_sampling_rate;
	godot_mix_rate = p_godot_mix_rate;
	init_resampler();
}

void AudioStreamSteamAudioListenerPlayback::init_resampler()
{
	if (steam_sampling_rate > 0 && godot_mix_rate > 0 && steam_sampling_rate != godot_mix_rate) {
		resampler.reset(oboe::resampler::MultiChannelResampler::make(
			2,
			steam_sampling_rate,
			godot_mix_rate,
			oboe::resampler::MultiChannelResampler::Quality::High
		));
	} else {
		resampler.reset();
	}
}

void AudioStreamSteamAudioListenerPlayback::set_buffer_size( int p_frames )
{
    // nearest_shift() yields the smallest power of two strictly greater than
    // p_frames, so usable (= size - 1) >= p_frames.
    ring_buffer.resize( godot::nearest_shift( godot::MAX( 1, p_frames ) ) );
    ring_buffer.clear();
}

void AudioStreamSteamAudioListenerPlayback::push_frames( const godot::AudioFrame *p_frames, int p_count )
{
    int to_push = godot::MIN(ring_buffer.space_left(), p_count);
    if (to_push > 0)
        ring_buffer.write( p_frames, to_push );
}

bool AudioStreamSteamAudioListenerPlayback::push_buffer( const godot::PackedVector2Array &p_buffer )
{
    for (const godot::Vector2 v : p_buffer)
    {
        if (ring_buffer.space_left() < 1)
            return false;
        ring_buffer.write( {v.x, v.y } );
    }
    return true;
}

int AudioStreamSteamAudioListenerPlayback::get_free_buffer_size() const
{
    return ring_buffer.space_left();
}

int AudioStreamSteamAudioListenerPlayback::get_available_buffer_size() const
{
    return ring_buffer.data_left();
}

void AudioStreamSteamAudioListenerPlayback::_start( double p_from_pos )
{
    reset_requested.store( true, std::memory_order_release );
    active = true;
}

void AudioStreamSteamAudioListenerPlayback::_stop()
{
    active = false;
    reset_requested.store( true, std::memory_order_release );
}

bool AudioStreamSteamAudioListenerPlayback::_is_playing() const
{
    return active;
}

double AudioStreamSteamAudioListenerPlayback::_get_playback_position() const
{
    return mixed;
}

int32_t AudioStreamSteamAudioListenerPlayback::_mix(
	godot::AudioFrame *p_buffer,
	float p_rate_scale,
	int32_t p_frames)
{
	if (resampler == nullptr && steam_sampling_rate == 0) {
		if (SteamAudioServer *srv = SteamAudioServer::get_singleton()) {
			set_rates(srv->get_sampling_rate(), srv->get_godot_mix_rate());
		}
	}

	if (reset_requested.exchange( false, std::memory_order_acq_rel ))
	{
		ring_buffer.clear();
		if (resampler) {
			resampler->reset();
		}
	}

	int to_mix = 0;
	if (resampler == nullptr) {
		int available = ring_buffer.data_left();
		to_mix = godot::MIN(available, p_frames);
		if (to_mix > 0) {
			ring_buffer.read(p_buffer, to_mix);
		}
	} else {
		int i = 0;
		for (; i < p_frames; ++i) {
			bool underrun = false;
			while (resampler->isWriteNeeded()) {
				if (ring_buffer.data_left() > 0) {
					godot::AudioFrame in_frame;
					ring_buffer.read(&in_frame, 1);
					float in_data[2] = { in_frame.left, in_frame.right };
					resampler->writeNextFrame(in_data);
				} else {
					underrun = true;
					break;
				}
			}
			if (underrun) {
				break;
			}
			float out_data[2];
			resampler->readNextFrame(out_data);
			p_buffer[i].left = out_data[0];
			p_buffer[i].right = out_data[1];
		}
		to_mix = i;
	}

	// ---- 1. Consume real audio ----
	if (to_mix > 0)
	{
		// Arm the ramp before reading so it covers this block, not the next one.
		if (underrun_active)
		{
			underrun_active = false;
			underrun_fade_pos = 0;
			resume_fade_pos = 0;
		}

		if (resume_fade_pos >= 0)
		{
			int fade_end = godot::MIN( to_mix, UNDERRUN_FADE_LEN - resume_fade_pos );
			for (int i = 0; i < fade_end; i++)
			{
				float t = (float)(resume_fade_pos + i) / (float)UNDERRUN_FADE_LEN;
				p_buffer[i].left = last_frame.left * (1.0f - t) + p_buffer[i].left * t;
				p_buffer[i].right = last_frame.right * (1.0f - t) + p_buffer[i].right * t;
			}
			resume_fade_pos += fade_end;
			if (resume_fade_pos >= UNDERRUN_FADE_LEN)
				resume_fade_pos = -1;
		}

		last_frame = p_buffer[to_mix - 1];
		mixed += to_mix;
	}

	// ---- 2. Handle underrun: fade last frame to silence ----
	if (to_mix < p_frames)
	{
		if (!underrun_active)
		{
			underrun_active = true;
			underrun_fade_pos = 0;
		}

		for (int i = to_mix; i < p_frames; i++)
		{
			if (underrun_fade_pos < UNDERRUN_FADE_LEN)
			{
				float t = 1.0f - (float)underrun_fade_pos / (float)UNDERRUN_FADE_LEN;
				p_buffer[i].left = last_frame.left * t;
				p_buffer[i].right = last_frame.right * t;
				underrun_fade_pos++;
			}
			else
			{
				p_buffer[i] = {0, 0};
			}
			num_underrun_samples++;
		}
	}

	return p_frames;
}

// --- AudioStreamSteamAudioListener ---

godot::Ref<godot::AudioStreamPlayback> AudioStreamSteamAudioListener::_instantiate_playback() const
{
    godot::Ref<AudioStreamSteamAudioListenerPlayback> playback;
    playback.instantiate();
    return playback;
}
