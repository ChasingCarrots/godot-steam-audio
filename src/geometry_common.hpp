#ifndef STEAM_AUDIO_GEOMETRY_COMMON_H
#define STEAM_AUDIO_GEOMETRY_COMMON_H

#include "godot_cpp/classes/array_mesh.hpp"
#include "godot_cpp/classes/box_mesh.hpp"
#include "godot_cpp/classes/box_shape3d.hpp"
#include "godot_cpp/classes/capsule_mesh.hpp"
#include "godot_cpp/classes/capsule_shape3d.hpp"
#include "godot_cpp/classes/collision_shape3d.hpp"
#include "godot_cpp/classes/concave_polygon_shape3d.hpp"
#include "godot_cpp/classes/convex_polygon_shape3d.hpp"
#include "godot_cpp/classes/cylinder_mesh.hpp"
#include "godot_cpp/classes/cylinder_shape3d.hpp"
#include "godot_cpp/classes/mesh.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/sphere_mesh.hpp"
#include "godot_cpp/classes/sphere_shape3d.hpp"
#include "godot_cpp/variant/packed_int32_array.hpp"
#include "godot_cpp/variant/packed_vector3_array.hpp"
#include "steam_audio.hpp"

// Raw triangle payload extracted from a Godot node, ready to be handed to
// SteamAudioServer::geometry_create_*(). Vertices are pre-transformed (unless
// the caller asked to ignore the transform, e.g. for dynamic geometry where the
// transform is pushed separately); triangles use Godot CW winding (the server
// flips them to IPL CCW).
struct RawGeometry {
	godot::PackedVector3Array verts;
	godot::PackedInt32Array tris;

	bool is_empty() const { return verts.is_empty() || tris.is_empty(); }
};

// Append all surfaces of a Godot Mesh into the raw payload, applying trf to the
// vertices and offsetting indices so multiple surfaces can share one buffer.
inline void append_mesh_surfaces(RawGeometry &out, godot::Ref<godot::Mesh> mesh, const godot::Transform3D &trf) {
	if (mesh.is_null())
		return;
	for (int s = 0; s < mesh->get_surface_count(); s++) {
		godot::Array dat = mesh->surface_get_arrays(s);
		godot::PackedVector3Array verts = dat[godot::Mesh::ARRAY_VERTEX];
		godot::PackedInt32Array idx = dat[godot::Mesh::ARRAY_INDEX];

		int base = (int)out.verts.size();
		for (int i = 0; i < verts.size(); i++) {
			out.verts.push_back(trf.xform(verts[i]));
		}

		if (idx.size() == 0) {
			// No index array: vertices form sequential triangles.
			for (int i = 0; i < verts.size(); i++) {
				out.tris.push_back(base + i);
			}
		} else {
			for (int i = 0; i < idx.size(); i++) {
				out.tris.push_back(base + idx[i]);
			}
		}
	}
}

inline RawGeometry extract_mesh_inst_3d(godot::MeshInstance3D *mesh_inst, bool ignore_trf) {
	RawGeometry out;
	godot::Ref<godot::Mesh> mesh = mesh_inst->get_mesh();
	godot::Transform3D trf = ignore_trf ? godot::Transform3D() : mesh_inst->get_global_transform();
	append_mesh_surfaces(out, mesh, trf);
	return out;
}

inline RawGeometry extract_coll_inst_3d(godot::CollisionShape3D *coll_inst, bool ignore_trf) {
	RawGeometry out;
	godot::Transform3D trf = ignore_trf ? godot::Transform3D() : coll_inst->get_global_transform();

	godot::Ref<godot::Mesh> mesh;

	if (godot::Object::cast_to<godot::BoxShape3D>(coll_inst->get_shape().ptr())) {
		godot::Ref<godot::BoxShape3D> shape = coll_inst->get_shape();
		godot::Ref<godot::BoxMesh> box_mesh;
		box_mesh.instantiate();
		box_mesh->set_size(shape->get_size());
		mesh = box_mesh;
	} else if (godot::Object::cast_to<godot::CylinderShape3D>(coll_inst->get_shape().ptr())) {
		godot::Ref<godot::CylinderShape3D> shape = coll_inst->get_shape();
		godot::Ref<godot::CylinderMesh> cyl_mesh;
		cyl_mesh.instantiate();
		cyl_mesh->set_radial_segments(4);
		cyl_mesh->set_rings(4);
		cyl_mesh->set_bottom_radius(shape->get_radius());
		cyl_mesh->set_top_radius(shape->get_radius());
		cyl_mesh->set_height(shape->get_height());
		mesh = cyl_mesh;
	} else if (godot::Object::cast_to<godot::CapsuleShape3D>(coll_inst->get_shape().ptr())) {
		godot::Ref<godot::CapsuleShape3D> shape = coll_inst->get_shape();
		godot::Ref<godot::CapsuleMesh> cap_mesh;
		cap_mesh.instantiate();
		cap_mesh->set_radial_segments(4);
		cap_mesh->set_rings(4);
		cap_mesh->set_radius(shape->get_radius());
		cap_mesh->set_height(shape->get_height());
		mesh = cap_mesh;
	} else if (godot::Object::cast_to<godot::SphereShape3D>(coll_inst->get_shape().ptr())) {
		godot::Ref<godot::SphereShape3D> shape = coll_inst->get_shape();
		godot::Ref<godot::SphereMesh> sph_mesh;
		sph_mesh.instantiate();
		sph_mesh->set_radial_segments(4);
		sph_mesh->set_rings(4);
		sph_mesh->set_radius(shape->get_radius());
		sph_mesh->set_height(shape->get_radius() * 2);
		mesh = sph_mesh;
	} else if (godot::Object::cast_to<godot::ConcavePolygonShape3D>(coll_inst->get_shape().ptr())) {
		godot::Ref<godot::ConcavePolygonShape3D> concave = coll_inst->get_shape();
		godot::Ref<godot::ArrayMesh> arr_mesh;
		arr_mesh.instantiate();
		auto faces = concave->get_faces();

		godot::PackedInt32Array tris;
		tris.resize(faces.size());
		for (int i = 0; i < faces.size(); i++) {
			tris[i] = i;
		}

		godot::Array surface_array;
		surface_array.resize(godot::Mesh::ArrayType::ARRAY_MAX);
		surface_array[godot::Mesh::ArrayType::ARRAY_VERTEX] = faces;
		surface_array[godot::Mesh::ArrayType::ARRAY_INDEX] = tris;

		arr_mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, surface_array);
		mesh = arr_mesh;
	} else if (godot::Object::cast_to<godot::ConvexPolygonShape3D>(coll_inst->get_shape().ptr())) {
		godot::Ref<godot::Mesh> debug_mesh = coll_inst->get_shape()->get_debug_mesh();
		godot::Ref<godot::ArrayMesh> arr_mesh;
		arr_mesh.instantiate();

		for (int i = 0; i < debug_mesh->get_surface_count(); i++) {
			godot::Array surface_data = debug_mesh->surface_get_arrays(i);
			godot::PackedVector3Array vertices = surface_data[godot::Mesh::ARRAY_VERTEX];
			godot::Array idx_array = surface_data[godot::Mesh::ARRAY_INDEX];

			if (idx_array.size() == 0 && vertices.size() >= 3) {
				godot::PackedInt32Array tris;
				tris.resize(vertices.size());
				for (int j = 0; j < vertices.size(); j++) {
					tris[j] = j;
				}

				godot::Array surface_array;
				surface_array.resize(godot::Mesh::ArrayType::ARRAY_MAX);
				surface_array[godot::Mesh::ArrayType::ARRAY_VERTEX] = vertices;
				surface_array[godot::Mesh::ArrayType::ARRAY_INDEX] = tris;
				arr_mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, surface_array);
			} else {
				arr_mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, surface_data);
			}
		}
		mesh = arr_mesh;
	} else {
		SteamAudio::log(SteamAudio::log_error, "SteamAudioGeometry supports sphere, box, cylinder, capsule, concave polygon and convex polygon shapes. Something else was provided, so this geometry will not do anything.");
		return out;
	}

	append_mesh_surfaces(out, mesh, trf);
	return out;
}

#endif // STEAM_AUDIO_GEOMETRY_COMMON_H
