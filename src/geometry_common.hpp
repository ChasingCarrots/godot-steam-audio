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
#include <cmath>
#include <vector>

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

// Build outward-wound (Godot CW) triangles for a convex hull from arbitrary 3D points.
inline void append_convex_hull(RawGeometry &out, const godot::PackedVector3Array &raw_pts, const godot::Transform3D &trf) {
	std::vector<godot::Vector3> pts;
	pts.reserve(raw_pts.size());
	for (int i = 0; i < raw_pts.size(); ++i) {
		godot::Vector3 p = raw_pts[i];
		bool dup = false;
		for (const auto &q : pts) {
			if (p.distance_squared_to(q) < 1e-8f) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			pts.push_back(p);
		}
	}
	if (pts.size() < 4) {
		return;
	}

	// Pick 4 non-coplanar vertices for the initial tetrahedron.
	int i0 = 0, i1 = 0;
	for (int i = 1; i < (int)pts.size(); ++i) {
		if (pts[i].x < pts[i0].x)
			i0 = i;
		if (pts[i].x > pts[i1].x)
			i1 = i;
	}
	if (i0 == i1) {
		for (int i = 1; i < (int)pts.size(); ++i) {
			if (pts[i].distance_squared_to(pts[i0]) > pts[i1].distance_squared_to(pts[i0]))
				i1 = i;
		}
	}
	if (i0 == i1 || pts[i0].distance_squared_to(pts[i1]) < 1e-8f) {
		return;
	}

	godot::Vector3 edge01 = (pts[i1] - pts[i0]).normalized();
	int i2 = -1;
	float best_d2 = 1e-8f;
	for (int i = 0; i < (int)pts.size(); ++i) {
		if (i == i0 || i == i1)
			continue;
		godot::Vector3 v = pts[i] - pts[i0];
		float d2 = v.cross(edge01).length_squared();
		if (d2 > best_d2) {
			best_d2 = d2;
			i2 = i;
		}
	}
	if (i2 < 0) {
		return;
	}

	godot::Vector3 tri_n = (pts[i1] - pts[i0]).cross(pts[i2] - pts[i0]).normalized();
	int i3 = -1;
	float best_dist = 1e-5f;
	for (int i = 0; i < (int)pts.size(); ++i) {
		if (i == i0 || i == i1 || i == i2)
			continue;
		float d = std::fabs((pts[i] - pts[i0]).dot(tri_n));
		if (d > best_dist) {
			best_dist = d;
			i3 = i;
		}
	}
	if (i3 < 0) {
		return;
	}

	godot::Vector3 center = (pts[i0] + pts[i1] + pts[i2] + pts[i3]) * 0.25f;
	struct Face {
		int a, b, c;
		godot::Vector3 n; // outward unit normal (CCW: (b-a)x(c-a) points outward)
		bool alive;
	};
	auto make_face = [&](int a, int b, int c) -> Face {
		godot::Vector3 n = (pts[b] - pts[a]).cross(pts[c] - pts[a]);
		if (n.dot(pts[a] - center) < 0.0f) {
			int tmp = b;
			b = c;
			c = tmp;
			n = -n;
		}
		float len = n.length();
		if (len > 1e-12f)
			n /= len;
		return Face{ a, b, c, n, true };
	};

	std::vector<Face> faces;
	faces.push_back(make_face(i0, i1, i2));
	faces.push_back(make_face(i0, i1, i3));
	faces.push_back(make_face(i1, i2, i3));
	faces.push_back(make_face(i2, i0, i3));

	for (int pi = 0; pi < (int)pts.size(); ++pi) {
		if (pi == i0 || pi == i1 || pi == i2 || pi == i3)
			continue;
		const godot::Vector3 &p = pts[pi];

		std::vector<int> visible;
		for (int fi = 0; fi < (int)faces.size(); ++fi) {
			if (!faces[fi].alive)
				continue;
			if (faces[fi].n.dot(p - pts[faces[fi].a]) > 1e-5f) {
				visible.push_back(fi);
			}
		}
		if (visible.empty())
			continue;

		for (int fi : visible) {
			faces[fi].alive = false;
		}

		// Collect directed edges of visible faces; interior edges cancel out in opposite directions.
		std::vector<std::pair<int, int>> edges;
		edges.reserve(visible.size() * 3);
		for (int fi : visible) {
			const Face &f = faces[fi];
			edges.push_back({ f.a, f.b });
			edges.push_back({ f.b, f.c });
			edges.push_back({ f.c, f.a });
		}
		for (size_t ei = 0; ei < edges.size(); ++ei) {
			bool shared = false;
			for (size_t ej = 0; ej < edges.size(); ++ej) {
				if (ei != ej && edges[ei].first == edges[ej].second && edges[ei].second == edges[ej].first) {
					shared = true;
					break;
				}
			}
			if (!shared) {
				faces.push_back(make_face(edges[ei].first, edges[ei].second, pi));
			}
		}
	}

	int base = (int)out.verts.size();
	for (const auto &p : pts) {
		out.verts.push_back(trf.xform(p));
	}
	for (const auto &f : faces) {
		if (!f.alive)
			continue;
		// Emit Godot CW winding (reverse of outward CCW).
		out.tris.push_back(base + f.a);
		out.tris.push_back(base + f.c);
		out.tris.push_back(base + f.b);
	}
}

// Append all triangle surfaces of a Godot Mesh into the raw payload, applying trf to the
// vertices and offsetting indices so multiple surfaces can share one buffer.
inline void append_mesh_surfaces(RawGeometry &out, godot::Ref<godot::Mesh> mesh, const godot::Transform3D &trf) {
	if (mesh.is_null())
		return;
	for (int s = 0; s < mesh->get_surface_count(); s++) {
		if (auto *arr_mesh = godot::Object::cast_to<godot::ArrayMesh>(mesh.ptr())) {
			if (arr_mesh->surface_get_primitive_type(s) != godot::Mesh::PRIMITIVE_TRIANGLES) {
				continue;
			}
		}
		godot::Array dat = mesh->surface_get_arrays(s);
		godot::PackedVector3Array verts = dat[godot::Mesh::ARRAY_VERTEX];
		godot::PackedInt32Array idx = dat[godot::Mesh::ARRAY_INDEX];

		int base = (int)out.verts.size();
		for (int i = 0; i < verts.size(); i++) {
			out.verts.push_back(trf.xform(verts[i]));
		}

		if (idx.size() == 0) {
			int count = (verts.size() / 3) * 3;
			for (int i = 0; i < count; i++) {
				out.tris.push_back(base + i);
			}
		} else {
			int count = (idx.size() / 3) * 3;
			for (int i = 0; i < count; i++) {
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
		godot::Ref<godot::ConvexPolygonShape3D> convex = coll_inst->get_shape();
		append_convex_hull(out, convex->get_points(), trf);
		return out;
	} else {
		SteamAudio::log(SteamAudio::log_error, "SteamAudioGeometry supports sphere, box, cylinder, capsule, concave polygon and convex polygon shapes. Something else was provided, so this geometry will not do anything.");
		return out;
	}

	append_mesh_surfaces(out, mesh, trf);
	return out;
}

#endif // STEAM_AUDIO_GEOMETRY_COMMON_H
