#include "listener.hpp"
#include "godot_cpp/classes/audio_stream_player.hpp"
#include "godot_cpp/classes/audio_stream_player3d.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/project_settings.hpp"
#include "server.hpp"
#include "source.hpp"

using namespace godot;

void SteamAudioListenerSensorSlot::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_source_rid"), &SteamAudioListenerSensorSlot::get_source_rid);
	ClassDB::bind_method(D_METHOD("get_steam_audio_source"), &SteamAudioListenerSensorSlot::get_steam_audio_source);
	ClassDB::bind_method(D_METHOD("set_position", "p_position"), &SteamAudioListenerSensorSlot::set_position);
	ClassDB::bind_method(D_METHOD("get_position"), &SteamAudioListenerSensorSlot::get_position);
	ClassDB::bind_method(D_METHOD("set_db_level", "db_level"), &SteamAudioListenerSensorSlot::set_db_level);
	ClassDB::bind_method(D_METHOD("get_db_level"), &SteamAudioListenerSensorSlot::get_db_level);

	ADD_PROPERTY(PropertyInfo(Variant::RID, "source_rid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "", "get_source_rid");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "position"), "set_position", "get_position");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "db_level"), "set_db_level", "get_db_level");
}

void SteamAudioListenerSensorSlot::set_source_rid(const RID &p_source) { source = p_source; }
RID SteamAudioListenerSensorSlot::get_source_rid() const { return source; }
SteamAudioSource *SteamAudioListenerSensorSlot::get_steam_audio_source() { return SteamAudioSource::for_rid(source); }
void SteamAudioListenerSensorSlot::set_position(const godot::Vector3 &p_position) { position = p_position; }
godot::Vector3 SteamAudioListenerSensorSlot::get_position() { return position; }
void SteamAudioListenerSensorSlot::set_db_level(float p_db_level) { db_level = p_db_level; }
float SteamAudioListenerSensorSlot::get_db_level() { return db_level; }

void SteamAudioListener::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_rid"), &SteamAudioListener::get_rid);
	ClassDB::bind_method(D_METHOD("play_on_audiostreamplayer", "audiostreamplayer"), &SteamAudioListener::play_on_audiostreamplayer);

	ClassDB::bind_method(D_METHOD("get_reflection_simulation_enabled"), &SteamAudioListener::get_reflection_simulation_enabled);
	ClassDB::bind_method(D_METHOD("set_reflection_simulation_enabled", "p_enabled"), &SteamAudioListener::set_reflection_simulation_enabled);
	ClassDB::bind_method(D_METHOD("get_refl_duration"), &SteamAudioListener::get_refl_duration);
	ClassDB::bind_method(D_METHOD("set_refl_duration", "p_refl_duration"), &SteamAudioListener::set_refl_duration);
	ClassDB::bind_method(D_METHOD("get_irradiance_min_dist"), &SteamAudioListener::get_irradiance_min_dist);
	ClassDB::bind_method(D_METHOD("set_irradiance_min_dist", "p_irradiance_min_dist"), &SteamAudioListener::set_irradiance_min_dist);
	ClassDB::bind_method(D_METHOD("get_num_refl_rays"), &SteamAudioListener::get_num_refl_rays);
	ClassDB::bind_method(D_METHOD("set_num_refl_rays", "p_num_refl_rays"), &SteamAudioListener::set_num_refl_rays);
	ClassDB::bind_method(D_METHOD("get_num_refl_bounces"), &SteamAudioListener::get_num_refl_bounces);
	ClassDB::bind_method(D_METHOD("set_num_refl_bounces", "p_num_refl_bounces"), &SteamAudioListener::set_num_refl_bounces);
	ClassDB::bind_method(D_METHOD("get_refl_ambisonics_order"), &SteamAudioListener::get_refl_ambisonics_order);
	ClassDB::bind_method(D_METHOD("set_refl_ambisonics_order", "p_refl_ambisonics_order"), &SteamAudioListener::set_refl_ambisonics_order);
	ClassDB::bind_method(D_METHOD("get_refl_type"), &SteamAudioListener::get_refl_type);
	ClassDB::bind_method(D_METHOD("set_refl_type", "p_refl_type"), &SteamAudioListener::set_refl_type);
	ClassDB::bind_method(D_METHOD("get_mask"), &SteamAudioListener::get_mask);
	ClassDB::bind_method(D_METHOD("set_mask", "mask"), &SteamAudioListener::set_mask);
	ClassDB::bind_method(D_METHOD("get_range"), &SteamAudioListener::get_range);
	ClassDB::bind_method(D_METHOD("set_range", "range"), &SteamAudioListener::set_range);

	ADD_GROUP("Reflection Simulation", "reflection_simulation_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reflection_simulation_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_reflection_simulation_enabled", "get_reflection_simulation_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reflection_simulation_duration", PROPERTY_HINT_RANGE, "0.1,10.0,0.1"), "set_refl_duration", "get_refl_duration");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reflection_simulation_irradiance_min_distance", PROPERTY_HINT_RANGE, "0.1,5.0,0.1"), "set_irradiance_min_dist", "get_irradiance_min_dist");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflection_simulation_rays", PROPERTY_HINT_RANGE, "1,8192,1"), "set_num_refl_rays", "get_num_refl_rays");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflection_simulation_bounces", PROPERTY_HINT_RANGE, "1,64,1"), "set_num_refl_bounces", "get_num_refl_bounces");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflection_simulation_ambisonics_order", PROPERTY_HINT_RANGE, "0,5,1"), "set_refl_ambisonics_order", "get_refl_ambisonics_order");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reflection_simulation_type", PROPERTY_HINT_ENUM, "Convolution,Parametric,Hybrid"), "set_refl_type", "get_refl_type");

	ADD_PROPERTY(PropertyInfo(Variant::INT, "mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_mask", "get_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "range", PROPERTY_HINT_RANGE, "0.0,10000.0,0.1,or_greater"), "set_range", "get_range");

	ClassDB::bind_method(D_METHOD("get_num_source_db_sensor_slots"), &SteamAudioListener::get_num_source_db_sensor_slots);
	ClassDB::bind_method(D_METHOD("set_num_source_db_sensor_slots", "p_num_source_db_sensor_slots"), &SteamAudioListener::set_num_source_db_sensor_slots);
	ClassDB::bind_method(D_METHOD("get_sensor_slot", "p_sensor_slot"), &SteamAudioListener::get_sensor_slot);
	ClassDB::bind_method(D_METHOD("get_sensor_slots"), &SteamAudioListener::get_sensor_slots);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "num_source_db_sensor_slots"), "set_num_source_db_sensor_slots", "get_num_source_db_sensor_slots");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "sensor_slots", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_sensor_slots");
}

void SteamAudioListener::push_config() {
	if (!rid.is_valid())
		return;
	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (!srv)
		return;
	srv->listener_set_mask(rid, mask);
	srv->listener_set_range(rid, range);
	srv->listener_set_sensor(rid, !sensor_slots.empty());
	srv->listener_set_reflection(rid, reflection_simulation_enabled, num_refl_rays, num_refl_bounces, refl_duration, refl_ambisonics_order, refl_type, irradiance_min_dist);
}

void SteamAudioListener::ready_internal() {
	generator.instantiate();
	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (!srv)
		return;
	rid = srv->listener_create();
	srv->listener_set_debug_name(rid, get_name());
	push_config();
	srv->listener_set_transform(rid, get_global_transform());
}

void SteamAudioListener::update_sensor_slots(float delta) {
	int num_slots = (int)sensor_slots.size();
	if (num_slots == 0)
		return;
	if (!rid.is_valid())
		return;
	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (!srv)
		return;

	// decay dB levels
	for (auto &slot : sensor_slots) {
		float current_db = slot->get_db_level();
		current_db -= delta * 30.0f; // decay by 30 dB per second
		if (current_db < -60.0f) {
			slot->set_db_level(-500.0f);
			slot->set_source_rid(RID());
		} else {
			slot->set_db_level(current_db);
		}
	}

	const LocalVector<ListenerSourceDBLevel> &levels = srv->listener_get_source_db_levels_ref(rid);
	for (const auto &lvl : levels) {
		float final_db_level = lvl.db_level;
		if (final_db_level < -60.0f)
			continue;

		// first check if the source is already in a slot
		int existing_slot_index = -1;
		for (int slot_index = 0; slot_index < num_slots; ++slot_index) {
			if (sensor_slots[slot_index]->get_source_rid() == lvl.source) {
				existing_slot_index = slot_index;
				break;
			}
		}

		if (existing_slot_index != -1) {
			// source already exists, update it if the new level is higher
			if (final_db_level > sensor_slots[existing_slot_index]->get_db_level()) {
				sensor_slots[existing_slot_index]->set_db_level(final_db_level);
				sensor_slots[existing_slot_index]->set_position(lvl.position);

				// check if it needs to move up (becoming louder)
				int current_slot = existing_slot_index;
				while (current_slot > 0 && sensor_slots[current_slot]->get_db_level() > sensor_slots[current_slot - 1]->get_db_level()) {
					RID prev_source = sensor_slots[current_slot - 1]->get_source_rid();
					Vector3 prev_position = sensor_slots[current_slot - 1]->get_position();
					float prev_db = sensor_slots[current_slot - 1]->get_db_level();

					sensor_slots[current_slot - 1]->set_source_rid(sensor_slots[current_slot]->get_source_rid());
					sensor_slots[current_slot - 1]->set_position(sensor_slots[current_slot]->get_position());
					sensor_slots[current_slot - 1]->set_db_level(sensor_slots[current_slot]->get_db_level());

					sensor_slots[current_slot]->set_source_rid(prev_source);
					sensor_slots[current_slot]->set_position(prev_position);
					sensor_slots[current_slot]->set_db_level(prev_db);

					current_slot--;
				}
			}
		} else {
			for (int slot_index = 0; slot_index < num_slots; ++slot_index) {
				if (final_db_level > sensor_slots[slot_index]->get_db_level()) {
					// shift down remaining
					for (int shift_index = num_slots - 1; shift_index > slot_index; --shift_index) {
						sensor_slots[shift_index]->set_source_rid(sensor_slots[shift_index - 1]->get_source_rid());
						sensor_slots[shift_index]->set_position(sensor_slots[shift_index - 1]->get_position());
						sensor_slots[shift_index]->set_db_level(sensor_slots[shift_index - 1]->get_db_level());
					}
					sensor_slots[slot_index]->set_source_rid(lvl.source);
					sensor_slots[slot_index]->set_position(lvl.position);
					sensor_slots[slot_index]->set_db_level(final_db_level);
					break;
				}
			}
		}
	}
}

void SteamAudioListener::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
			if (Engine::get_singleton()->is_editor_hint())
				return;
			set_notify_transform(true);
			set_process(true);
			ready_internal();
			break;
		case NOTIFICATION_EXIT_TREE:
			if (Engine::get_singleton()->is_editor_hint())
				return;
			set_notify_transform(false);
			if (rid.is_valid()) {
				SteamAudioServer *srv = SteamAudioServer::get_singleton();
				if (srv)
					srv->listener_free(rid);
				rid = RID();
			}
			break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			if (Engine::get_singleton()->is_editor_hint() || !rid.is_valid())
				return;
			SteamAudioServer *srv = SteamAudioServer::get_singleton();
			if (srv)
				srv->listener_set_transform(rid, get_global_transform());
			break;
		}
		case NOTIFICATION_PROCESS: {
			if (Engine::get_singleton()->is_editor_hint())
				return;
			update_sensor_slots(get_process_delta_time());
			break;
		}
		case NOTIFICATION_READY: {
			if (Engine::get_singleton()->is_editor_hint())
				return;
			if (get_child_count() > 0) {
				Node *child = get_child(0);
				if (child && (child->is_class("AudioStreamPlayer") || child->is_class("AudioStreamPlayer3D") || child->is_class("AudioStreamPlayer2D") || child->is_class("SteamAudioSource"))) {
					play_on_audiostreamplayer(child);
				}
			}
		} break;
	}
}

Ref<AudioStreamSteamAudioListenerPlayback> SteamAudioListener::play_on_audiostreamplayer(Variant audiostreamplayer) {
	Ref<AudioStreamSteamAudioListenerPlayback> playback;
	if (!generator.is_valid()) {
		ERR_PRINT("SteamAudioListener: generator is invalid, the SteamAudioListener has to be added to the tree before calling play_on_audiostreamplayer.");
		return playback;
	}
	if (audiostreamplayer.get_type() != Variant::OBJECT || (!audiostreamplayer.has_method("set_stream") && !audiostreamplayer.has_method("play_stream"))) {
		ERR_PRINT("SteamAudioListener: audiostreamplayer is not an AudioStreamPlayer/2D/3D or SteamAudioSource.");
		return playback;
	}
	if (audiostreamplayer.has_method("set_stream")) {
		audiostreamplayer.call("set_stream", generator);
		audiostreamplayer.call("play");
		playback = audiostreamplayer.call("get_stream_playback");
	}
	else if (audiostreamplayer.has_method("play_stream")) {
		playback = audiostreamplayer.call("play_stream", generator);
	}
	if (playback.is_valid()) {
		// the output buffer should just be 2 times the frame size (so it can essentially
		// fit 2 rounds of steam simulation mixed audio)
		playback->set_buffer_size(SteamAudioServer::get_singleton()->get_frame_size() * 2);
		SteamAudioServer::get_singleton()->listener_add_playback(rid, playback);
	}
	else {
		ERR_PRINT("SteamAudioListener: play_on_audiostreamplayer failed.");
	}
	return playback;
}

SteamAudioListener::SteamAudioListener() {}
SteamAudioListener::~SteamAudioListener() {}

int SteamAudioListener::get_num_refl_rays() { return num_refl_rays; }
void SteamAudioListener::set_num_refl_rays(int p_num_refl_rays) { num_refl_rays = p_num_refl_rays; push_config(); }
int SteamAudioListener::get_num_refl_bounces() { return num_refl_bounces; }
void SteamAudioListener::set_num_refl_bounces(int p_num_refl_bounces) { num_refl_bounces = p_num_refl_bounces; push_config(); }
int SteamAudioListener::get_refl_ambisonics_order() { return refl_ambisonics_order; }
void SteamAudioListener::set_refl_ambisonics_order(int p_refl_ambisonics_order) { refl_ambisonics_order = p_refl_ambisonics_order; push_config(); }
float SteamAudioListener::get_refl_duration() { return refl_duration; }
void SteamAudioListener::set_refl_duration(float p_refl_duration) { refl_duration = p_refl_duration; push_config(); }
float SteamAudioListener::get_irradiance_min_dist() { return irradiance_min_dist; }
void SteamAudioListener::set_irradiance_min_dist(float p_irradiance_min_dist) { irradiance_min_dist = p_irradiance_min_dist; push_config(); }
int SteamAudioListener::get_refl_type() { return refl_type; }
void SteamAudioListener::set_refl_type(int p_refl_type) { refl_type = p_refl_type; push_config(); }
void SteamAudioListener::set_mask(uint32_t p_mask) { mask = p_mask; push_config(); }
void SteamAudioListener::set_range(float p_range) { range = p_range; push_config(); }
void SteamAudioListener::set_reflection_simulation_enabled(bool p_enabled) { reflection_simulation_enabled = p_enabled; push_config(); }

void SteamAudioListener::set_num_source_db_sensor_slots(int p_num_source_db_sensor_slots) {
	int current_size = sensor_slots.size();
	if (p_num_source_db_sensor_slots == current_size)
		return;

	if (p_num_source_db_sensor_slots < current_size) {
		sensor_slots.resize(p_num_source_db_sensor_slots);
	} else {
		for (int i = current_size; i < p_num_source_db_sensor_slots; i++) {
			Ref<SteamAudioListenerSensorSlot> slot;
			slot.instantiate();
			sensor_slots.push_back(slot);
		}
	}

	// Keep the server's sensor flag in sync so sources get mixed (and their dB level
	// kept fresh) for this listener even without a nearby output listener. If the rid
	// doesn't exist yet (slots set before the node entered the tree), push_config()
	// applies the flag once ready_internal() creates it.
	if (rid.is_valid()) {
		SteamAudioServer *srv = SteamAudioServer::get_singleton();
		if (srv)
			srv->listener_set_sensor(rid, !sensor_slots.empty());
	}
}

int SteamAudioListener::get_num_source_db_sensor_slots() {
	return sensor_slots.size();
}

Ref<SteamAudioListenerSensorSlot> SteamAudioListener::get_sensor_slot(int p_sensor_slot) {
	if (p_sensor_slot < 0 || p_sensor_slot >= (int)sensor_slots.size()) {
		return Ref<SteamAudioListenerSensorSlot>();
	}
	return sensor_slots[p_sensor_slot];
}

Array SteamAudioListener::get_sensor_slots() const {
	Array arr;
	for (const auto &slot : sensor_slots) {
		arr.push_back(slot);
	}
	return arr;
}

PackedStringArray SteamAudioListener::_get_configuration_warnings() const {
	PackedStringArray res;
	return res;
}
