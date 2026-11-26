#ifndef STEAM_AUDIO_SOUNDFIELD_HPP
#define STEAM_AUDIO_SOUNDFIELD_HPP

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <vector>

class SteamAudioSoundfieldSector : public godot::Resource {
	GDCLASS(SteamAudioSoundfieldSector, godot::Resource);

private:
	godot::Vector3 direction = godot::Vector3(0, 1, 0);
	godot::Ref<godot::AudioStream> stream;
	float volume_db = 0.0f;
	float pitch_scale = 1.0f;
	float start_offset = 0.0f;

protected:
	static void _bind_methods();

public:
	SteamAudioSoundfieldSector();
	~SteamAudioSoundfieldSector();

	godot::Vector3 get_direction() const { return direction; }
	void set_direction(const godot::Vector3 &p_direction) { direction = p_direction; }

	godot::Ref<godot::AudioStream> get_stream() const { return stream; }
	void set_stream(const godot::Ref<godot::AudioStream> &p_stream) { stream = p_stream; }

	float get_volume_db() const { return volume_db; }
	void set_volume_db(float p_volume_db) { volume_db = p_volume_db; }

	float get_pitch_scale() const { return pitch_scale; }
	void set_pitch_scale(float p_pitch_scale) { pitch_scale = godot::MAX(0.01f, p_pitch_scale); }

	float get_start_offset() const { return start_offset; }
	void set_start_offset(float p_start_offset) { start_offset = p_start_offset; }
};

class SteamAudioSoundfield : public godot::Node {
	GDCLASS(SteamAudioSoundfield, godot::Node);

private:
	struct SectorDescriptor {
		int handle_id = -1;
		int server_id = -1;
		godot::Vector3 direction = godot::Vector3(0, 1, 0);
		godot::Ref<godot::AudioStream> stream;
		float volume_db = 0.0f;
		float pitch_scale = 1.0f;
		float from_position = 0.0f;
	};

	godot::RID rid;

	bool enabled = true;
	uint32_t layers = 1;
	float volume_db = 0.0f;
	float probe_distance = 32.0f;
	float smoothing_time = 0.18f;
	godot::TypedArray<SteamAudioSoundfieldSector> sectors;

	std::vector<SectorDescriptor> active_sectors;
	int next_sector_handle_id = 1;
	bool sectors_dirty_from_property = false;

	void ensure_registered();
	void unregister_from_server();
	void push_config();
	void populate_active_sectors_from_resources();
	void register_cached_sectors();
	SectorDescriptor *find_sector_by_handle(int p_id);

protected:
	static void _bind_methods();

public:
	SteamAudioSoundfield();
	~SteamAudioSoundfield();

	void _notification(int p_what);

	godot::RID get_rid() const { return rid; }

	bool get_enabled() const { return enabled; }
	void set_enabled(bool p_enabled);

	uint32_t get_layers() const { return layers; }
	void set_layers(uint32_t p_layers);

	float get_volume_db() const { return volume_db; }
	void set_volume_db(float p_db);

	float get_probe_distance() const { return probe_distance; }
	void set_probe_distance(float p_dist);

	float get_smoothing_time() const { return smoothing_time; }
	void set_smoothing_time(float p_time);

	godot::TypedArray<SteamAudioSoundfieldSector> get_sectors() const { return sectors; }
	void set_sectors(const godot::TypedArray<SteamAudioSoundfieldSector> &p_sectors);

	int add_sector(const godot::Vector3 &p_direction, const godot::Ref<godot::AudioStream> &p_stream, float p_volume_db = 0.0f, float p_pitch_scale = 1.0f, float p_from_position = 0.0f);
	void remove_sector(int p_id);
	void set_sector_volume_db(int p_id, float p_volume_db);
	void set_sector_pitch_scale(int p_id, float p_pitch_scale);
	void set_sector_direction(int p_id, const godot::Vector3 &p_direction);
	void clear_sectors();
};

#endif // STEAM_AUDIO_SOUNDFIELD_HPP
