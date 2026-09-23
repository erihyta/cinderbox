#pragma once

// The mods compiled into this server, in folder-name order (which is also the order they tick in
// when all of them run).

#include "mod_api.h"

#include <memory>
#include <string>
#include <vector>

namespace cb::mods
{

const std::vector<ModInfo>& CompiledMods();

// Null if no compiled mod has that name.
std::unique_ptr<ServerMod> CreateMod( const std::string& name );

} // namespace cb::mods
