#ifndef STEAM_AUDIO_SERVER_H
#define STEAM_AUDIO_SERVER_H

#include "godot_cpp/classes/audio_buffer.hpp"
#include "godot_cpp/classes/audio_effect_instance.hpp"
#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/object.hpp"
#include "godot_cpp/classes/thread.hpp"
#include "godot_cpp/classes/worker_thread_pool.hpp"
#include "godot_cpp/templates/hash_map.hpp"
#include "godot_cpp/templates/local_vector.hpp"
#include "godot_cpp/templates/rid_owner.hpp"
#include "godot_cpp/variant/packed_float32_array.hpp"
#include "godot_cpp/variant/packed_vector2_array.hpp"
#include "godot_cpp/variant/packed_int32_array.hpp"
#include "godot_cpp/variant/packed_vector3_array.hpp"
#include "godot_cpp/variant/rid.hpp"
#include "godot_cpp/variant/transform3d.hpp"
#include "godot_cpp/variant/typed_array.hpp"
#include <phonon.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <variant>
#include <vector>

#include "AudioStreamSteamAudioListener.h"
#include "resampler/MultiChannelResampler.h"
#include "godot_cpp/classes/audio_effect.hpp"
#include "godot_cpp/classes/audio_stream_playback.hpp"

namespace godot {
class AudioStream;
}

struct ListenerData;

// Atomic wrapper safe inside LocalVector (which copies elements only under unique collections_mutex).
template <typename T>
struct RelocatableAtomic {
	std::atomic<T> value;
	RelocatableAtomic(T v = T{}) : value(v) {}
	RelocatableAtomic(const RelocatableAtomic &other) : value(other.value.load(std::memory_order_relaxed)) {}
	RelocatableAtomic &operator=(const RelocatableAtomic &other) {
		value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
		return *this;
	}
	T load(std::memory_order order = std::memory_order_acquire) const { return value.load(order); }
	void store(T v, std::memory_order order = std::memory_order_release) { value.store(v, order); }
	T exchange(T v, std::memory_order order = std::memory_order_acq_rel) { return value.exchange(v, order); }
};

enum class SimMembershipOp : uint8_t {
	None = 0,
	Add,
	Remove,
};

struct ListenerPlaybackEntry {
	godot::Ref<AudioStreamSteamAudioListenerPlayback> playback;
	int remaining_from_push_buffer = 0;
	// ONLY TO BE USED AS IDENTIFIER! RID of the source this playback belongs to
	// (when a listener plays back through a source, e.g. walkie talkie), else invalid.
	godot::RID is_playback_of_source;

	uint32_t debug_times_drained = 0;
};

struct ListenerSourceDBLevel {
	godot::RID source;
	godot::Vector3 position;
	float db_level = 0.0f;
};

// All configuration a listener pushes into the server. Plain data, read across
// threads; no SteamAudioListener node knowledge.
struct ListenerConfig {
	uint32_t mask = 1;
	float range = 0.0f;
	// A sensor listener reads per-source dB levels but never plays audio out (no
	// playbacks). It still needs sources to be mixed so their current_db_level stays
	// fresh — otherwise the level is only refreshed as a side-effect of mixing for a
	// real output listener that happens to be nearby.
	bool is_sensor = false;
	bool reflection_simulation_enabled = true;
	int num_refl_rays = 4096;
	int num_refl_bounces = 16;
	float refl_duration = 2.0f;
	int refl_ambisonics_order = 1;
	int refl_type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
	float irradiance_min_dist = 1.0f;
};

struct ListenerData {
	godot::RID self;
	ListenerConfig cfg;
	godot::String debug_name;

	std::atomic<IPLSimulator> simulator{ nullptr };
	bool simulator_reflection_enabled = false;
	bool pending_simulator_create = false;

	// This buffer will be filled by the pre-mixed and steam audio handled
	// audio data from all relevant sources (additive). When all relevant sources have
	// added to it, it will be pushed to the listener's playbacks, ready for a new round.
	godot::PackedVector2Array push_buffer;

	// Generation-based flow control (no maintained counters):
	// - "Contributors still pending" is derived each cycle in Phase 3 by scanning the
	//   relevant sources and checking last_contributed_generation against `generation`.
	// - "Drains still pending" is derived in Phase 1 by scanning playbacks for any with
	//   remaining_from_push_buffer > 0.
	// Generation counter: incremented each time the push_buffer is cleared, which arms a
	// fresh contribution round. Used with SourceListenerData::last_contributed_generation
	// to track which sources have already contributed without needing reset loops.
	uint32_t generation = 1;

	// Multiple playbacks per listener, protected by playbacks_mutex
	godot::LocalVector<ListenerPlaybackEntry> playbacks;
	std::unique_ptr<std::mutex> playbacks_mutex = std::make_unique<std::mutex>();

	godot::LocalVector<ListenerSourceDBLevel> source_db_levels;
	// Bumped every time source_db_levels is actually rebuilt in tick(). tick() has
	// early-outs (not running, direct-sim job still queued on a saturated
	// WorkerThreadPool) that leave the previous snapshot in place, while
	// SteamAudioListener::update_sensor_slots() keeps running every frame. Without a
	// version to compare against, that stale snapshot gets re-applied each frame and
	// always beats the just-decayed slot value, so the 30 dB/s decay can never win and
	// the sensor reports the last sound at the last position for as long as the stall
	// lasts. Consumers apply a snapshot at most once.
	uint64_t source_db_levels_version = 0;

	// Transform pushed by the owner (node or RID user); consumed on the main thread.
	godot::Transform3D pending_transform;
	bool has_transform = false;

	// Target reference frame (invalid = World Frame) and the frame currently bound to simulator.
	godot::RID frame;
	godot::RID active_sim_frame;

	// Cached simulation-space transform (in target frame), updated on main thread.
	godot::Transform3D last_trf;
	bool has_last_trf = false;
	bool dirty = false;

	// World-space pose and Ambisonics IR-frame decode pose, synchronized for mixing/reflection threads.
	IPLCoordinateSpace3 world_coords{};
	godot::Basis world_basis;
	godot::RID pending_ir_frame;
	godot::RID ir_frame;
	IPLCoordinateSpace3 refl_decode_coords{};
	std::unique_ptr<std::mutex> orientation_mutex = std::make_unique<std::mutex>();

	IPLSimulationSharedInputs pending_direct_shared_inputs{};
	bool direct_shared_inputs_dirty = false;
	IPLSimulationSharedInputs pending_refl_shared_inputs{};
	bool refl_shared_inputs_dirty = false;
	IPLSimulationSharedInputs pending_switch_direct_shared_inputs{};
	bool has_pending_frame_switch = false;

	IPLReflectionMixer reflection_mixer = nullptr;
	IPLAmbisonicsDecodeEffect ambisonics_decode_effect = nullptr;
	IPLAudioBuffer mixed_ambisonics_buffer{};
	IPLAudioBuffer decode_output_buffer{};

	uint32_t debug_times_pushed = 0;
};

struct SourceListenerData {
	// Stable pointer into listener_owner storage; identifies the listener this
	// per-pair state belongs to.
	ListenerData *listener = nullptr;
	float dist_to_listener = 0.0f;
	float doppler_pitch = 1.0f;
	bool out_of_range = false;
	RelocatableAtomic<bool> direct_simulated_once{ false };
	// Generation of the listener's push_buffer that this SLD last contributed to.
	// Compared against ListenerData::generation to determine if contribution is needed.
	uint32_t last_contributed_generation = 0;
	// mix_generation of the source that this pair last consumed. Compared against
	// SourceData::mix_generation to decide whether this listener still owes a
	// consumption of the source's current mix (gates the source's mix-reset).
	uint64_t last_consumed_mix = 0;
	RelocatableAtomic<IPLSource> source{ nullptr };
	IPLBinauralEffect binaural_effect = nullptr;
	IPLDirectEffect direct_effect = nullptr;
	IPLReflectionEffect reflection_effect = nullptr;
	IPLAmbisonicsDecodeEffect ambisonics_decode_effect = nullptr;
	IPLAudioBuffer input_buffer{};
	IPLAudioBuffer direct_buffer{};
	IPLAudioBuffer output_buffer{};
	IPLAudioBuffer ambisonics_buffer{};
	uint8_t skip_reflection_applies = 0;

	// Temporal smoothing and volumetric occlusion fallback for direct effect.
	float smoothed_occlusion = 1.0f;
	float smoothed_transmission[3] = { 1.0f, 1.0f, 1.0f };
	float last_occluded_transmission[3] = { 0.5f, 0.25f, 0.1f };
	bool has_smoothed_direct = false;

	// Deferred simulation lifecycle and reflection inputs, consumed in run_direct_job's commit phase.
	bool pending_create = false;
	SimMembershipOp pending_sim_membership = SimMembershipOp::None;
	IPLSourceSettings pending_source_settings{};
	IPLSimulationInputs pending_direct_inputs{};
	IPLSimulationInputs pending_refl_inputs{};
	bool refl_inputs_dirty = false;
	IPLSimulationInputs pending_switch_direct_inputs{};
	bool has_pending_switch_direct_inputs = false;

	uint32_t debug_times_contributed = 0;
};

struct PlaybackResampleState {
	std::shared_ptr<oboe::resampler::MultiChannelResampler> resampler;
	godot::Ref<godot::AudioBuffer> unconsumed_input_buffer;
	int unconsumed_input_count = 0;
	int unconsumed_input_index = 0;
};

struct SourcePlaybackEntry {
	godot::Ref<godot::AudioStreamPlayback> playback;
	int num_mixed_in_current_mixed_frames = 0;
	float volume_linear = 1.0f;
	float pitch_scale = 1.0f;

	uint32_t debug_num_mixed = 0;

	PlaybackResampleState resample_state;
};

// All configuration a source pushes into the server. Plain data, read across
// threads; no SteamAudioSource node knowledge.
struct SourceConfig {
	uint32_t layers = 1;
	float volume_db = 0.0f;
	bool direct_enabled = true;
	bool binaural_enabled = true;
	int binaural_interpolation = IPL_HRTFINTERPOLATION_NEAREST;
	float binaural_spatial_blend = 1.0f;
	bool distance_attenuation_enabled = true;
	float distance_attenuation_min = 1.0f;
	float distance_attenuation_max = 120.0f;
	bool air_absorption_enabled = true;
	godot::Vector3 air_absorption_coefficients = godot::Vector3(0.0002f, 0.0017f, 0.0182f);
	bool occlusion_enabled = false;
	int occlusion_type = IPL_OCCLUSIONTYPE_RAYCAST;
	float occlusion_radius = 1.0f;
	int occlusion_samples = 16;
	bool transmission_enabled = false;
	int transmission_type = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
	int transmission_rays = 16;
	bool reflection_enabled = false;
	float reflection_duration = 2.0f;
	float reflection_hybrid_delay = 0.5f;
	float doppler_factor = 1.0f;
};

struct SourceData {
	godot::RID self;
	SourceConfig cfg;
	godot::String debug_name;

	// Multiple playbacks per source, protected by playbacks_mutex
	godot::LocalVector<SourcePlaybackEntry> playbacks;
	std::unique_ptr<std::mutex> playbacks_mutex = std::make_unique<std::mutex>();

	godot::LocalVector<SourceListenerData> listener_data;

	// Pre-mixed audio frames from source playbacks.
	godot::PackedVector2Array mixed_frames;
	int mixed_frames_ready = 0;
	// Monotonic generation of the current full mix. Bumped once each time
	// mixed_frames becomes a complete frame. Consumption is tracked per-pair via
	// SourceListenerData::last_consumed_mix, so there is no maintained consumer
	// counter to desync: the mix-reset gate is recomputed each cycle by comparing
	// each active listener's last_consumed_mix against this value.
	uint64_t mix_generation = 0;
	bool is_skipping_mixing = false;

	// Realtime pacing for sensor-only mixing. When a source is mixed solely for sensor
	// listeners (no output listener consuming it) there is no audio-device backpressure,
	// so we throttle re-mixing to ~realtime using this wall-clock sample accumulator.
	double sensor_pacing_accumulator = 0.0;

	float current_db_level = 0;
	float volume_linear = 1.0f;

	// AudioEffectInstances created from the source's effect stack
	godot::LocalVector<godot::Ref<godot::AudioEffectInstance>> effect_instances;

	// Transform pushed by the owner (node or RID user); consumed on the main thread.
	godot::Transform3D pending_transform;
	bool has_transform = false;

	// Cached transform data, updated on main thread
	IPLCoordinateSpace3 cached_coords{};
	godot::Transform3D last_trf;

	uint32_t debug_times_mixed = 0;
};

// Two-level instanced geometry: every geometry owns an immutable sub-scene holding one
// multi-material IPLStaticMesh, mounted as an IPLInstancedMesh into the World scene and each Frame.
struct GeometryData {
	godot::RID self;
	bool dynamic = false;
	godot::RID frame; // invalid = World Frame; valid = moving reference frame RID
	IPLScene sub_scene = nullptr;
	IPLStaticMesh mesh = nullptr;
	IPLInstancedMesh world_instanced_mesh = nullptr;

	godot::Transform3D local_trf;
	godot::Transform3D world_last_trf;
	bool has_world_last_trf = false;
};

struct FrameInstance {
	IPLInstancedMesh instanced_mesh = nullptr;
	bool enabled = false;
	godot::Transform3D last_trf;
	bool has_last_trf = false;
};

struct FrameData {
	godot::RID self;
	std::atomic<bool> freed{ false };

	// Main-thread state used by tick().
	godot::Transform3D world_transform;
	godot::Transform3D inv_world_transform;
	bool has_world_transform = false;
	bool is_moving = false;

	// Commit-phase state owned exclusively by run_direct_job's idle-reflection phase.
	IPLScene scene = nullptr;
	godot::Transform3D sim_world_transform;
	godot::Transform3D sim_inv_world_transform;
	bool sim_is_moving = false;
	bool scene_dirty = false;
	bool need_rebuild_scene = false;
	bool committed_this_pass = false;
	godot::HashMap<GeometryData *, FrameInstance> instances;
};

// Helper to clean up a SourceListenerData's audio-side IPL resources
void cleanup_source_listener_data(SourceListenerData &sld, IPLContext ctx);

static constexpr int SOUNDFIELD_PROBE_COUNT = 36;

struct SoundfieldConfig {
	std::atomic<uint32_t> layers{ 1 };
	std::atomic<bool> enabled{ true };
	std::atomic<float> volume_db{ 0.0f };
	std::atomic<float> probe_distance{ 32.0f };
	std::atomic<float> smoothing_time{ 0.18f };
};

struct SoundfieldSectorState {
	int id = -1;
	godot::Vector3 nominal_direction = godot::Vector3(0, 1, 0);
	float volume_linear = 1.0f;
	float pitch_scale = 1.0f;
	godot::Ref<godot::AudioStreamPlayback> playback;
	PlaybackResampleState resample_state;
	godot::PackedFloat32Array mixed_frames;
	int mixed_frames_ready = 0;
};

struct SoundfieldListenerSectorDSP {
	int id = -1;
	IPLDirectEffect direct_effect = nullptr;
	IPLBinauralEffect binaural_effect = nullptr;
	IPLAudioBuffer input_buffer{};
	IPLAudioBuffer direct_buffer{};
	IPLAudioBuffer output_buffer{};

	float target_occlusion = 0.0f;
	float target_transmission[3] = { 0.2f, 0.05f, 0.015f };
	float last_occluded_transmission[3] = { 0.2f, 0.05f, 0.015f };
	godot::Vector3 target_direction = godot::Vector3(0, 1, 0);

	float smoothed_occlusion = 0.0f;
	float smoothed_transmission[3] = { 0.2f, 0.05f, 0.015f };
	godot::Vector3 smoothed_direction = godot::Vector3(0, 1, 0);
	bool has_smoothed = false;
};

struct SoundfieldListenerData {
	ListenerData *listener = nullptr;
	uint32_t last_contributed_generation = 0;
	uint64_t last_consumed_mix = 0;
	std::atomic<bool> probed_once{ false };

	// Direct-only Embree sky probe sources owned by listener->simulator.
	IPLSource probe_sources[SOUNDFIELD_PROBE_COUNT]{};
	bool probes_created = false;
	bool probes_pending_create = false;
	bool probes_attached = false;
	bool probes_in_sim = false;
	SimMembershipOp pending_probe_membership = SimMembershipOp::None;
	IPLSimulationInputs pending_probe_inputs[SOUNDFIELD_PROBE_COUNT]{};
	IPLSimulationInputs pending_switch_probe_inputs[SOUNDFIELD_PROBE_COUNT]{};
	bool has_pending_switch_probe_inputs = false;

	bool dsp_ready = false;
	std::vector<SoundfieldListenerSectorDSP> sector_dsp;

	std::mutex sim_state_mutex;
	float last_outdoorness = 0.0f;
	float last_output_rms_left = 0.0f;
	float last_output_rms_right = 0.0f;
};

struct SoundfieldData {
	godot::RID self;
	SoundfieldConfig cfg;
	godot::String debug_name;
	std::atomic<float> volume_linear{ 1.0f };
	uint64_t mix_generation = 0;
	bool mixed_ready = false;
	std::vector<SoundfieldSectorState> sectors;
	std::vector<std::unique_ptr<SoundfieldListenerData>> listener_data;
};

// Pending operation types for the mixing thread's audio-graph queue (RID-keyed).
struct PendingAddSource {
	godot::RID source;
};

struct PendingRemoveSource {
	godot::RID source;
};

struct PendingAddListener {
	godot::RID listener;
};

struct PendingRemoveListener {
	godot::RID listener;
};

struct PendingAddPlaybackToSource {
	godot::RID source;
	godot::Ref<godot::AudioStreamPlayback> playback;
	float volume_linear;
	float pitch_scale;
};

struct PendingAddPlaybackToListener {
	godot::RID listener;
	godot::Ref<AudioStreamSteamAudioListenerPlayback> playback;
};

struct PendingAddSoundfield {
	godot::RID soundfield;
};

struct PendingRemoveSoundfield {
	godot::RID soundfield;
};

struct PendingSoundfieldAddSector {
	godot::RID soundfield;
	int sector_id = -1;
	godot::Vector3 direction;
	godot::Ref<godot::AudioStreamPlayback> playback;
	float volume_linear = 1.0f;
	float pitch_scale = 1.0f;
};

struct PendingSoundfieldRemoveSector {
	godot::RID soundfield;
	int sector_id = -1;
};

struct PendingSoundfieldClearSectors {
	godot::RID soundfield;
};

struct PendingSoundfieldSectorVolume {
	godot::RID soundfield;
	int sector_id = -1;
	float volume_linear = 1.0f;
};

struct PendingSoundfieldSectorPitch {
	godot::RID soundfield;
	int sector_id = -1;
	float pitch_scale = 1.0f;
};

struct PendingSoundfieldSectorDirection {
	godot::RID soundfield;
	int sector_id = -1;
	godot::Vector3 direction;
};

using PendingOp = std::variant<
	PendingAddSource,
	PendingRemoveSource,
	PendingAddListener,
	PendingRemoveListener,
	PendingAddPlaybackToSource,
	PendingAddPlaybackToListener,
	PendingAddSoundfield,
	PendingRemoveSoundfield,
	PendingSoundfieldAddSector,
	PendingSoundfieldRemoveSector,
	PendingSoundfieldClearSectors,
	PendingSoundfieldSectorVolume,
	PendingSoundfieldSectorPitch,
	PendingSoundfieldSectorDirection
>;

// Geometry and frame ops drained exclusively inside run_direct_job's idle-reflection commit phase.
struct PendingGeomCreate {
	godot::RID geometry;
	godot::PackedVector3Array verts;
	godot::PackedInt32Array tris;
	godot::PackedInt32Array mat_indices;
	godot::PackedFloat32Array materials_flat;
	bool dynamic = false;
	godot::RID frame;
};

struct PendingGeomTransform {
	godot::RID geometry;
	godot::Transform3D transform;
};

struct PendingGeomMaterial {
	godot::RID geometry;
	godot::PackedFloat32Array material;
	int material_index = 0;
};

struct PendingGeomRemove {
	godot::RID geometry;
};

struct PendingFrameCreate {
	godot::RID frame;
};

struct PendingFrameTransform {
	godot::RID frame;
	godot::Transform3D transform;
};

struct PendingFrameMoving {
	godot::RID frame;
	bool moving = false;
};

struct PendingFrameRemove {
	godot::RID frame;
};

using PendingGeomOp = std::variant<
	PendingGeomCreate,
	PendingGeomTransform,
	PendingGeomMaterial,
	PendingGeomRemove,
	PendingFrameCreate,
	PendingFrameTransform,
	PendingFrameMoving,
	PendingFrameRemove
>;

struct PendingSimSourceRemoval {
	IPLSimulator simulator = nullptr; // retained handle, released after iplSourceRemove
	IPLSource source = nullptr;       // owned handle, released after iplSourceRemove
};

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

	// Reflection Simulation Thread
	godot::Ref<godot::Thread> simulation_thread;
	void simulation_thread_func();

	// Direct simulation runs as a fire-and-forget WorkerThreadPool job, kicked off
	// at the end of tick() and waited on (the barrier) at the start of the next
	// tick(). It also owns the commit + reflection-thread handshake.
	static void run_direct_job(void *p_self);
	godot::WorkerThreadPool::TaskID direct_task_id = 0;
	bool direct_job_pending = false;

	// Mixing Thread
	godot::Ref<godot::Thread> mixing_thread;
	void mixing_thread_func();

	// RID owners hold the actual data structs (stable addresses); the vectors below
	// are the per-frame iteration lists, maintained only inside apply_pending_ops().
	godot::RID_PtrOwner<SourceData, true> source_owner;
	godot::RID_PtrOwner<ListenerData, true> listener_owner;
	godot::RID_PtrOwner<GeometryData, true> geometry_owner;
	godot::RID_PtrOwner<FrameData, true> frame_owner;
	godot::RID_PtrOwner<SoundfieldData, true> soundfield_owner;

	godot::LocalVector<SourceData *> sources;
	std::vector<ListenerData *> listeners;
	godot::LocalVector<GeometryData *> geometries;
	godot::LocalVector<FrameData *> frames;
	godot::LocalVector<SoundfieldData *> soundfields;

	// Precomputed hemisphere probe directions for soundfield occlusion/transmission probing.
	godot::Vector3 sky_probe_dirs[SOUNDFIELD_PROBE_COUNT];
	std::atomic<int> next_soundfield_sector_id{ 1 };

	// Protects sources and listeners (geometries is owned exclusively by the simulation commit phase).
	// Unique-locked briefly by the mixing thread to apply pending ops.
	// Shared-locked by tick(), process_audio() (after applying ops),
	// simulation_thread, and debug functions.
	std::shared_mutex collections_mutex;

	// Pending audio-graph operations queue: callers enqueue, mixing thread drains.
	std::mutex pending_ops_mutex;
	std::vector<PendingOp> pending_ops;
	void apply_pending_ops();

	// Pending geometry operations queue: main thread enqueues, run_direct_job commit phase drains.
	std::mutex geom_ops_mutex;
	std::vector<PendingGeomOp> pending_geom_ops;
	void apply_pending_geom_ops();

	// Deferred IPLSource/IPLSimulator teardown pushed by mixing thread, drained in run_direct_job commit phase.
	std::mutex sim_ops_mutex;
	std::vector<PendingSimSourceRemoval> pending_sim_source_removals;
	std::vector<IPLSimulator> pending_sim_releases;

	// Builds audio-side per-listener IPL objects (mixer/decode) from ld->cfg.
	void create_listener_audio(ListenerData *ld);
	// Builds the IPLSimulator for ld inside run_direct_job's commit phase.
	void create_listener_simulator(ListenerData *ld);
	// Soundfield DSP helpers (called on mix thread / finish()).
	std::unique_ptr<SoundfieldListenerData> create_soundfield_listener_data(SoundfieldData *sf, ListenerData *ld);
	SoundfieldListenerSectorDSP *find_soundfield_sector_dsp(SoundfieldListenerData &sfld, int sector_id);
	bool ensure_soundfield_sector_dsp(SoundfieldListenerSectorDSP &dsp);
	void cleanup_soundfield_sector_dsp(SoundfieldListenerSectorDSP &dsp);
	void release_soundfield_listener_dsp(SoundfieldListenerData &sfld);
	void cleanup_soundfield_listener_data(SoundfieldListenerData &sfld);

	// Scene / two-level instance helpers (called only in init / run_direct_job commit phase).
	IPLScene create_ipl_scene();
	FrameData *find_sim_frame(godot::RID frame_rid);
	FrameData *get_valid_frame_main(godot::RID frame_rid);
	IPLScene scene_for_frame(godot::RID frame_rid);
	godot::Transform3D compute_world_trf(GeometryData *g);
	void sync_geometry_in_world(GeometryData *g);
	void sync_geometry_in_frame(GeometryData *g, FrameData *f);
	void remove_geometry_from_frame(GeometryData *g, FrameData *f);
	void rebuild_world_scene();
	void rebuild_frame_scene(FrameData *f);

	std::atomic<bool> is_refl_thread_processing;
	std::atomic<bool> new_inputs_set;

	std::atomic<float> mixing_thread_usage_pct{0.0f};
	std::atomic<float> stress_mitigation{0.0f};
	std::atomic<float> sim_thread_avg_duration_ms{0.0f};
	std::atomic<float> direct_job_avg_duration_ms{0.0f};
	std::mutex refl_mux;
	std::condition_variable refl_cv;

	bool scene_dirty = false;

	godot::Ref<godot::AudioBuffer> mix_scratch_buffer;
	godot::PackedVector2Array soundfield_mix_scratch;
	godot::Ref<godot::AudioBuffer> effect_src_buffer;
	godot::Ref<godot::AudioBuffer> effect_dst_buffer;

	std::shared_ptr<oboe::resampler::MultiChannelResampler> create_resampler(int channels) const;
	template <int Channels, typename EmitFn>
	int pull_playback_frames(
			const godot::Ref<godot::AudioStreamPlayback> &playback,
			PlaybackResampleState &state,
			float pitch_scale,
			int to_pull,
			EmitFn &&emit_frame);

	// Cached audio settings, set once during init()
	IPLAudioSettings cached_audio_settings{};
	int godot_mix_rate = 0;

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
	// Runs init() on first use; no GDExtension init level is late enough.
	void ensure_initialized();
	void finish();

	void tick(float delta);

	// ── Listener RID API ─────────────────────────────────────────────────────
	godot::RID listener_create();
	godot::RID listener_create_with_config(const ListenerConfig &cfg, const godot::String &debug_name, const godot::Transform3D &xform, bool has_xform);
	void listener_free(godot::RID listener);
	void listener_set_transform(godot::RID listener, const godot::Transform3D &xform);
	void listener_set_frame(godot::RID listener, godot::RID frame);
	godot::RID listener_get_frame(godot::RID listener);
	godot::RID listener_get_active_frame(godot::RID listener);
	void listener_set_mask(godot::RID listener, uint32_t mask);
	void listener_set_range(godot::RID listener, float range);
	void listener_set_sensor(godot::RID listener, bool enabled);
	void listener_set_reflection(godot::RID listener, bool enabled, int rays, int bounces, float duration, int ambisonics_order, int type, float irradiance_min_dist);
	void listener_set_debug_name(godot::RID listener, const godot::String &name);
	void listener_add_playback(godot::RID listener, godot::Ref<AudioStreamSteamAudioListenerPlayback> playback);
	// Per-source dB levels for this listener (copied out, RID-keyed) — for GDScript.
	godot::Array listener_get_source_db_levels(godot::RID listener);
	// C++-only hot-path accessor (not bound); returns a reference valid until the
	// next tick(). Used by the SteamAudioListener node for its sensor slots.
	// out_version receives ListenerData::source_db_levels_version so the caller can
	// tell a freshly rebuilt snapshot from one a stalled tick left behind (0 when the
	// listener is unknown).
	const godot::LocalVector<ListenerSourceDBLevel> &listener_get_source_db_levels_ref(godot::RID listener, uint64_t *out_version = nullptr);

	// ── Source RID API ───────────────────────────────────────────────────────
	godot::RID source_create();
	godot::RID source_create_with_config(const SourceConfig &cfg, const godot::String &debug_name, const godot::Transform3D &xform, bool has_xform);
	void source_free(godot::RID source);
	void source_set_transform(godot::RID source, const godot::Transform3D &xform);
	void source_set_layers(godot::RID source, uint32_t layers);
	void source_set_volume_db(godot::RID source, float volume_db);
	void source_set_doppler_factor(godot::RID source, float factor);
	void source_set_direct_enabled(godot::RID source, bool enabled);
	void source_set_binaural(godot::RID source, bool enabled, int interpolation, float spatial_blend);
	void source_set_distance_attenuation(godot::RID source, bool enabled, float min, float max);
	void source_set_air_absorption(godot::RID source, bool enabled, const godot::Vector3 &coefficients = godot::Vector3(0.0002f, 0.0017f, 0.0182f));
	void source_set_occlusion(godot::RID source, bool enabled, int type, float radius, int samples);
	void source_set_transmission(godot::RID source, bool enabled, int type, int rays);
	void source_set_reflection(godot::RID source, bool enabled, float duration, float hybrid_delay);
	void source_set_effect_stack(godot::RID source, const godot::TypedArray<godot::AudioEffect> &stack);
	void source_set_debug_name(godot::RID source, const godot::String &name);
	void source_add_playback(godot::RID source, godot::Ref<godot::AudioStreamPlayback> p_playback, float p_volume_db, float p_pitch_scale);
	void source_set_playback_volume(godot::RID source, godot::Ref<godot::AudioStreamPlayback> p_playback, float p_volume_db);
	void source_set_playback_pitch(godot::RID source, godot::Ref<godot::AudioStreamPlayback> p_playback, float p_pitch_scale);
	int source_get_num_active_playbacks(godot::RID source);

	// ── Geometry RID API ─────────────────────────────────────────────────────
	// Materials are passed as 7 floats per material: absorption[3], scattering, transmission[3].
	godot::RID geometry_create_static(const godot::PackedVector3Array &verts, const godot::PackedInt32Array &tris, const godot::PackedFloat32Array &material);
	godot::RID geometry_create_dynamic(const godot::PackedVector3Array &verts, const godot::PackedInt32Array &tris, const godot::PackedFloat32Array &material);
	godot::RID geometry_create_batched(const godot::PackedVector3Array &verts, const godot::PackedInt32Array &tris, const godot::PackedInt32Array &mat_indices, const godot::PackedFloat32Array &materials_flat, bool dynamic, godot::RID frame = godot::RID());
	void geometry_set_transform(godot::RID geometry, const godot::Transform3D &xform);
	void geometry_set_material(godot::RID geometry, const godot::PackedFloat32Array &material, int material_index = 0);
	void geometry_free(godot::RID geometry);

	// ── Moving Reference Frame RID API ───────────────────────────────────────
	godot::RID frame_create();
	void frame_set_world_transform(godot::RID frame, const godot::Transform3D &xform);
	void frame_set_moving(godot::RID frame, bool moving);
	bool frame_is_moving(godot::RID frame);
	void frame_free(godot::RID frame);

	// ── Soundfield RID API ───────────────────────────────────────────────────
	godot::RID soundfield_create();
	void soundfield_free(godot::RID soundfield);
	void soundfield_set_layers(godot::RID soundfield, uint32_t layers);
	void soundfield_set_enabled(godot::RID soundfield, bool enabled);
	void soundfield_set_volume_db(godot::RID soundfield, float volume_db);
	void soundfield_set_probe_distance(godot::RID soundfield, float distance);
	void soundfield_set_smoothing_time(godot::RID soundfield, float seconds);
	void soundfield_set_debug_name(godot::RID soundfield, const godot::String &name);
	int soundfield_add_sector(godot::RID soundfield, const godot::Vector3 &direction, godot::Ref<godot::AudioStreamPlayback> playback, float volume_db = 0.0f, float pitch_scale = 1.0f, float from_position = 0.0f);
	void soundfield_remove_sector(godot::RID soundfield, int sector_id);
	void soundfield_set_sector_volume_db(godot::RID soundfield, int sector_id, float volume_db);
	void soundfield_set_sector_pitch_scale(godot::RID soundfield, int sector_id, float pitch_scale);
	void soundfield_set_sector_direction(godot::RID soundfield, int sector_id, const godot::Vector3 &direction);
	void soundfield_clear_sectors(godot::RID soundfield);
	godot::Dictionary soundfield_get_debug_state(godot::RID soundfield, godot::RID listener);

	// Audio pulling and effect application. dt is the wall-clock time (seconds) since
	// the previous process_audio cycle, used to pace sensor-only source mixing.
	void process_audio(double dt);

	int get_frame_size() const { return cached_audio_settings.frameSize; }
	// Trades latency against CPU: halving it halves the source mix block latency and
	// doubles the IPL effect call rate. Only settable before the server initializes
	// (the HRTF, effects and simulators are built against it), so before the first
	// SteamAudioSource/Listener enters the tree. Errors if it is already too late.
	void set_frame_size(int p_frame_size);
	bool get_is_initialized() const { return is_initialized; }
	int get_sampling_rate() const { return cached_audio_settings.samplingRate; }
	int get_godot_mix_rate() const { return godot_mix_rate; }
	// Capacity of a listener's output ring, which is also its output latency in frames.
	int get_listener_ring_capacity_frames() const;
	// Occupancy of the listener's playbacks (max across them), in frames.
	int listener_get_output_latency_frames(godot::RID listener);
	// Allocated capacity, i.e. get_listener_ring_capacity_frames() rounded up to a
	// power of two.
	int listener_get_allocated_ring_capacity_frames(godot::RID listener);

	// Debug functions
	float get_mixing_thread_usage_pct() const;
	float get_stress_mitigation() const;
	float get_sim_thread_avg_duration_ms() const;
	float get_direct_job_avg_duration_ms() const;
	int get_source_count();
	int get_listener_count();
	int get_geometry_count();
	int get_frame_count();
	int get_soundfield_count();
	godot::String get_source_name(int index);
	godot::String get_listener_name(int index);
	godot::String get_soundfield_name(int index);
	godot::String get_source_debug_string(int index);
	godot::String get_listener_debug_string(int index);
	godot::String get_soundfield_debug_string(int index);

private:
	// Internal RID creation shared by static, dynamic, and batched multi-material geometry.
	godot::RID geometry_create_internal(const godot::PackedVector3Array &verts, const godot::PackedInt32Array &tris, const godot::PackedInt32Array &mat_indices, const godot::PackedFloat32Array &materials_flat, bool dynamic, godot::RID frame);
};

#endif // STEAM_AUDIO_SERVER_H
