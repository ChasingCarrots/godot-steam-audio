#include "AudioStreamSteamAudioListener.h"

#include <cstring>

// --- AudioStreamSteamAudioListenerPlayback ---

AudioStreamSteamAudioListenerPlayback::AudioStreamSteamAudioListenerPlayback()
{
    ring_buffer.resize( godot::nearest_shift( 1024 ) );
    ring_buffer.clear();
}

void AudioStreamSteamAudioListenerPlayback::set_buffer_size( int p_frames )
{
    ring_buffer.resize( godot::nearest_shift( p_frames ) );
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
            break;
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
    active = true;
}

void AudioStreamSteamAudioListenerPlayback::_stop()
{
    active = false;
    ring_buffer.clear();
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
	int available = ring_buffer.data_left();
	int to_mix = godot::MIN(available, p_frames);

	// ---- 1. Consume real audio ----
	if (to_mix > 0)
	{
		ring_buffer.read(p_buffer, to_mix);

		last_frame = p_buffer[to_mix - 1];
		mixed += to_mix;

		if (underrun_active)
		{
			underrun_active = false;
			underrun_fade_pos = 0;
		}
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
