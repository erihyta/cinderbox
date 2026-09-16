#pragma once

// Client-side presentation scripts. Each is a small set of flecs systems/observers registered on
// the presentation world. They never touch the simulation.

#include "flecs.h"

namespace cb::present::scripts
{

// Pop-in when something appears, shrink-out when it disappears.
void RegisterSpawnDestroyEffects( flecs::world& world );

// Player body motion: walk-cycle phase, and (from M3) ozz blend weights.
void RegisterPlayerMotion( flecs::world& world );

inline void RegisterAll( flecs::world& world )
{
	RegisterSpawnDestroyEffects( world );
	RegisterPlayerMotion( world );
}

} // namespace cb::present::scripts
