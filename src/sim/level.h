#pragma once

// The sandbox layout. Pure data, so tools and the client can inspect it without a simulation.

#include "components.h"
#include "reflect.h"

#include <string>
#include <vector>

namespace cb
{

struct LevelBox
{
	b3Vec3 center;
	b3Vec3 halfExtents;
	float pitch; // rotation about X in radians (ramps)
	float yaw;	 // rotation about Y in radians
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

// One placement of a template in the level.
struct LevelInstance
{
	uint32_t templateIndex;
	b3Vec3 position;
	float pitch;
	float yaw;
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
	std::vector<LevelProp> props;
	b3Vec3 spawnCenter;
	float spawnRadius;
};

const LevelLayout& GetLevelLayout();

} // namespace cb
