#include "scripts.h"

#include "../presentation.h"

#include <algorithm>

namespace cb::present::scripts
{

namespace
{
constexpr float kSpawnSeconds = 0.18f;
constexpr float kDestroySeconds = 0.25f;

float EaseOutBack( float t )
{
	const float c1 = 1.70158f;
	const float c3 = c1 + 1.0f;
	float u = t - 1.0f;
	return 1.0f + c3 * u * u * u + c1 * u * u;
}
} // namespace

void RegisterSpawnDestroyEffects( flecs::world& world )
{
	world.system<SpawnEffect, RenderPose>( "SpawnEffect" ).each( []( flecs::iter& it, size_t i, SpawnEffect& fx, RenderPose& pose ) {
		fx.time += it.delta_time();
		float t = std::min( fx.time / kSpawnSeconds, 1.0f );
		pose.scale = EaseOutBack( t );
		if ( t >= 1.0f )
		{
			pose.scale = 1.0f;
			it.entity( i ).remove<SpawnEffect>();
		}
	} );

	world.system<DestroyEffect, RenderPose>( "DestroyEffect" ).each( []( flecs::iter& it, size_t i, DestroyEffect& fx, RenderPose& pose ) {
		fx.time += it.delta_time();
		float t = std::min( fx.time / kDestroySeconds, 1.0f );
		pose.scale = 1.0f - t * t;
		pose.position.y -= 0.5f * it.delta_time();
		if ( t >= 1.0f )
		{
			it.entity( i ).destruct();
		}
	} );

	// A visual that starts dying stops following the simulation and cancels its spawn effect.
	world.observer<DestroyEffect>( "OnDestroyEffect" ).event( flecs::OnAdd ).each( []( flecs::entity e, DestroyEffect& ) {
		e.remove<SpawnEffect>();
		e.remove<SimLink>();
	} );
}

} // namespace cb::present::scripts
