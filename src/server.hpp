#ifndef STEAM_AUDIO_SERVER_H
#define STEAM_AUDIO_SERVER_H

#include "godot_cpp/classes/audio_effect_instance.hpp"
#include "godot_cpp/classes/audio_frame.hpp"
#include "godot_cpp/classes/audio_stream_generator_playback.hpp"
#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/object.hpp"
#include "godot_cpp/classes/thread.hpp"
#include "godot_cpp/templates/local_vector.hpp"
#include "godot_cpp/variant/packed_vector2_array.hpp"
#include "material.hpp"
#include <phonon.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace godot {
class AudioStream;
}
class SteamAudioListener;
class SteamAudioSource;

struct ListenerPlaybackEntry {
	godot::Ref<godot::AudioStreamGeneratorPlayback> playback;
	int remaining_from_push_buffer = 0;
};

struct ListenerData {
	SteamAudioListener *listener = nullptr;
	IPLSimulator simulator = nullptr;

	// This buffer will be filled by the pre-mixed and steam audio handled
	// audio data from all relevant sources. When all relevant sources have
	// filled it, it will be pushed to the listener's playbacks, ready for a new round.
	godot::PackedVector2Array push_buffer;
	bool push_buffer_ready = false;

	// Multiple playbacks per listener, protected by playbacks_mutex
	godot::LocalVector<ListenerPlaybackEntry> playbacks;
	std::unique_ptr<std::mutex> playbacks_mutex = std::make_unique<std::mutex>();

	// Cached transform data, updated on main thread
	IPLCoordinateSpace3 cached_coords{};
	godot::Transform3D last_trf;
	bool dirty = false;
};

struct SourceListenerData {
	SteamAudioListener *listener = nullptr;
	float dist_to_listener = 0.0f;
	float doppler_pitch = 1.0f;
	bool out_of_range = false;
	bool consumed_source_mix = true;
	bool pushed_to_listener_buffer = false;
	IPLSource source = nullptr;
	IPLBinauralEffect binaural_effect = nullptr;
	IPLDirectEffect direct_effect = nullptr;
	IPLReflectionEffect reflection_effect = nullptr;
	IPLAmbisonicsDecodeEffect ambisonics_decode_effect = nullptr;
	IPLAudioBuffer input_buffer{};
	IPLAudioBuffer output_buffer{};
	IPLAudioBuffer ambisonics_buffer{};
};

struct SourcePlaybackEntry {
	godot::Ref<godot::AudioStreamPlayback> playback;
	int num_mixed_too_much_last_round = 0;
	float volume_linear = 1.0f;
	float pitch_scale = 1.0f;
};

struct SourceData {
	SteamAudioSource *source_node = nullptr;

	godot::LocalVector<SourcePlaybackEntry> playbacks;

	godot::LocalVector<SourceListenerData> listener_data;

	// Pre-mixed audio frames from source playbacks.
	godot::PackedVector2Array mixed_frames;
	int mixed_frames_ready = 0;

	// AudioEffectInstances created from the source's effect stack
	godot::LocalVector<godot::Ref<godot::AudioEffectInstance>> effect_instances;

	// Cached transform data, updated on main thread
	IPLCoordinateSpace3 cached_coords{};
	godot::Transform3D last_trf;
};

struct DynamicGeometryData {
	godot::Node3D *node = nullptr;
	IPLScene sub_scene = nullptr;
	IPLInstancedMesh instanced_mesh = nullptr;
	std::vector<IPLStaticMesh> meshes;
	godot::Transform3D last_trf;
};

struct StaticGeometryData {
	godot::Node *node = nullptr;
	std::vector<IPLStaticMesh> meshes;
};

// Helper to clean up a SourceListenerData's IPL resources
void cleanup_source_listener_data(SourceListenerData &sld, IPLContext ctx);

class SteamAudioServer : public godot::Object {
	GDCLASS(SteamAudioServer, godot::Object)

private:
	static SteamAudioServer *self;

	bool is_initialized = false;
	IPLContext phonon_context = nullptr;
	IPLScene phonon_scene = nullptr;
	// HRTF is global for now, but could be per listener if needed.
	// SteamAudio uses one HRTF for the context usually.
	IPLHRTF phonon_hrtf = nullptr;

	std::atomic<bool> is_running;

	// Simulation Thread
	godot::Ref<godot::Thread> simulation_thread;
	void simulation_thread_func();

	// Mixing Thread
	godot::Ref<godot::Thread> mixing_thread;
	void mixing_thread_func();

	std::vector<ListenerData> listeners;
	godot::LocalVector<SourceData> sources;
	godot::LocalVector<DynamicGeometryData> dynamic_geometry;
	godot::LocalVector<StaticGeometryData> static_geometry;

	// Protects listeners, sources, dynamic_geometry, static_geometry
	std::shared_mutex collections_mutex;

	std::atomic<bool> refl_thread_wait_for_commit;
	std::atomic<bool> is_refl_thread_processing;
	std::atomic<bool> new_inputs_set;
	std::mutex refl_mux;
	std::condition_variable refl_cv;

	bool scene_dirty = false;

	// Cached audio settings, set once during init()
	IPLAudioSettings cached_audio_settings{};

	// Registration of Project Settings
	void register_settings();

	IPLAudioSettings get_audio_settings();

protected:
	static void _bind_methods();

public:
	SteamAudioServer();
	~SteamAudioServer();

	static SteamAudioServer *get_singleton();

	void init();
	void finish();

	void tick(float delta);

	// Listener management
	void add_listener(SteamAudioListener *listener);
	void add_playback_to_listener(SteamAudioListener *listener, godot::Ref<godot::AudioStreamGeneratorPlayback> playback);
	void remove_listener(SteamAudioListener *listener);

	// Source management (for SteamAudioSource nodes)
	void add_source(SteamAudioSource *source_node);
	void add_playback_to_source(const SteamAudioSource *source_node, godot::Ref<godot::AudioStreamPlayback> p_playback, float p_volume_db, float p_pitch_scale);
	void set_source_playback_volume(const SteamAudioSource * source_node, godot::Ref<godot::AudioStreamPlayback> p_playback, float p_volume_db);
	void set_source_playback_pitch(const SteamAudioSource * source_node, godot::Ref<godot::AudioStreamPlayback> p_playback, float p_pitch_scale);
	int source_get_num_active_playbacks(const SteamAudioSource * source_node);
	void remove_source(SteamAudioSource *source_node);

	void add_static_geometry(godot::Node *p_node, godot::Ref<SteamAudioMaterial> p_material);
	void remove_static_geometry(godot::Node *p_node);
	void add_dynamic_geometry(godot::Node *p_node, godot::Ref<SteamAudioMaterial> p_material);
	void remove_dynamic_geometry(godot::Node *node);

	// Audio pulling and effect application
	void process_audio();

	int get_frame_size() const { return cached_audio_settings.frameSize; }
	int get_sampling_rate() const { return cached_audio_settings.samplingRate; }
};

#endif // STEAM_AUDIO_SERVER_H
