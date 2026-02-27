#include "AudioStreamSteamAudioListener.h"

#include <cstring>

// --- AudioStreamSteamAudioListenerPlayback ---

void AudioStreamSteamAudioListenerPlayback::compute_ar2()
{
	predictor_valid = false;

	if (history_count < 3)
		return;

	const int N = godot::MIN(history_count, HISTORY_SIZE);

	float r0 = 0.0f;
	float r1 = 0.0f;
	float r2 = 0.0f;

	// autocorrelation over mono mix
	for (int i = 2; i < N; i++)
	{
		godot::AudioFrame x0 = get_history(i);
		godot::AudioFrame x1 = get_history(i - 1);
		godot::AudioFrame x2 = get_history(i - 2);

		float s0 = 0.5f * (x0.left + x0.right);
		float s1 = 0.5f * (x1.left + x1.right);
		float s2 = 0.5f * (x2.left + x2.right);

		r0 += s0 * s0;
		r1 += s0 * s1;
		r2 += s0 * s2;
	}

	if (fabs(r0) < 1e-9f)
		return;

	// Yule-Walker solve for AR(2)
	float det = r0 * r0 - r1 * r1;
	if (fabs(det) < 1e-9f)
		return;

	ar_a1 = (r0 * r1 - r1 * r2) / det;
	ar_a2 = (r0 * r2 - r1 * r1) / det;

	predictor_valid = true;
}

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

		for (int i = 0; i < to_mix; i++)
			push_history(p_buffer[i]);

		mixed += to_mix;

		// If we were in underrun, and now recovered,
		// reset predictor state
		if (underrun_active)
		{
			underrun_active = false;
			predictor_valid = false;
		}
	}

	// ---- 2. Handle underrun ----
	if (to_mix < p_frames)
	{
		if (!underrun_active)
		{
			underrun_active = true;
			compute_ar2(); // compute once per underrun burst
		}

		for (int i = to_mix; i < p_frames; i++)
		{
			godot::AudioFrame predicted = {0, 0};

			if (predictor_valid && history_count >= 2)
			{
				godot::AudioFrame x1 = get_history(0);
				godot::AudioFrame x2 = get_history(1);

				predicted.left = ar_a1 * x1.left + ar_a2 * x2.left;
				predicted.right = ar_a1 * x1.right + ar_a2 * x2.right;
			}

			p_buffer[i] = predicted;
			push_history(predicted);
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
