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

	// Who holds an item with both hands: the items in a hand whose kind has a grip, while the other
	// hand is empty.
	world.system( "CollectHandGrips" ).run( []( flecs::iter& it ) {
		flecs::world w = it.world();
		HandGrips& grips = w.get_mut<HandGrips>();
		grips.byHolder.clear();
		std::unordered_map<uint32_t, int> hands; // holder -> how many hands are full
		w.each( [&]( const Visual& v ) {
			if ( v.kind == VisualKind::Item && v.holder != 0 && v.stowed == false && v.socket <= kSocketLeftHand )
			{
				hands[v.holder] += 1;
			}
		} );
		w.each( [&]( const Visual& v ) {
			if ( v.kind != VisualKind::Item || v.holder == 0 || v.stowed || v.socket > kSocketLeftHand || hands[v.holder] != 1 ||
				 v.itemKind >= grips.shapes.size() || grips.shapes[v.itemKind].grip == 0 )
			{
				return;
			}
			const ItemShape& shape = grips.shapes[v.itemKind];
			anim::HandGrip grip;
			grip.leftCarries = v.socket == kSocketLeftHand;
			grip.align = shape.grip == 2;
			grip.asAnimated = shape.grip == 3;
			grip.position = { shape.gripPosition.x, shape.gripPosition.y, shape.gripPosition.z };
			grip.rotation = { { shape.gripRotation[0], shape.gripRotation[1], shape.gripRotation[2] }, shape.gripRotation[3] };
			grips.byHolder[v.holder] = grip;
		} );
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
		const HandGrips& grips = it.world().get<HandGrips>();
		auto grip = grips.byHolder.find( v.netId );
		a.evaluator->Evaluate( a.shown, grip != grips.byHolder.end() ? &grip->second : nullptr );
	} );
}

} // namespace cb::present::scripts
