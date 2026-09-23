#pragma once

// Client-side presentation scripts. Each is a small set of flecs systems/observers registered on
// the presentation world. They never touch the simulation.

#include "flecs.h"

namespace cb::present::scripts
{

// Pop-in when something appears, shrink-out when it disappears.
void RegisterSpawnDestroyEffects( flecs::world& world );

// Player animation: interpolate the simulation's AnimState and evaluate the ozz pose.
void RegisterPlayerAnimation( flecs::world& world );

// Ragdolls: interpolate the parts and hang the skeleton off them, blending in from the player.
void RegisterRagdolls( flecs::world& world );

inline void RegisterAll( flecs::world& world )
{
	RegisterSpawnDestroyEffects( world );
	RegisterPlayerAnimation( world );
	RegisterRagdolls( world );
}

} // namespace cb::present::scripts
