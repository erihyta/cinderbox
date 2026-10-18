#pragma once

// The level's static shapes, for a viewer's questions (how far its camera can back away), in a
// simulation of their own: the game's runs on its source's thread and is stepped there; this one is
// built on the viewer's thread, from the same map, the first time it is asked after the map changed,
// and nothing ever steps it. Props and what templates place are not in it: only what never moves.

#include "level.h"

#include "box3d/math_functions.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace cb
{

class Simulation;

class LevelQuery
{
public:
	LevelQuery();
	~LevelQuery();

	// The map the source plays now (from its thread).
	void SetLevel( const LevelLayout& layout, uint64_t hash );
	// Simulation::StaticFreeDistance in the level's statics; -1 before there is a level.
	float FreeDistance( b3Vec3 origin, b3Vec3 direction, float maxDistance, float radius );

private:
	std::mutex m_mutex;
	LevelLayout m_pending;
	uint64_t m_pendingHash = 0;
	uint64_t m_builtHash = 0;
	std::unique_ptr<Simulation> m_level;
};

} // namespace cb
