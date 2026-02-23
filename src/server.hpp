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

class SteamAudioListener;
class SteamAudioSource;

struct ListenerData {
	SteamAudioListener *listener = nullptr;
	IPLSimulator simulator = nullptr;

	// Pre-allocated buffers to avoid reallocations in the mixing thread
	godot::LocalVector<godot::AudioFrame> mix_buffer;
	godot::PackedVector2Array push_buffer;

	// Multiple playbacks per listener, protected by playbacks_mutex
	godot::LocalVector<godot::Ref<godot::AudioStreamGeneratorPlayback>> playbacks;
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
	IPLSource source = nullptr;
	IPLBinauralEffect binaural_effect = nullptr;
	IPLDirectEffect direct_effect = nullptr;
	IPLReflectionEffect reflection_effect = nullptr;
	IPLAmbisonicsDecodeEffect ambisonics_decode_effect = nullptr;
	IPLAudioBuffer input_buffer{};
	IPLAudioBuffer output_buffer{};
	IPLAudioBuffer ambisonics_buffer{};
};

struct SourceData {
	SteamAudioSource *source_node = nullptr;

	godot::LocalVector<SourceListenerData> listener_data;

	// Pre-mixed audio frames from source playbacks.
	godot::PackedVector2Array mixed_frames;
	int mixed_frames_ready = 0;
	bool mixed_frames_consumed = false;

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
	void add_listener(SteamAudioListener *listener, godot::Ref<godot::AudioStreamGeneratorPlayback> playback);
	void remove_listener(SteamAudioListener *listener);

	// Source management (for SteamAudioSource nodes)
	void add_source(SteamAudioSource *source_node);
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
