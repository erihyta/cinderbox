#pragma once

// Authoring nodes for maps, and the baker that turns them into a .cbmap (see sim/map.h).
//
// A map scene is an ordinary Godot scene: meshes, lights and particles for the look, plus these
// marker nodes for the parts the simulation needs. The markers carry no behaviour. They are not
// Godot physics bodies on purpose: collision is Box3D's, on the server and inside the client's
// prediction, and Godot's own physics never sees them.
//
// Baking happens offline (the Bake Map button, or tools/bake_map.ps1). At runtime the client gets
// the map from the server, so nothing here runs in a shipped game.
//
// Conventions, matching the simulation:
// - Positions are the node's global position; +Y is up.
// - A static box is rotated by its yaw (around Y) and pitch (around X) only. Roll is ignored, and
//   the baker reports it.
// - `size` is the full size in metres, multiplied by the node's global scale.

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace cb::gd
{

// Solid level geometry: walls, floors, ramps, steps, platforms.
class CbStatic : public godot::Node3D
{
	GDCLASS( CbStatic, godot::Node3D )

public:
	void set_size( const godot::Vector3& size );
	godot::Vector3 get_size() const
	{
		return m_size;
	}

	void _notification( int what );

protected:
	static void _bind_methods();

private:
	void UpdatePreview();

	godot::Vector3 m_size = godot::Vector3( 1.0f, 1.0f, 1.0f );
	godot::MeshInstance3D* m_preview = nullptr;
};

// A dynamic prop the level starts with. Players can push these around.
class CbProp : public godot::Node3D
{
	GDCLASS( CbProp, godot::Node3D )

public:
	enum PropShape
	{
		SHAPE_BOX = 0,
		SHAPE_SPHERE = 1,
	};

	void set_shape( int shape );
	int get_shape() const
	{
		return m_shape;
	}
	void set_size( const godot::Vector3& size );
	godot::Vector3 get_size() const
	{
		return m_size;
	}
	void set_radius( float radius );
	float get_radius() const
	{
		return m_radius;
	}

	void _notification( int what );

protected:
	static void _bind_methods();

private:
	void UpdatePreview();

	int m_shape = SHAPE_BOX;
	godot::Vector3 m_size = godot::Vector3( 0.8f, 0.8f, 0.8f );
	float m_radius = 0.5f;
	godot::MeshInstance3D* m_preview = nullptr;
};

// Where players appear. Exactly one per map; the simulation spreads the slots around it.
class CbSpawn : public godot::Node3D
{
	GDCLASS( CbSpawn, godot::Node3D )

public:
	void set_radius( float radius );
	float get_radius() const
	{
		return m_radius;
	}

	void _notification( int what );

protected:
	static void _bind_methods();

private:
	void UpdatePreview();

	float m_radius = 4.0f;
	godot::MeshInstance3D* m_preview = nullptr;
};

// Walks a scene and writes the .cbmap. Entities are baked in scene-tree order, which is the order
// the simulation creates them in and therefore part of the map's identity.
class CinderboxMapBaker : public godot::RefCounted
{
	GDCLASS( CinderboxMapBaker, godot::RefCounted )

public:
	// Returns { ok, error, path, statics, props, spawn_found, bytes, hash, warnings }.
	godot::Dictionary bake( godot::Node* root, const godot::String& path );

protected:
	static void _bind_methods();
};

} // namespace cb::gd
