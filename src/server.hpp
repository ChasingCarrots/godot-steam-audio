#ifndef STEAM_AUDIO_SERVER_H
#define STEAM_AUDIO_SERVER_H

#include "godot_cpp/classes/audio_effect_instance.hpp"
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
#include <variant>
#include <vector>

#include "AudioStreamSteamAudioListener.h"
#include "godot_cpp/classes/audio_stream_playback.hpp"

namespace godot {
class AudioStream;
}
class SteamAudioListener;
class SteamAudioSource;

struct ListenerPlaybackEntry {
	godot::Ref<AudioStreamSteamAudioListenerPlayback> playback;
	int remaining_from_push_buffer = 0;

	uint32_t debug_times_drained = 0;
};

struct ListenerSourceDBLevel {
	SteamAudioSource* source;
	godot::Vector3 position;
	float db_level = 0.0f;
};

struct ListenerData {
	SteamAudioListener *listener = nullptr;
	IPLSimulator simulator = nullptr;

	// This buffer will be filled by the pre-mixed and steam audio handled
	// audio data from all relevant sources (additive). When all relevant sources have
	// added to it, it will be pushed to the listener's playbacks, ready for a new round.
	godot::PackedVector2Array push_buffer;

	// Counter-based flow control:
	// pending_contributors: number of relevant (to this listener) sources that still need to contribute
	// to the push_buffer. Set when push_buffer is cleared, decremented as sources contribute.
	// When it reaches 0, the push_buffer is ready to be pushed to playbacks.
	int pending_contributors = 0;
	// pending_drains: number of playbacks that still need to fully consume the push_buffer.
	// Set when push_buffer becomes ready, decremented as playbacks finish draining.
	// When it reaches 0, the push_buffer can be cleared and reused.
	int pending_drains = 0;
	// Generation counter: incremented each time the push_buffer is cleared and
	// pending_contributors is recomputed. Used with SourceListenerData::last_contributed_generation
	// to track which sources have already contributed without needing reset loops.
	uint32_t generation = 1;

	// Multiple playbacks per listener, protected by playbacks_mutex
	godot::LocalVector<ListenerPlaybackEntry> playbacks;
	std::unique_ptr<std::mutex> playbacks_mutex = std::make_unique<std::mutex>();

	godot::LocalVector<ListenerSourceDBLevel> source_db_levels;

	// Cached transform data, updated on main thread
	IPLCoordinateSpace3 cached_coords{};
	godot::Transform3D last_trf;
	bool dirty = false;

	uint32_t debug_times_pushed = 0;
};

struct SourceListenerData {
	SteamAudioListener *listener = nullptr;
	float dist_to_listener = 0.0f;
	float doppler_pitch = 1.0f;
	bool out_of_range = false;
	// Generation of the listener's push_buffer that this SLD last contributed to.
	// Compared against ListenerData::generation to determine if contribution is needed.
	uint32_t last_contributed_generation = 0;
	IPLSource source = nullptr;
	IPLBinauralEffect binaural_effect = nullptr;
	IPLDirectEffect direct_effect = nullptr;
	IPLReflectionEffect reflection_effect = nullptr;
	IPLAmbisonicsDecodeEffect ambisonics_decode_effect = nullptr;
	IPLAudioBuffer input_buffer{};
	IPLAudioBuffer output_buffer{};
	IPLAudioBuffer ambisonics_buffer{};

	uint32_t debug_times_contributed = 0;
};

struct SourcePlaybackEntry {
	godot::Ref<godot::AudioStreamPlayback> playback;
	int num_mixed_in_current_mixed_frames = 0;
	float volume_linear = 1.0f;
	float pitch_scale = 1.0f;

	uint32_t debug_num_mixed = 0;
};

struct SourceData {
	SteamAudioSource *source_node = nullptr;

	godot::LocalVector<SourcePlaybackEntry> playbacks;

	godot::LocalVector<SourceListenerData> listener_data;

	// Pre-mixed audio frames from source playbacks.
	godot::PackedVector2Array mixed_frames;
	int mixed_frames_ready = 0;
	// Number of listeners that still need to consume the current mix.
	// Set when mixed_frames_ready == frame_size, decremented as listeners consume.
	// When it reaches 0, the source can reset and start a new mix.
	int pending_consumers = 0;

	float current_db_level = 0;

	// AudioEffectInstances created from the source's effect stack
	godot::LocalVector<godot::Ref<godot::AudioEffectInstance>> effect_instances;

	// Cached transform data, updated on main thread
	IPLCoordinateSpace3 cached_coords{};
	godot::Transform3D last_trf;

	uint32_t debug_times_mixed = 0;
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

// Pending operation types for the commit queue
struct PendingAddSource {
	SteamAudioSource *source_node;
	SourceData source_data;
};

struct PendingRemoveSource {
	SteamAudioSource *source_node;
};

struct PendingAddListener {
	SteamAudioListener *listener;
	ListenerData listener_data;
};

struct PendingRemoveListener {
	SteamAudioListener *listener;
};

struct PendingAddPlaybackToSource {
	const SteamAudioSource *source_node;
	godot::Ref<godot::AudioStreamPlayback> playback;
	float volume_linear;
	float pitch_scale;
};

struct PendingAddPlaybackToListener {
	SteamAudioListener *listener;
	godot::Ref<AudioStreamSteamAudioListenerPlayback> playback;
};

using PendingOp = std::variant<
	PendingAddSource,
	PendingRemoveSource,
	PendingAddListener,
	PendingRemoveListener,
	PendingAddPlaybackToSource,
	PendingAddPlaybackToListener
>;

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
	IPLEmbreeDevice embree_dev = nullptr;

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

	// Protects listeners and sources collections.
	// Unique-locked briefly by the mixing thread to apply pending ops.
	// Shared-locked by tick(), process_audio() (after applying ops),
	// simulation_thread, and debug functions.
	std::shared_mutex collections_mutex;

	// Pending operations queue: callers enqueue, mixing thread drains.
	std::mutex pending_ops_mutex;
	std::vector<PendingOp> pending_ops;
	void apply_pending_ops();

	std::atomic<bool> refl_thread_wait_for_commit;
	std::atomic<bool> is_refl_thread_processing;
	std::atomic<bool> new_inputs_set;

	std::atomic<float> mixing_thread_usage_pct{0.0f};
	std::atomic<float> sim_thread_avg_duration_ms{0.0f};
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
	void add_playback_to_listener(SteamAudioListener *listener, godot::Ref<AudioStreamSteamAudioListenerPlayback> playback);
	void remove_listener(SteamAudioListener *listener);
	const godot::LocalVector<ListenerSourceDBLevel>& get_source_db_levels_for_listener(SteamAudioListener *listener);

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

	// Debug functions
	float get_mixing_thread_usage_pct() const;
	float get_sim_thread_avg_duration_ms() const;
	int get_source_count();
	int get_listener_count();
	godot::String get_source_name(int index);
	godot::String get_listener_name(int index);
	godot::String get_source_debug_string(int index);
	godot::String get_listener_debug_string(int index);
};

#endif // STEAM_AUDIO_SERVER_H
