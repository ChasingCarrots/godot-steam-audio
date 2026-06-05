#include "geometry.hpp"
#include "geometry_common.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/collision_shape3d.hpp"
#include "godot_cpp/variant/packed_float32_array.hpp"
#include "server.hpp"

using namespace godot;

static PackedFloat32Array material_to_floats(Ref<SteamAudioMaterial> mat) {
	PackedFloat32Array f;
	f.resize(7);
	if (mat.is_valid()) {
		IPLMaterial m = mat->get_material();
		f[0] = m.absorption[0];
		f[1] = m.absorption[1];
		f[2] = m.absorption[2];
		f[3] = m.scattering;
		f[4] = m.transmission[0];
		f[5] = m.transmission[1];
		f[6] = m.transmission[2];
	}
	return f;
}

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
			set_process(!dynamic_entries.empty());
		} break;
		case NOTIFICATION_PROCESS: {
			SteamAudioServer *srv = SteamAudioServer::get_singleton();
			if (!srv)
				break;
			for (auto &e : dynamic_entries) {
				if (e.node)
					srv->geometry_set_transform(e.rid, e.node->get_global_transform());
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			unregister_all();
		} break;
	}
}

void SteamAudioGeometry::find_and_register_geometry(Node *p_node) {
	if (!p_node) return;

	SteamAudioServer *srv = SteamAudioServer::get_singleton();

	Array groups = p_node->get_groups();
	for (int i = 0; i < groups.size(); ++i) {
		String group = groups[i];
		if (materials.has(group)) {
			Ref<SteamAudioMaterial> mat = materials[group];
			PackedFloat32Array mat_floats = material_to_floats(mat);

			RawGeometry raw;
			if (auto *mi = Object::cast_to<MeshInstance3D>(p_node)) {
				raw = extract_mesh_inst_3d(mi, is_dynamic);
			} else if (auto *cs = Object::cast_to<CollisionShape3D>(p_node)) {
				raw = extract_coll_inst_3d(cs, is_dynamic);
			}

			if (srv && !raw.is_empty()) {
				if (is_dynamic) {
					RID rid = srv->geometry_create_dynamic(raw.verts, raw.tris, mat_floats);
					if (rid.is_valid()) {
						geometry_rids.push_back(rid);
						Node3D *n3d = Object::cast_to<Node3D>(p_node);
						srv->geometry_set_transform(rid, n3d ? n3d->get_global_transform() : Transform3D());
						dynamic_entries.push_back({ n3d, rid });
					}
				} else {
					RID rid = srv->geometry_create_static(raw.verts, raw.tris, mat_floats);
					if (rid.is_valid())
						geometry_rids.push_back(rid);
				}
			}
			// One node can only be one geometry object for now to keep it simple.
			break;
		}
	}

	for (int i = 0; i < p_node->get_child_count(); ++i) {
		find_and_register_geometry(p_node->get_child(i));
	}
}

void SteamAudioGeometry::unregister_all() {
	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (srv) {
		for (const RID &rid : geometry_rids) {
			srv->geometry_free(rid);
		}
	}
	geometry_rids.clear();
	dynamic_entries.clear();
}

void SteamAudioGeometry::set_materials(const Dictionary &p_materials) { materials = p_materials; }
Dictionary SteamAudioGeometry::get_materials() const { return materials; }

void SteamAudioGeometry::set_is_dynamic(bool p_dynamic) { is_dynamic = p_dynamic; }
bool SteamAudioGeometry::get_is_dynamic() const { return is_dynamic; }

void SteamAudioGeometry::set_root_path(const NodePath &p_path) { root_path = p_path; }
NodePath SteamAudioGeometry::get_root_path() const { return root_path; }
