#ifndef AUDIOSTREAMSTEAMAUDIOLISTENER_H
#define AUDIOSTREAMSTEAMAUDIOLISTENER_H

#include "RingBuffer.h"

#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>

class AudioStreamSteamAudioListenerPlayback : public godot::AudioStreamPlayback
{
    GDCLASS( AudioStreamSteamAudioListenerPlayback, godot::AudioStreamPlayback )

protected:
    static void _bind_methods() {}

    bool active = false;
    uint64_t mixed = 0;
	uint32_t num_underrun_samples = 0;
    godot::RingBuffer<godot::AudioFrame> ring_buffer;

	// for the buffer underrun concealment logic
	godot::AudioFrame last_frame = {0, 0};
	bool underrun_active = false;
	int underrun_fade_pos = 0;
	static constexpr int UNDERRUN_FADE_LEN = 64;

public:
    AudioStreamSteamAudioListenerPlayback();
    void set_buffer_size( int p_frames );
    void push_frames( const godot::AudioFrame *p_frames, int p_count );
    bool push_buffer( const godot::PackedVector2Array& p_buffer );
    int get_free_buffer_size() const;
    int get_available_buffer_size() const;
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
