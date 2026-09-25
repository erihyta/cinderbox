#include "frame.h"

#include "pose_tools.h"
#include "ragdoll.h"
#include "simulation.h"

#include <algorithm>

namespace cb::present
{

void CaptureFrame( Simulation& sim, PresentationFrame& out )
{
	out.tick = sim.Tick();
	out.tickSeconds = sim.Config().TimeStep();

	// The whole ring travels with the frame: it is small, and a renderer that skipped ticks can
	// then still see every impact it missed.
	const SimGlobals& globals = sim.Globals();
	out.impactCount = globals.impactCount;
	out.impacts.assign( globals.impacts, globals.impacts + kImpactHistory );
	out.modEventCount = globals.modEventCount;
	out.modEvents.assign( globals.modEvents, globals.modEvents + kModEventHistory );
	std::copy( globals.board, globals.board + kBoardSlots, out.board );
	out.entities.clear();
	out.entities.reserve( sim.Entities().size() );
	out.ragdolls.clear();

	for ( const Simulation::EntityRef& ref : sim.Entities() )
	{
		flecs::entity e( sim.World(), ref.entity );
		if ( const Ragdoll* r = e.try_get<Ragdoll>() )
		{
			const RagdollPose& pose = e.get<RagdollPose>();
			FrameRagdoll fr;
			fr.netId = ref.netId;
			fr.owner = r->owner;
			fr.slot = r->slot;
			fr.yaw = r->yaw;
			std::copy( pose.part, pose.part + kRagdollParts, fr.parts );

			FrameEntity f;
			f.netId = ref.netId;
			f.kind = VisualKind::Ragdoll;
			f.slot = r->slot;
			f.transform = RagdollFrame( pose.part[ragdoll::Pelvis], r->yaw );
			f.velocity = pose.linear[ragdoll::Pelvis];
			f.ragdoll = uint32_t( out.ragdolls.size() );
			if ( const Blackboard* b = e.try_get<Blackboard>() )
			{
				f.board = *b;
				f.hasBoard = true;
			}
			out.ragdolls.push_back( fr );
			out.entities.push_back( f );
			continue;
		}
		if ( const HeldItem* item = e.try_get<HeldItem>() )
		{
			FrameEntity f;
			f.netId = ref.netId;
			f.kind = VisualKind::Item;
			f.transform = e.get<Transform>();
			f.holder = item->holder;
			f.itemKind = item->kind;
			f.socket = item->socket;
			if ( const Blackboard* b = e.try_get<Blackboard>() )
			{
				f.board = *b;
				f.hasBoard = true;
			}
			out.entities.push_back( f );
			continue;
		}
		if ( e.has<Shape>() == false )
		{
			continue;
		}
		FrameEntity f;
		f.netId = ref.netId;
		f.transform = e.get<Transform>();
		const Shape& shape = e.get<Shape>();
		f.shape = shape.kind;
		f.halfExtents = shape.halfExtents;

		if ( e.has<StaticGeometry>() )
		{
			f.kind = VisualKind::Static;
		}
		else if ( const Character* c = e.try_get<Character>() )
		{
			f.kind = VisualKind::Player;
			f.slot = c->slot;
		}
		else
		{
			f.kind = VisualKind::Prop;
		}

		if ( const TemplateRef* t = e.try_get<TemplateRef>() )
		{
			f.templateIndex = t->index;
		}
		if ( const Character* ch = e.try_get<Character>() )
		{
			f.stepCount = ch->stepCount;
			f.dead = ch->dead != 0;
		}
		if ( const Blackboard* b = e.try_get<Blackboard>() )
		{
			f.board = *b;
			f.hasBoard = true;
		}
		if ( const Velocity* v = e.try_get<Velocity>() )
		{
			f.velocity = v->linear;
		}
		if ( const AnimState* a = e.try_get<AnimState>() )
		{
			f.anim = *a;
			f.hasAnim = true;
		}
		out.entities.push_back( f );
	}
}

} // namespace cb::present
