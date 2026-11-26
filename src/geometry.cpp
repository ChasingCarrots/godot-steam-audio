#include "geometry.hpp"
#include "geometry_common.hpp"
#include "godot_cpp/classes/area3d.hpp"
#include "godot_cpp/classes/collision_shape3d.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/core/object.hpp"
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

static void append_material_floats(PackedFloat32Array &dst, Ref<SteamAudioMaterial> mat) {
	PackedFloat32Array f = material_to_floats(mat);
	for (int i = 0; i < 7; ++i) {
		dst.push_back(f[i]);
	}
}

void SteamAudioGeometry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_materials"), &SteamAudioGeometry::get_materials);
	ClassDB::bind_method(D_METHOD("set_materials", "materials"), &SteamAudioGeometry::set_materials);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "materials", PROPERTY_HINT_DICTIONARY_TYPE, "String;SteamAudioMaterial"), "set_materials", "get_materials");

	ClassDB::bind_method(D_METHOD("get_default_material"), &SteamAudioGeometry::get_default_material);
	ClassDB::bind_method(D_METHOD("set_default_material", "material"), &SteamAudioGeometry::set_default_material);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "default_material", PROPERTY_HINT_RESOURCE_TYPE, "SteamAudioMaterial"), "set_default_material", "get_default_material");

	ClassDB::bind_method(D_METHOD("get_is_dynamic"), &SteamAudioGeometry::get_is_dynamic);
	ClassDB::bind_method(D_METHOD("set_is_dynamic", "dynamic"), &SteamAudioGeometry::set_is_dynamic);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "is_dynamic"), "set_is_dynamic", "get_is_dynamic");

	ClassDB::bind_method(D_METHOD("get_batch_geometry"), &SteamAudioGeometry::get_batch_geometry);
	ClassDB::bind_method(D_METHOD("set_batch_geometry", "batch"), &SteamAudioGeometry::set_batch_geometry);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "batch_geometry"), "set_batch_geometry", "get_batch_geometry");

	ClassDB::bind_method(D_METHOD("get_auto_register"), &SteamAudioGeometry::get_auto_register);
	ClassDB::bind_method(D_METHOD("set_auto_register", "auto_register"), &SteamAudioGeometry::set_auto_register);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_register"), "set_auto_register", "get_auto_register");

	ClassDB::bind_method(D_METHOD("get_root_path"), &SteamAudioGeometry::get_root_path);
	ClassDB::bind_method(D_METHOD("set_root_path", "path"), &SteamAudioGeometry::set_root_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "root_path"), "set_root_path", "get_root_path");

	ClassDB::bind_method(D_METHOD("set_frame", "frame", "frame_root"), &SteamAudioGeometry::set_frame, DEFVAL(nullptr));
	ClassDB::bind_method(D_METHOD("get_frame"), &SteamAudioGeometry::get_frame);
	ClassDB::bind_method(D_METHOD("register_geometry"), &SteamAudioGeometry::register_geometry);
	ClassDB::bind_method(D_METHOD("unregister_geometry"), &SteamAudioGeometry::unregister_geometry);
	ClassDB::bind_method(D_METHOD("update_materials"), &SteamAudioGeometry::update_materials);
}

SteamAudioGeometry::SteamAudioGeometry() {}
SteamAudioGeometry::~SteamAudioGeometry() {}

Node3D *SteamAudioGeometry::get_frame_root() const {
	if (frame_root_id == 0)
		return nullptr;
	Object *obj = ObjectDB::get_instance(frame_root_id);
	Node3D *n3d = Object::cast_to<Node3D>(obj);
	if (n3d && n3d->is_inside_tree())
		return n3d;
	return nullptr;
}

Transform3D SteamAudioGeometry::get_frame_inv() const {
	Node3D *root = get_frame_root();
	if (root)
		return root->get_global_transform().affine_inverse();
	return Transform3D();
}

void SteamAudioGeometry::_notification(int p_what) {
	if (Engine::get_singleton()->is_editor_hint())
		return;

	switch (p_what) {
		case NOTIFICATION_READY: {
			if (auto_register) {
				register_geometry();
			}
		} break;
		case NOTIFICATION_PROCESS: {
			SteamAudioServer *srv = SteamAudioServer::get_singleton();
			if (!srv || dynamic_entries.empty())
				break;
			Transform3D frame_inv = get_frame_inv();
			for (auto &e : dynamic_entries) {
				Object *obj = ObjectDB::get_instance(e.node_id);
				Node3D *n3d = Object::cast_to<Node3D>(obj);
				if (!n3d || !n3d->is_inside_tree())
					continue;
				Transform3D trf = frame_inv * n3d->get_global_transform();
				if (!e.has_last_trf || !trf.is_equal_approx(e.last_trf)) {
					srv->geometry_set_transform(e.rid, trf);
					e.last_trf = trf;
					e.has_last_trf = true;
				}
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			unregister_all();
		} break;
	}
}

void SteamAudioGeometry::register_geometry() {
	unregister_all();
	if (Engine::get_singleton()->is_editor_hint() || !is_inside_tree())
		return;

	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (!srv)
		return;

	Node *root = get_node_or_null(root_path);
	if (!root)
		root = this;

	Transform3D frame_inv = get_frame_inv();

	if (batch_geometry) {
		Node3D *root_n3d = Object::cast_to<Node3D>(root);
		Transform3D base_inv = (is_dynamic && root_n3d) ? root_n3d->get_global_transform().affine_inverse() : frame_inv;

		BatchedAccum accum;
		collect_for_batch(root, base_inv, accum);

		if (!accum.verts.is_empty() && accum.tris.size() >= 3) {
			RID rid = srv->geometry_create_batched(accum.verts, accum.tris, accum.mat_indices, accum.materials_flat, is_dynamic, frame_rid);
			if (rid.is_valid()) {
				geometry_rids.push_back(rid);
				for (const KeyValue<String, int> &kv : accum.group_to_mat_index) {
					geometry_entries.push_back({ rid, kv.key, kv.value, false });
				}
				if (accum.default_mat_index >= 0) {
					geometry_entries.push_back({ rid, String(), accum.default_mat_index, true });
				}
				if (is_dynamic && root_n3d) {
					Transform3D initial_trf = frame_inv * root_n3d->get_global_transform();
					srv->geometry_set_transform(rid, initial_trf);
					dynamic_entries.push_back({ root_n3d->get_instance_id(), rid, initial_trf, true });
				}
			}
		}
	} else {
		find_and_register_per_node(root, frame_inv);
	}

	set_process(!dynamic_entries.empty());
}

void SteamAudioGeometry::unregister_geometry() {
	unregister_all();
	set_process(false);
}

static bool match_node_material(Node *p_node, const Dictionary &p_materials, const Ref<SteamAudioMaterial> &p_default_material, String &r_group, Ref<SteamAudioMaterial> &r_mat, bool &r_is_default) {
	Array groups = p_node->get_groups();
	for (int i = 0; i < groups.size(); ++i) {
		String group = groups[i];
		if (p_materials.has(group)) {
			r_group = group;
			r_mat = p_materials[group];
			r_is_default = false;
			return true;
		}
	}
	if (p_default_material.is_valid()) {
		r_group = String();
		r_mat = p_default_material;
		r_is_default = true;
		return true;
	}
	return false;
}

static bool extract_node_geometry(Node *p_node, bool p_ignore_trf, RawGeometry &r_raw) {
	if (auto *mi = Object::cast_to<MeshInstance3D>(p_node)) {
		r_raw = extract_mesh_inst_3d(mi, p_ignore_trf);
		return !r_raw.is_empty();
	}
	if (auto *cs = Object::cast_to<CollisionShape3D>(p_node)) {
		if (cs->is_disabled() || Object::cast_to<Area3D>(cs->get_parent()) != nullptr) {
			return false;
		}
		r_raw = extract_coll_inst_3d(cs, p_ignore_trf);
		return !r_raw.is_empty();
	}
	return false;
}

void SteamAudioGeometry::collect_for_batch(Node *p_node, const Transform3D &p_base_inv, BatchedAccum &r_accum) {
	if (!p_node)
		return;

	String matched_group;
	Ref<SteamAudioMaterial> matched_mat;
	bool is_default = false;
	if (match_node_material(p_node, materials, default_material, matched_group, matched_mat, is_default)) {
		RawGeometry raw;
		if (extract_node_geometry(p_node, true, raw)) {
			int mat_idx = -1;
			if (is_default) {
				if (r_accum.default_mat_index < 0) {
					r_accum.default_mat_index = (int)(r_accum.materials_flat.size() / 7);
					append_material_floats(r_accum.materials_flat, matched_mat);
				}
				mat_idx = r_accum.default_mat_index;
			} else if (r_accum.group_to_mat_index.has(matched_group)) {
				mat_idx = r_accum.group_to_mat_index[matched_group];
			} else {
				mat_idx = (int)(r_accum.materials_flat.size() / 7);
				r_accum.group_to_mat_index[matched_group] = mat_idx;
				append_material_floats(r_accum.materials_flat, matched_mat);
			}

			Node3D *n3d = Object::cast_to<Node3D>(p_node);
			Transform3D rel_trf = n3d ? (p_base_inv * n3d->get_global_transform()) : Transform3D();

			int base_vert = (int)r_accum.verts.size();
			for (int i = 0; i < raw.verts.size(); ++i) {
				r_accum.verts.push_back(rel_trf.xform(raw.verts[i]));
			}
			for (int i = 0; i < raw.tris.size(); ++i) {
				r_accum.tris.push_back(base_vert + raw.tris[i]);
			}
			int num_tris = (int)(raw.tris.size() / 3);
			for (int i = 0; i < num_tris; ++i) {
				r_accum.mat_indices.push_back(mat_idx);
			}
		}
	}

	for (int i = 0; i < p_node->get_child_count(); ++i) {
		collect_for_batch(p_node->get_child(i), p_base_inv, r_accum);
	}
}

void SteamAudioGeometry::find_and_register_per_node(Node *p_node, const Transform3D &p_frame_inv) {
	if (!p_node)
		return;

	SteamAudioServer *srv = SteamAudioServer::get_singleton();

	String matched_group;
	Ref<SteamAudioMaterial> matched_mat;
	bool is_default = false;
	if (match_node_material(p_node, materials, default_material, matched_group, matched_mat, is_default)) {
		RawGeometry raw;
		if (srv && extract_node_geometry(p_node, true, raw)) {
			PackedFloat32Array mat_floats = material_to_floats(matched_mat);
			PackedInt32Array mat_indices;
			mat_indices.resize(raw.tris.size() / 3);
			mat_indices.fill(0);

			Node3D *n3d = Object::cast_to<Node3D>(p_node);
			Transform3D rel_trf = n3d ? (p_frame_inv * n3d->get_global_transform()) : Transform3D();

			if (is_dynamic) {
				RID rid = srv->geometry_create_batched(raw.verts, raw.tris, mat_indices, mat_floats, true, frame_rid);
				if (rid.is_valid()) {
					geometry_rids.push_back(rid);
					geometry_entries.push_back({ rid, matched_group, 0, is_default });
					srv->geometry_set_transform(rid, rel_trf);
					if (n3d) {
						dynamic_entries.push_back({ n3d->get_instance_id(), rid, rel_trf, true });
					}
				}
			} else {
				for (int i = 0; i < raw.verts.size(); ++i) {
					raw.verts.set(i, rel_trf.xform(raw.verts[i]));
				}
				RID rid = srv->geometry_create_batched(raw.verts, raw.tris, mat_indices, mat_floats, false, frame_rid);
				if (rid.is_valid()) {
					geometry_rids.push_back(rid);
					geometry_entries.push_back({ rid, matched_group, 0, is_default });
				}
			}
		}
	}

	for (int i = 0; i < p_node->get_child_count(); ++i) {
		find_and_register_per_node(p_node->get_child(i), p_frame_inv);
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
	geometry_entries.clear();
	dynamic_entries.clear();
}

void SteamAudioGeometry::set_materials(const Dictionary &p_materials) {
	materials = p_materials;
	if (is_inside_tree()) {
		update_materials();
	}
}

void SteamAudioGeometry::update_materials() {
	SteamAudioServer *srv = SteamAudioServer::get_singleton();
	if (!srv)
		return;
	for (const auto &entry : geometry_entries) {
		if (entry.is_default) {
			if (default_material.is_valid()) {
				srv->geometry_set_material(entry.rid, material_to_floats(default_material), entry.material_index);
			}
		} else if (materials.has(entry.group)) {
			Ref<SteamAudioMaterial> mat = materials[entry.group];
			srv->geometry_set_material(entry.rid, material_to_floats(mat), entry.material_index);
		}
	}
}

Dictionary SteamAudioGeometry::get_materials() const { return materials; }

void SteamAudioGeometry::set_default_material(const Ref<SteamAudioMaterial> &p_material) {
	default_material = p_material;
	if (is_inside_tree()) {
		update_materials();
	}
}
Ref<SteamAudioMaterial> SteamAudioGeometry::get_default_material() const { return default_material; }

void SteamAudioGeometry::set_is_dynamic(bool p_dynamic) { is_dynamic = p_dynamic; }
bool SteamAudioGeometry::get_is_dynamic() const { return is_dynamic; }

void SteamAudioGeometry::set_batch_geometry(bool p_batch) { batch_geometry = p_batch; }
bool SteamAudioGeometry::get_batch_geometry() const { return batch_geometry; }

void SteamAudioGeometry::set_auto_register(bool p_auto) { auto_register = p_auto; }
bool SteamAudioGeometry::get_auto_register() const { return auto_register; }

void SteamAudioGeometry::set_root_path(const NodePath &p_path) { root_path = p_path; }
NodePath SteamAudioGeometry::get_root_path() const { return root_path; }

void SteamAudioGeometry::set_frame(RID p_frame, Node3D *p_frame_root) {
	frame_rid = p_frame;
	frame_root_id = p_frame_root ? p_frame_root->get_instance_id() : 0;
	if (is_inside_tree() && !Engine::get_singleton()->is_editor_hint() && (!geometry_rids.empty() || !auto_register)) {
		register_geometry();
	}
}

RID SteamAudioGeometry::get_frame() const { return frame_rid; }
