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
	bool is_dynamic = false;
	godot::NodePath root_path;

	// All geometry RIDs created by this node (for cleanup).
	std::vector<godot::RID> geometry_rids;
	// Dynamic geometry: the moving node + its server RID, so we can push the
	// transform each frame.
	struct DynamicEntry {
		godot::Node3D *node = nullptr;
		godot::RID rid;
	};
	std::vector<DynamicEntry> dynamic_entries;

	void find_and_register_geometry(godot::Node *p_node);
	void unregister_all();

protected:
	static void _bind_methods();

public:
	SteamAudioGeometry();
	~SteamAudioGeometry();

	void _notification(int p_what);

	void set_materials(const godot::Dictionary &p_materials);
	godot::Dictionary get_materials() const;

	void set_is_dynamic(bool p_dynamic);
	bool get_is_dynamic() const;

	void set_root_path(const godot::NodePath &p_path);
	godot::NodePath get_root_path() const;
};

#endif // STEAM_AUDIO_GEOMETRY_H
