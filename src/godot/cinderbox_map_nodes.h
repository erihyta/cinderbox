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
// - A static piece turns any way: yaw (around Y), pitch (around X) and roll (around Z), Godot's
//   YXZ order, each on a 1/4096 rad grid.
// - `size` is the full size in metres, multiplied by the node's global scale; so are a
//   CollisionShape3D's sizes.

#include "cinderbox_body.h"

#include "components.h"

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <vector>

namespace cb::gd
{

// Solid level geometry: walls, floors, ramps, steps, platforms. Its own box (`size`), or the
// CollisionShape3D children it has (box, sphere, capsule; any rotation, roll too), each baked as a
// piece of its own.
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
	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	void UpdatePreview();

	godot::Vector3 m_size = godot::Vector3( 1.0f, 1.0f, 1.0f );
	godot::MeshInstance3D* m_preview = nullptr;
};

// A dynamic prop the level starts with. A body like any other (CbBody): what it weighs, how it
// grips, how it bounces. Solid to characters (they stand on it and are stopped by it; skills and
// blasts move it), unless `pushed_by_characters`: then they walk through it and shove it (a ball).
class CbProp : public CbBody
{
	GDCLASS( CbProp, CbBody )

public:
	// A map's ball rolls to a stop: it starts with some rolling resistance (an item, whose rolling
	// is its mod's to tune, starts at 0).
	CbProp()
	{
		m_rolling = 0.05;
	}

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
	void set_pushed_by_characters( bool pushed )
	{
		m_pushed = pushed;
	}
	bool get_pushed_by_characters() const
	{
		return m_pushed;
	}

	void _notification( int what );

protected:
	static void _bind_methods();

private:
	void UpdatePreview();

	bool m_pushed = false;
	int m_shape = SHAPE_BOX;
	godot::Vector3 m_size = godot::Vector3( 0.8f, 0.8f, 0.8f );
	float m_radius = 0.5f;
	godot::MeshInstance3D* m_preview = nullptr;
};

// A wall (or anything static) that breaks: its CollisionShape3D children are its shapes whole, its
// meshes its look whole, and its CbProp children are what it breaks into: each piece a body of its
// own (its shapes or its box, its meshes, its Body settings), rubble that players stand on and are
// stopped by but never shove. A mod breaks it (ctx.Break: the frag grenade's blast does); until then
// it is static and costs nothing.
class CbBreakable : public godot::Node3D
{
	GDCLASS( CbBreakable, godot::Node3D )

public:
	godot::PackedStringArray _get_configuration_warnings() const override;
	void _notification( int what );

	// Where it is held up, a bit each: the pieces at its foot (the ground), at its top (a ceiling),
	// at its two ends (pillars). A blast leaves a piece standing while touching pieces join it to a
	// held one.
	enum HeldAt
	{
		HELD_BOTTOM = 1,
		HELD_TOP = 2,
		HELD_SIDES = 4,
	};
	void set_held_at( int held )
	{
		m_heldAt = held;
	}
	int get_held_at() const
	{
		return m_heldAt;
	}
	// Glued (newtons, 0: not): its pieces are bodies from the start, welded where they touch, the held
	// ones static; a weld pulled harder than this breaks. It sags, cracks and falls by its own physics.
	void set_glue( double glue )
	{
		m_glue = glue;
	}
	double get_glue() const
	{
		return m_glue;
	}

protected:
	static void _bind_methods();

private:
	int m_heldAt = HELD_BOTTOM;
	double m_glue = 0.0;
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

// A character's body the map places, standing where the node is, facing where it faces: nobody
// drives it until a player or a mod takes it over. `character` is a character item's name
// ("critter"): the server loads it and players download it.
class CbCharacterBody : public godot::Node3D
{
	GDCLASS( CbCharacterBody, godot::Node3D )

public:
	void set_character( const godot::String& name )
	{
		m_character = name;
		UpdatePreview();
	}
	godot::String get_character() const
	{
		return m_character;
	}

	void _notification( int what );

protected:
	static void _bind_methods();

private:
	void UpdatePreview();

	godot::String m_character;
	godot::MeshInstance3D* m_preview = nullptr;
};

// The nodes of a map's scene a template's look can be (EntityTemplate::look): every CbProp,
// CbTemplate, CbEntity and CbBreakable, in scene-tree order. The bake numbers them and a client finds
// them so. A look drawn is a copy of its node without the look nodes under it (a breakable's pieces).
void MapLookNodes( godot::Node* node, std::vector<godot::Node3D*>& out );
bool IsLookNode( const godot::Node* node );

// One CollisionShape3D as a body's shape where `at` puts it (its place in the body's frame, scale
// and all): its kind, its half size as Shape has it, where it is and how it is turned (no scale:
// that is in the sizes), and a hull's points (in its own frame, scaled). Returns "" when it becomes a
// shape, else why not; what is kept but worth saying (an uneven sphere) goes to `warnings`.
// A triangle mesh (ConcavePolygonShape3D): `points` three a triangle, as Godot has them (clockwise
// seen from the side they face). A height field (HeightMapShape3D): `at` its corner, `countX` by
// `countZ` heights scaled by `scale` (a non-finite one makes the cells around it holes).
struct BakedShape
{
	cb::ShapeKind kind = cb::ShapeKind::Box;
	godot::Vector3 half;
	godot::Transform3D at;
	std::vector<godot::Vector3> points;
	int countX = 0;
	int countZ = 0;
	godot::Vector3 scale;
	std::vector<float> heights;
};
godot::String BakeShape( const godot::CollisionShape3D* node, const godot::Transform3D& at, const godot::String& name, BakedShape& out,
						 godot::Array& warnings );

// Walks a scene and writes the .cbmap. Entities are baked in scene-tree order, which is the order
// the simulation creates them in and therefore part of the map's identity.
class CinderboxMapBaker : public godot::RefCounted
{
	GDCLASS( CinderboxMapBaker, godot::RefCounted )

public:
	// Returns { ok, error, path, statics, pieces, props, spawn_found, bytes, hash, warnings }; pieces:
	// each static piece as stored ({ kind, center, half_extents, rotation: YXZ euler }).
	godot::Dictionary bake( godot::Node* root, const godot::String& path );

protected:
	static void _bind_methods();
};

} // namespace cb::gd
