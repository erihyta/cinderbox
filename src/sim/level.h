#pragma once

// The sandbox layout. Pure data, so tools and the client can inspect it without a simulation.

#include "components.h"
#include "reflect.h"

#include <string>
#include <vector>

namespace cb
{

// A piece of solid level geometry: a box, a sphere, a capsule, a convex hull, a triangle mesh or a
// height field, turned any way. The turn is
// yaw, then pitch, then roll (Godot's YXZ order): rotation = yaw(Y) * pitch(X) * roll(Z).
// halfExtents as Shape's: a box's half size; a sphere's radius in x; a capsule's radius in x and
// half the distance between its two sphere centres in y (it stands along Y); a hull's bounds,
// with its points in LevelLayout::hulls.
struct LevelBox
{
	b3Vec3 center;
	b3Vec3 halfExtents;
	float pitch; // rotation about X in radians (ramps)
	float yaw;	 // rotation about Y in radians
	float roll = 0.0f; // rotation about Z in radians
	ShapeKind kind = ShapeKind::Box;
	uint32_t hull = 0; // a hull's points: LevelLayout::hulls[hull]; a mesh's, a height field's: theirs
};

// A triangle mesh of the level (a cave, an arch, stairs): its vertices on the position grid, in its
// piece's frame, and three indices a triangle, counter-clockwise seen from the side it faces.
struct LevelMesh
{
	std::vector<b3Vec3> vertices;
	std::vector<uint32_t> indices;
};

// A height field (terrain): countX by countZ points `scale` apart (x, z) from the piece's origin
// at a corner, each height times scale.y; a cell whose `holes` byte is 1 is a hole.
struct LevelHeightField
{
	uint32_t countX = 0;
	uint32_t countZ = 0;
	b3Vec3 scale = { 1.0f, 1.0f, 1.0f };
	std::vector<float> heights;	 // countX * countZ, row by row (z), on the position grid
	std::vector<uint8_t> holes;	 // (countX - 1) * (countZ - 1), or empty: none
};

struct LevelProp
{
	ShapeKind kind;
	b3Vec3 position;
	b3Vec3 halfExtents;
	// Its body, as every body is described (a template's Material, an item's): what it weighs
	// (kg; 0: its volume times `density`, kg/m^3), how it grips, how it bounces.
	float mass = 0.0f;
	float density = 40.0f;
	float friction = 0.6f;
	float restitution = 0.0f;
};

// One placement of a template in the level, turned as a piece is (yaw, pitch, roll).
struct LevelInstance
{
	uint32_t templateIndex;
	b3Vec3 position;
	float pitch;
	float yaw;
	float roll = 0.0f;
};

struct LevelLayout
{
	// Short name of the map, used by clients to find its visuals (res://maps/<name>.tscn).
	// The simulation ignores it: it is presentation data that travels with the level.
	std::string name;
	// Entity definitions the level places and the server can spawn at runtime.
	std::vector<EntityTemplate> templates;
	std::vector<LevelInstance> instances;
	// Template the spawn-prop button creates, or kNoTemplate to keep the built-in behaviour.
	uint32_t spawnTemplate = kNoTemplate;
	std::vector<LevelBox> statics;
	// The hulls' points, each list in its piece's own frame (centred and turned with it, the
	// scale already in), on the position grid. Box3D builds the same hull from them everywhere.
	std::vector<std::vector<b3Vec3>> hulls;
	std::vector<LevelMesh> meshes;
	std::vector<LevelHeightField> heightFields;
	// Bodies of several shapes (a template's CollisionShape3D children): each set's pieces in the
	// body's own frame (a piece's center is its offset from the body's origin).
	std::vector<std::vector<LevelBox>> shapeSets;
	std::vector<LevelProp> props;
	b3Vec3 spawnCenter;
	float spawnRadius;
};

const LevelLayout& GetLevelLayout();

} // namespace cb
