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

int32_t AudioStreamSteamAudioListenerPlayback::_mix( godot::AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames )
{
    int available = ring_buffer.data_left();
    int to_mix = godot::MIN(available, p_frames);

    if ( to_mix > 0 )
        ring_buffer.read(  p_buffer, to_mix );
    mixed += to_mix;

    if (fill_with_zero)
    {
        for (int i = to_mix; i < p_frames; i++)
        {
            p_buffer[i] = {0,0};
        }

        return p_frames;
    }
    return to_mix;
}

// --- AudioStreamSteamAudioListener ---

godot::Ref<godot::AudioStreamPlayback> AudioStreamSteamAudioListener::_instantiate_playback() const
{
    godot::Ref<AudioStreamSteamAudioListenerPlayback> playback;
    playback.instantiate();
    return playback;
}
