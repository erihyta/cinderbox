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
			std::string ignored;
			a.evaluator->SetGraph( lib.graph, ignored );
			a.evaluator->SetPacks( lib.packs, lib.packClips );
		}
	} );

	// Sample, blend and build model matrices from the state between the last two ticks; the
	// viewer's own player with its upper layers led by what its looks predicted (anim_lead.h).
	world.system<PlayerAnim, const Visual>( "EvaluatePlayerPose" ).each( []( flecs::iter& it, size_t, PlayerAnim& a, const Visual& v ) {
		if ( a.evaluator == nullptr )
		{
			return;
		}
		float alpha = it.world().get<FrameTiming>().tickAlpha;
		a.shown = anim::InterpolateAnimState( a.previous, a.current, alpha );
		const AnimLead& lead = it.world().get<AnimLead>();
		if ( lead.netId != 0 && lead.netId == v.netId )
		{
			const AnimLibrary& lib = it.world().get<AnimLibrary>();
			if ( lib.graph )
			{
				a.shown = LeadAnimState( a.shown, *lib.graph, lib.packs, lead );
			}
		}
		a.evaluator->Evaluate( a.shown );
	} );
}

} // namespace cb::present::scripts
