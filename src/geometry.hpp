#ifndef STEAM_AUDIO_GEOMETRY_H
#define STEAM_AUDIO_GEOMETRY_H

#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/templates/hash_map.hpp"
#include "material.hpp"

class SteamAudioGeometry : public godot::Node3D {
	GDCLASS(SteamAudioGeometry, godot::Node3D);

private:
	godot::Dictionary materials; // Group Name (String) -> SteamAudioMaterial (Resource)
	bool is_dynamic = false;
	godot::NodePath root_path;

	void find_and_register_geometry(godot::Node *p_node);
	void find_and_unregister_geometry(godot::Node *p_node);

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
