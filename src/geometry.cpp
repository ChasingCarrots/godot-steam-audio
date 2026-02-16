#include "geometry.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/collision_shape3d.hpp"
#include "server.hpp"

using namespace godot;

void SteamAudioGeometry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_materials"), &SteamAudioGeometry::get_materials);
	ClassDB::bind_method(D_METHOD("set_materials", "materials"), &SteamAudioGeometry::set_materials);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "materials", PROPERTY_HINT_DICTIONARY_TYPE, "String;SteamAudioMaterial"), "set_materials", "get_materials");

	ClassDB::bind_method(D_METHOD("get_is_dynamic"), &SteamAudioGeometry::get_is_dynamic);
	ClassDB::bind_method(D_METHOD("set_is_dynamic", "dynamic"), &SteamAudioGeometry::set_is_dynamic);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "is_dynamic"), "set_is_dynamic", "get_is_dynamic");

	ClassDB::bind_method(D_METHOD("get_root_path"), &SteamAudioGeometry::get_root_path);
	ClassDB::bind_method(D_METHOD("set_root_path", "path"), &SteamAudioGeometry::set_root_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "root_path"), "set_root_path", "get_root_path");
}

SteamAudioGeometry::SteamAudioGeometry() {}
SteamAudioGeometry::~SteamAudioGeometry() {}

void SteamAudioGeometry::_notification(int p_what) {
	if (Engine::get_singleton()->is_editor_hint()) return;

	switch (p_what) {
		case NOTIFICATION_READY: {
			Node *root = get_node_or_null(root_path);
			if (!root) root = this;
			find_and_register_geometry(root);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			Node *root = get_node_or_null(root_path);
			if (!root) root = this;
			find_and_unregister_geometry(root);
		} break;
	}
}

void SteamAudioGeometry::find_and_register_geometry(Node *p_node) {
	if (!p_node) return;

	Array groups = p_node->get_groups();
	for (int i = 0; i < groups.size(); ++i) {
		String group = groups[i];
		if (materials.has(group)) {
			Ref<SteamAudioMaterial> mat = materials[group];
			if (is_dynamic) {
				SteamAudioServer::get_singleton()->add_dynamic_geometry(p_node, mat);
			} else {
				SteamAudioServer::get_singleton()->add_static_geometry(p_node, mat);
			}
			// One node can only be one geometry object for now to keep it simple.
			break;
		}
	}

	for (int i = 0; i < p_node->get_child_count(); ++i) {
		find_and_register_geometry(p_node->get_child(i));
	}
}

void SteamAudioGeometry::find_and_unregister_geometry(Node *p_node) {
	if (!p_node) return;

	if (is_dynamic) {
		SteamAudioServer::get_singleton()->remove_dynamic_geometry(p_node);
	}
	else {
		SteamAudioServer::get_singleton()->remove_static_geometry(p_node);
	}

	for (int i = 0; i < p_node->get_child_count(); ++i) {
		find_and_unregister_geometry(p_node->get_child(i));
	}
}

void SteamAudioGeometry::set_materials(const Dictionary &p_materials) { materials = p_materials; }
Dictionary SteamAudioGeometry::get_materials() const { return materials; }

void SteamAudioGeometry::set_is_dynamic(bool p_dynamic) { is_dynamic = p_dynamic; }
bool SteamAudioGeometry::get_is_dynamic() const { return is_dynamic; }

void SteamAudioGeometry::set_root_path(const NodePath &p_path) { root_path = p_path; }
NodePath SteamAudioGeometry::get_root_path() const { return root_path; }
