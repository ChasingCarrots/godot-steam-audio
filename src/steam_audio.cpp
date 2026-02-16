#include "steam_audio.hpp"
#include "godot_cpp/classes/project_settings.hpp"
#include "godot_cpp/variant/utility_functions.hpp"

using namespace godot;

static int get_log_level_setting() {
	Variant v = ProjectSettings::get_singleton()->get_setting("steamaudio/log_level");
	if (v.get_type() == Variant::NIL) return SteamAudio::log_info;
	return int(v);
}

void SteamAudio::log(GodotSteamAudioLogLevel lvl, const char *str) {
	if (lvl < get_log_level_setting()) {
		return;
	}

	switch (lvl) {
		case log_error:
			UtilityFunctions::push_error("[godot-steam-audio] ", str);
			return;
		case log_warn:
			UtilityFunctions::push_warning("[godot-steam-audio] ", str);
			return;
		default:
			UtilityFunctions::print("[godot-steam-audio] ", str);
			return;
	}
}
