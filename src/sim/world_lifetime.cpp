#include "world_lifetime.h"

namespace cb
{

std::mutex& WorldLifetimeMutex()
{
	static std::mutex mutex;
	return mutex;
}

flecs::world CreateFlecsWorld()
{
	std::lock_guard<std::mutex> lock( WorldLifetimeMutex() );
	return flecs::world();
}

void ReleaseFlecsWorld( flecs::world& world )
{
	std::lock_guard<std::mutex> lock( WorldLifetimeMutex() );
	world.release();
}

} // namespace cb
