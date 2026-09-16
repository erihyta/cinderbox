#pragma once

// The sandbox layout. Pure data, so tools and the client can inspect it without a simulation.

#include "components.h"

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
};

struct LevelLayout
{
	std::vector<LevelBox> statics;
	std::vector<LevelProp> props;
	b3Vec3 spawnCenter;
	float spawnRadius;
};

const LevelLayout& GetLevelLayout();

} // namespace cb
