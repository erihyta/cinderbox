#include "scripts.h"

#include "../mirror.h"

namespace cb::present::scripts
{

void RegisterPlayerAnimation( flecs::world& world )
{
	// Give every new player its own pose evaluator (ozz buffers are per instance).
	world.observer<PlayerAnim>( "CreatePoseEvaluator" ).event( flecs::OnSet ).each( []( flecs::iter& it, size_t, PlayerAnim& a ) {
		if ( a.evaluator == nullptr )
		{
			const AnimLibrary& lib = it.world().get<AnimLibrary>();
			a.evaluator = std::make_shared<anim::PoseEvaluator>( *lib.set );
			a.evaluator->SetStances( lib.stances );
		}
	} );

	// Sample, blend and build model matrices from the state between the last two ticks.
	world.system<PlayerAnim>( "EvaluatePlayerPose" ).each( []( flecs::iter& it, size_t, PlayerAnim& a ) {
		if ( a.evaluator == nullptr )
		{
			return;
		}
		float alpha = it.world().get<FrameTiming>().tickAlpha;
		a.evaluator->Evaluate( anim::InterpolateAnimState( a.previous, a.current, alpha ) );
	} );
}

} // namespace cb::present::scripts
