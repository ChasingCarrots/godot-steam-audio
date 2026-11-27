#include "soundfield.hpp"
#include "godot_cpp/classes/audio_stream_playback.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/variant/utility_functions.hpp"
#include "server.hpp"

using namespace godot;

static float resolve_start_offset(const Ref<AudioStream> &p_stream, float p_from_position) {
	if (p_from_position < 0.0f) {
		double length = p_stream.is_valid() ? p_stream->get_length() : 0.0;
		return length > 0.0 ? static_cast<float>(UtilityFunctions::randf() * length) : 0.0f;
	}
	return p_from_position;
}

void SteamAudioSoundfieldSector::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_direction"), &SteamAudioSoundfieldSector::get_direction);
	ClassDB::bind_method(D_METHOD("set_direction", "direction"), &SteamAudioSoundfieldSector::set_direction);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "direction"), "set_direction", "get_direction");

	ClassDB::bind_method(D_METHOD("get_stream"), &SteamAudioSoundfieldSector::get_stream);
	ClassDB::bind_method(D_METHOD("set_stream", "stream"), &SteamAudioSoundfieldSector::set_stream);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "stream", PROPERTY_HINT_RESOURCE_TYPE, "AudioStream"), "set_stream", "get_stream");

	ClassDB::bind_method(D_METHOD("get_volume_db"), &SteamAudioSoundfieldSector::get_volume_db);
	ClassDB::bind_method(D_METHOD("set_volume_db", "volume_db"), &SteamAudioSoundfieldSector::set_volume_db);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume_db", PROPERTY_HINT_RANGE, "-80.0,24.0,0.1,suffix:dB"), "set_volume_db", "get_volume_db");

	ClassDB::bind_method(D_METHOD("get_pitch_scale"), &SteamAudioSoundfieldSector::get_pitch_scale);
	ClassDB::bind_method(D_METHOD("set_pitch_scale", "pitch_scale"), &SteamAudioSoundfieldSector::set_pitch_scale);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pitch_scale", PROPERTY_HINT_RANGE, "0.05,4.0,0.01"), "set_pitch_scale", "get_pitch_scale");

	ClassDB::bind_method(D_METHOD("get_start_offset"), &SteamAudioSoundfieldSector::get_start_offset);
	ClassDB::bind_method(D_METHOD("set_start_offset", "start_offset"), &SteamAudioSoundfieldSector::set_start_offset);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "start_offset", PROPERTY_HINT_RANGE, "-1.0,600.0,0.01,or_greater,suffix:s"), "set_start_offset", "get_start_offset");
}

SteamAudioSoundfieldSector::SteamAudioSoundfieldSector() {}
SteamAudioSoundfieldSector::~SteamAudioSoundfieldSector() {}

void SteamAudioSoundfield::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_rid"), &SteamAudioSoundfield::get_rid);

	ClassDB::bind_method(D_METHOD("get_enabled"), &SteamAudioSoundfield::get_enabled);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &SteamAudioSoundfield::set_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "get_enabled");

	ClassDB::bind_method(D_METHOD("get_layers"), &SteamAudioSoundfield::get_layers);
	ClassDB::bind_method(D_METHOD("set_layers", "layers"), &SteamAudioSoundfield::set_layers);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "layers", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_layers", "get_layers");

	ClassDB::bind_method(D_METHOD("get_volume_db"), &SteamAudioSoundfield::get_volume_db);
	ClassDB::bind_method(D_METHOD("set_volume_db", "volume_db"), &SteamAudioSoundfield::set_volume_db);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume_db", PROPERTY_HINT_RANGE, "-80.0,24.0,0.1,suffix:dB"), "set_volume_db", "get_volume_db");

	ClassDB::bind_method(D_METHOD("get_probe_distance"), &SteamAudioSoundfield::get_probe_distance);
	ClassDB::bind_method(D_METHOD("set_probe_distance", "distance"), &SteamAudioSoundfield::set_probe_distance);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_distance", PROPERTY_HINT_RANGE, "2.0,150.0,0.5,suffix:m"), "set_probe_distance", "get_probe_distance");

	ClassDB::bind_method(D_METHOD("get_smoothing_time"), &SteamAudioSoundfield::get_smoothing_time);
	ClassDB::bind_method(D_METHOD("set_smoothing_time", "seconds"), &SteamAudioSoundfield::set_smoothing_time);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "smoothing_time", PROPERTY_HINT_RANGE, "0.01,2.0,0.01,suffix:s"), "set_smoothing_time", "get_smoothing_time");

	ClassDB::bind_method(D_METHOD("get_openness_exponent"), &SteamAudioSoundfield::get_openness_exponent);
	ClassDB::bind_method(D_METHOD("set_openness_exponent", "exponent"), &SteamAudioSoundfield::set_openness_exponent);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "openness_exponent", PROPERTY_HINT_RANGE, "0.1,2.0,0.05"), "set_openness_exponent", "get_openness_exponent");

	ClassDB::bind_method(D_METHOD("get_aperture_boost"), &SteamAudioSoundfield::get_aperture_boost);
	ClassDB::bind_method(D_METHOD("set_aperture_boost", "boost"), &SteamAudioSoundfield::set_aperture_boost);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "aperture_boost", PROPERTY_HINT_RANGE, "0.0,1.0,0.02"), "set_aperture_boost", "get_aperture_boost");

	ClassDB::bind_method(D_METHOD("get_portal_gain"), &SteamAudioSoundfield::get_portal_gain);
	ClassDB::bind_method(D_METHOD("set_portal_gain", "gain"), &SteamAudioSoundfield::set_portal_gain);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "portal_gain", PROPERTY_HINT_RANGE, "0.0,1.5,0.05"), "set_portal_gain", "get_portal_gain");

	ClassDB::bind_method(D_METHOD("get_sectors"), &SteamAudioSoundfield::get_sectors);
	ClassDB::bind_method(D_METHOD("set_sectors", "sectors"), &SteamAudioSoundfield::set_sectors);
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "sectors", PROPERTY_HINT_TYPE_STRING, String::num(Variant::OBJECT) + "/" + String::num(PROPERTY_HINT_RESOURCE_TYPE) + ":SteamAudioSoundfieldSector"), "set_sectors", "get_sectors");

	ClassDB::bind_method(D_METHOD("add_sector", "direction", "stream", "volume_db", "pitch_scale", "from_position"), &SteamAudioSoundfield::add_sector, DEFVAL(0.0f), DEFVAL(1.0f), DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("remove_sector", "id"), &SteamAudioSoundfield::remove_sector);
	ClassDB::bind_method(D_METHOD("set_sector_volume_db", "id", "volume_db"), &SteamAudioSoundfield::set_sector_volume_db);
	ClassDB::bind_method(D_METHOD("set_sector_pitch_scale", "id", "pitch_scale"), &SteamAudioSoundfield::set_sector_pitch_scale);
	ClassDB::bind_method(D_METHOD("set_sector_direction", "id", "direction"), &SteamAudioSoundfield::set_sector_direction);
	ClassDB::bind_method(D_METHOD("clear_sectors"), &SteamAudioSoundfield::clear_sectors);
}

SteamAudioSoundfield::SteamAudioSoundfield() {}

SteamAudioSoundfield::~SteamAudioSoundfield() {
	unregister_from_server();
}

void SteamAudioSoundfield::_notification(int p_what) {
	if (Engine::get_singleton()->is_editor_hint())
		return;

	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (rid.is_valid()) {
				if (auto *srv = SteamAudioServer::get_singleton()) {
					srv->soundfield_set_debug_name(rid, get_name());
				}
			} else {
				ensure_registered();
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			unregister_from_server();
		} break;
	}
}

void SteamAudioSoundfield::ensure_registered() {
	if (rid.is_valid() || Engine::get_singleton()->is_editor_hint())
		return;
	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (!srv)
		return;
	rid = srv->soundfield_create();
	if (rid.is_valid()) {
		push_config();
		if (sectors_dirty_from_property) {
			active_sectors.clear();
			populate_active_sectors_from_resources();
			sectors_dirty_from_property = false;
		}
		register_cached_sectors();
	}
}

void SteamAudioSoundfield::unregister_from_server() {
	for (auto &sec : active_sectors) {
		sec.server_id = -1;
	}
	if (!rid.is_valid())
		return;
	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (srv) {
		srv->soundfield_free(rid);
	}
	rid = RID();
}

void SteamAudioSoundfield::push_config() {
	if (!rid.is_valid())
		return;
	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (!srv)
		return;
	srv->soundfield_set_enabled(rid, enabled);
	srv->soundfield_set_layers(rid, layers);
	srv->soundfield_set_volume_db(rid, volume_db);
	srv->soundfield_set_probe_distance(rid, probe_distance);
	srv->soundfield_set_smoothing_time(rid, smoothing_time);
	srv->soundfield_set_openness_exponent(rid, openness_exponent);
	srv->soundfield_set_aperture_boost(rid, aperture_boost);
	srv->soundfield_set_portal_gain(rid, portal_gain);
	srv->soundfield_set_debug_name(rid, get_name());
}

void SteamAudioSoundfield::populate_active_sectors_from_resources() {
	for (int i = 0; i < sectors.size(); ++i) {
		Ref<SteamAudioSoundfieldSector> sec = sectors[i];
		if (sec.is_null() || sec->get_stream().is_null())
			continue;
		active_sectors.push_back(SectorDescriptor{
			next_sector_handle_id++,
			-1,
			sec->get_direction(),
			sec->get_stream(),
			sec->get_volume_db(),
			sec->get_pitch_scale(),
			sec->get_start_offset(),
		});
	}
}

void SteamAudioSoundfield::register_cached_sectors() {
	if (!rid.is_valid())
		return;
	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (!srv)
		return;
	for (auto &sec : active_sectors) {
		if (sec.server_id >= 0 || sec.stream.is_null())
			continue;
		Ref<AudioStreamPlayback> pb = sec.stream->instantiate_playback();
		if (pb.is_null())
			continue;
		float offset = resolve_start_offset(sec.stream, sec.from_position);
		sec.server_id = srv->soundfield_add_sector(rid, sec.direction, pb, sec.volume_db, sec.pitch_scale, offset);
	}
}

SteamAudioSoundfield::SectorDescriptor *SteamAudioSoundfield::find_sector_by_handle(int p_id) {
	for (auto &sec : active_sectors) {
		if (sec.handle_id == p_id)
			return &sec;
	}
	return nullptr;
}

void SteamAudioSoundfield::set_enabled(bool p_enabled) {
	enabled = p_enabled;
	if (rid.is_valid()) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_enabled(rid, enabled);
		}
	}
}

void SteamAudioSoundfield::set_layers(uint32_t p_layers) {
	layers = p_layers;
	if (rid.is_valid()) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_layers(rid, layers);
		}
	}
}

void SteamAudioSoundfield::set_volume_db(float p_db) {
	volume_db = p_db;
	if (rid.is_valid()) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_volume_db(rid, volume_db);
		}
	}
}

void SteamAudioSoundfield::set_probe_distance(float p_dist) {
	probe_distance = MAX(1.0f, p_dist);
	if (rid.is_valid()) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_probe_distance(rid, probe_distance);
		}
	}
}

void SteamAudioSoundfield::set_smoothing_time(float p_time) {
	smoothing_time = MAX(0.005f, p_time);
	if (rid.is_valid()) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_smoothing_time(rid, smoothing_time);
		}
	}
}

void SteamAudioSoundfield::set_openness_exponent(float p_exponent) {
	openness_exponent = CLAMP(p_exponent, 0.1f, 3.0f);
	if (rid.is_valid()) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_openness_exponent(rid, openness_exponent);
		}
	}
}

void SteamAudioSoundfield::set_aperture_boost(float p_boost) {
	aperture_boost = CLAMP(p_boost, 0.0f, 1.0f);
	if (rid.is_valid()) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_aperture_boost(rid, aperture_boost);
		}
	}
}

void SteamAudioSoundfield::set_portal_gain(float p_gain) {
	portal_gain = CLAMP(p_gain, 0.0f, 2.0f);
	if (rid.is_valid()) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_portal_gain(rid, portal_gain);
		}
	}
}

void SteamAudioSoundfield::set_sectors(const TypedArray<SteamAudioSoundfieldSector> &p_sectors) {
	sectors = p_sectors;
	if (Engine::get_singleton()->is_editor_hint())
		return;
	if (rid.is_valid()) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_clear_sectors(rid);
		}
		active_sectors.clear();
		populate_active_sectors_from_resources();
		sectors_dirty_from_property = false;
		register_cached_sectors();
	} else {
		sectors_dirty_from_property = true;
	}
}

int SteamAudioSoundfield::add_sector(const Vector3 &p_direction, const Ref<AudioStream> &p_stream, float p_volume_db, float p_pitch_scale, float p_from_position) {
	if (Engine::get_singleton()->is_editor_hint() || p_stream.is_null())
		return -1;
	ensure_registered();
	if (!rid.is_valid())
		return -1;
	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (!srv)
		return -1;
	Ref<AudioStreamPlayback> pb = p_stream->instantiate_playback();
	if (pb.is_null())
		return -1;
	float pitch = MAX(0.01f, p_pitch_scale);
	float offset = resolve_start_offset(p_stream, p_from_position);
	int server_id = srv->soundfield_add_sector(rid, p_direction, pb, p_volume_db, pitch, offset);
	if (server_id < 0)
		return -1;
	int handle_id = next_sector_handle_id++;
	active_sectors.push_back(SectorDescriptor{
		handle_id,
		server_id,
		p_direction,
		p_stream,
		p_volume_db,
		pitch,
		p_from_position,
	});
	return handle_id;
}

void SteamAudioSoundfield::remove_sector(int p_id) {
	for (auto it = active_sectors.begin(); it != active_sectors.end(); ++it) {
		if (it->handle_id == p_id) {
			if (rid.is_valid() && it->server_id >= 0) {
				if (auto *srv = SteamAudioServer::get_singleton()) {
					srv->soundfield_remove_sector(rid, it->server_id);
				}
			}
			active_sectors.erase(it);
			return;
		}
	}
}

void SteamAudioSoundfield::set_sector_volume_db(int p_id, float p_volume_db) {
	SectorDescriptor *sec = find_sector_by_handle(p_id);
	if (!sec)
		return;
	sec->volume_db = p_volume_db;
	if (rid.is_valid() && sec->server_id >= 0) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_sector_volume_db(rid, sec->server_id, p_volume_db);
		}
	}
}

void SteamAudioSoundfield::set_sector_pitch_scale(int p_id, float p_pitch_scale) {
	SectorDescriptor *sec = find_sector_by_handle(p_id);
	if (!sec)
		return;
	float pitch = MAX(0.01f, p_pitch_scale);
	sec->pitch_scale = pitch;
	if (rid.is_valid() && sec->server_id >= 0) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_sector_pitch_scale(rid, sec->server_id, pitch);
		}
	}
}

void SteamAudioSoundfield::set_sector_direction(int p_id, const Vector3 &p_direction) {
	SectorDescriptor *sec = find_sector_by_handle(p_id);
	if (!sec)
		return;
	sec->direction = p_direction;
	if (rid.is_valid() && sec->server_id >= 0) {
		if (auto *srv = SteamAudioServer::get_singleton()) {
			srv->soundfield_set_sector_direction(rid, sec->server_id, p_direction);
		}
	}
}

void SteamAudioSoundfield::clear_sectors() {
	active_sectors.clear();
	sectors_dirty_from_property = false;
	if (!rid.is_valid())
		return;
	if (auto *srv = SteamAudioServer::get_singleton()) {
		srv->soundfield_clear_sectors(rid);
	}
}
