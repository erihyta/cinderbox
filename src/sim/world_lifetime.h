#pragma once

// flecs (OS API init counter) and Box3D (static world table) are not safe to create or destroy
// worlds on several threads at once. Everything that creates flecs or Box3D worlds while others
// may be doing the same on other threads (simulations, the presentation mirror, the server's mod
// world) goes through these.

#include "flecs.h"

#include <mutex>

namespace cb
{

std::mutex& WorldLifetimeMutex();
flecs::world CreateFlecsWorld();
void ReleaseFlecsWorld( flecs::world& world );

} // namespace cb
