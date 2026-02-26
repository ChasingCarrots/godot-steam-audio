#include "register_types.hpp"

#include "godot_cpp/core/memory.hpp"
#include "geometry.hpp"
#include "listener.hpp"
#include "material.hpp"
#include "server.hpp"
#include "source.hpp"
#include "parameterized_audio_stream.h"
#include "AudioStreamSteamAudioListener.h"

#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/scene_tree.hpp"
#include "godot_cpp/variant/utility_functions.hpp"
#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

SteamAudioServer *srv;

void init_ext(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE && p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		GDREGISTER_CLASS(SteamAudioServer);
		srv = memnew(SteamAudioServer);
		Engine::get_singleton()->register_singleton("SteamAudioServer", srv);
	}

	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		ClassDB::register_class<SteamAudioListener>();
		ClassDB::register_class<SteamAudioSource>();
		ClassDB::register_class<SteamAudioMaterial>();
		ClassDB::register_class<SteamAudioGeometry>();
		ClassDB::register_abstract_class<ParameterCondition>();
		ClassDB::register_class<ParameterConditionComparison>();
		ClassDB::register_class<ParameterConditionRange>();
		ClassDB::register_abstract_class<ParameterizedOutput>();
		ClassDB::register_class<ParameterizedOutputRandomize>();
		ClassDB::register_class<ParameterizedOutputGranularLinearSweep>();
		ClassDB::register_class<ParameterizedAudioStreamInput>();
		ClassDB::register_class<AudioStreamParameterized>();
		ClassDB::register_class<AudioStreamPlaybackParameterized>();
		ClassDB::register_class<AudioStreamSteamAudioListener>();
		ClassDB::register_class<AudioStreamSteamAudioListenerPlayback>();

	}
}

void uninit_ext(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SERVERS) {
		Engine::get_singleton()->unregister_singleton("SteamAudioServer");
		memdelete(srv);
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT init_extension(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(init_ext);
	init_obj.register_terminator(uninit_ext);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
