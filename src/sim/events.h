#pragma once

// What the simulation remembers of what just happened, for presentation: rings of the most recent
// impacts and mod events, with counters that only grow, so a viewer can tell what it missed and a
// rollback can un-count what did not happen. Part of the simulation's state (SimGlobals), and of
// every frame a viewer is shown.

#include "box3d/math_functions.h"

#include <cstdint>

namespace cb
{

// One collision hard enough to be worth showing. Both entities are named so presentation can pick
// an effect by what was hit; a static is an entity too, so its NetId appears here like any other.
struct ImpactRecord
{
	uint32_t netIdA = 0;
	uint32_t netIdB = 0;
	uint32_t tick = 0;
	float speed = 0.0f; // approach speed along the contact normal, m/s
	b3Vec3 point = {};
};

// How many impacts presentation can pick up at once. A renderer that falls far behind drops the
// rest, which is the right trade for an effect.
inline constexpr uint32_t kImpactHistory = 16;
inline constexpr uint32_t kImpactsPerTick = 8;

// Something a server mod announced (an Event command): "pistol fired", "player killed". The type
// indexes the event names the server sends on join; the simulation only keeps the record, so
// presentation can play it and a rollback can un-count it.
struct ModEventRecord
{
	uint16_t type = 0;
	uint16_t reserved = 0;
	uint32_t netIdA = 0;
	uint32_t netIdB = 0;
	uint32_t tick = 0;
	int32_t value = 0;
	b3Vec3 point = {};
	b3Vec3 vector = {};
};

inline constexpr uint32_t kModEventHistory = 32;

static_assert( sizeof( ImpactRecord ) == 28, "ImpactRecord layout changed: check for padding" );
static_assert( sizeof( ModEventRecord ) == 44, "ModEventRecord layout changed: check for padding" );

} // namespace cb
