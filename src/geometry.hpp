#ifndef STEAM_AUDIO_GEOMETRY_H
#define STEAM_AUDIO_GEOMETRY_H

#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/templates/hash_map.hpp"
#include "godot_cpp/variant/rid.hpp"
#include "material.hpp"
#include <vector>

class SteamAudioGeometry : public godot::Node3D {
	GDCLASS(SteamAudioGeometry, godot::Node3D);

private:
	godot::Dictionary materials; // Group Name (String) -> SteamAudioMaterial (Resource)
	godot::Ref<SteamAudioMaterial> default_material;
	bool is_dynamic = false;
	bool batch_geometry = true;
	bool auto_register = true;
	godot::NodePath root_path;
	godot::RID frame_rid;
	uint64_t frame_root_id = 0;

	// All geometry RIDs created by this node (for cleanup).
	std::vector<godot::RID> geometry_rids;
	// Dynamic geometry: the moving node + its server RID, so we can push the
	// transform when it changes.
	struct DynamicEntry {
		uint64_t node_id = 0;
		godot::RID rid;
		godot::Transform3D last_trf;
		bool has_last_trf = false;
	};
	std::vector<DynamicEntry> dynamic_entries;
	struct GeometryEntry {
		godot::RID rid;
		godot::String group;
		int material_index = 0;
		bool is_default = false;
	};
	std::vector<GeometryEntry> geometry_entries;

	struct BatchedAccum {
		godot::PackedVector3Array verts;
		godot::PackedInt32Array tris;
		godot::PackedInt32Array mat_indices;
		godot::PackedFloat32Array materials_flat;
		godot::HashMap<godot::String, int> group_to_mat_index;
		int default_mat_index = -1;
	};

	godot::Node3D *get_frame_root() const;
	godot::Transform3D get_frame_inv() const;
	void collect_for_batch(godot::Node *p_node, const godot::Transform3D &p_base_inv, BatchedAccum &r_accum);
	void find_and_register_per_node(godot::Node *p_node, const godot::Transform3D &p_frame_inv);
	void unregister_all();

protected:
	static void _bind_methods();

public:
	SteamAudioGeometry();
	~SteamAudioGeometry();

	void _notification(int p_what);

	void set_materials(const godot::Dictionary &p_materials);
	godot::Dictionary get_materials() const;

	void set_default_material(const godot::Ref<SteamAudioMaterial> &p_material);
	godot::Ref<SteamAudioMaterial> get_default_material() const;

	void set_is_dynamic(bool p_dynamic);
	bool get_is_dynamic() const;

	void set_batch_geometry(bool p_batch);
	bool get_batch_geometry() const;

	void set_auto_register(bool p_auto);
	bool get_auto_register() const;

	void set_root_path(const godot::NodePath &p_path);
	godot::NodePath get_root_path() const;

	void set_frame(godot::RID p_frame, godot::Node3D *p_frame_root = nullptr);
	godot::RID get_frame() const;

	void register_geometry();
	void unregister_geometry();
	void update_materials();
};

#endif // STEAM_AUDIO_GEOMETRY_H
