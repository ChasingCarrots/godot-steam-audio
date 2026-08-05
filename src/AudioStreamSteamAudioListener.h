#ifndef AUDIOSTREAMSTEAMAUDIOLISTENER_H
#define AUDIOSTREAMSTEAMAUDIOLISTENER_H

#include "RingBuffer.h"

#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/core/defs.hpp>

#include <atomic>

// Output ring, filled by SteamAudioServer's mixing thread, drained by Godot's audio
// thread.
//
// DO NOT ADD LATENCY TRIMMING HERE. This ring is the pipeline's flow control: nothing
// else paces the mixing thread, which produces as fast as the ring accepts. Steady
// state is therefore "full", and output latency equals the capacity — set via
// SteamAudioServer::get_listener_ring_capacity_frames(), which is the only lever.
// Dropping frames to catch up just lets the producer refill, so the drops never stop
// and turn into a periodic click.
class AudioStreamSteamAudioListenerPlayback : public godot::AudioStreamPlayback
{
    GDCLASS( AudioStreamSteamAudioListenerPlayback, godot::AudioStreamPlayback )

protected:
    static void _bind_methods() {}

    bool active = false;
    uint64_t mixed = 0;
	uint32_t num_underrun_samples = 0;
    godot::RingBuffer<godot::AudioFrame> ring_buffer;

	// _start/_stop run on any thread; only _mix may move the read position, so it
	// performs the reset.
	std::atomic<bool> reset_requested{ false };

	// for the buffer underrun concealment logic
	godot::AudioFrame last_frame = {0, 0};
	bool underrun_active = false;
	int underrun_fade_pos = 0;
	static constexpr int UNDERRUN_FADE_LEN = 64;

	// Blends from last_frame into the new samples after an underrun. -1 = inactive.
	int resume_fade_pos = -1;

public:
    AudioStreamSteamAudioListenerPlayback();
    // p_frames is the desired USABLE capacity, rounded up to a power of two.
    // Setup only: reallocates, so call before handing the playback to the server.
    void set_buffer_size( int p_frames );
    void push_frames( const godot::AudioFrame *p_frames, int p_count );
    bool push_buffer( const godot::PackedVector2Array& p_buffer );
    int get_free_buffer_size() const;
    int get_available_buffer_size() const;
    int get_capacity() const { return ring_buffer.size() - 1; }
	uint32_t get_num_underrun_samples() const { return num_underrun_samples; }

    void _start( double p_from_pos ) override;
    void _stop() override;
    bool _is_playing() const override;
    double _get_playback_position() const override;
    int32_t _mix( godot::AudioFrame *p_buffer, float p_rate_scale, int32_t p_frames ) override;

};

class AudioStreamSteamAudioListener : public godot::AudioStream
{
    GDCLASS( AudioStreamSteamAudioListener, godot::AudioStream )

protected:
    static void _bind_methods() {}

public:
    godot::Ref<godot::AudioStreamPlayback> _instantiate_playback() const override;
    godot::String _get_stream_name() const override { return "AudioStreamSteamAudioListener"; }
};

#endif //AUDIOSTREAMSTEAMAUDIOLISTENER_H
