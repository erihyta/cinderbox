#include "capture.h"

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
	out.board = sim.GlobalBoard();
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
			// (Its player's character: the skeleton it hangs off.)
			f.character = uint8_t( sim.CharacterOf( sim.FindEntity( r->owner ) ) ); // (the body it fell from: a player's or an NPC's)
			if ( sim.HasBoard( e ) )
			{
				f.board = sim.GetBoard( e );
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
			if ( item->holder == 0 )
			{
				// In the world the body sits at the shape's centre; presentation draws the item from its grip.
				// (The body is turned in the item's frame: back to that frame first.)
				ItemShape shape = sim.ItemShapeOf( item->kind );
				f.transform.rotation = b3NormalizeQuat(
					b3MulQuat( f.transform.rotation, b3Quat{ { -shape.turn[0], -shape.turn[1], -shape.turn[2] }, shape.turn[3] } ) );
				b3Vec3 center = b3RotateVector( f.transform.rotation, b3Vec3{ shape.center.x, shape.center.y, shape.center.z } );
				f.transform.position = b3Sub( f.transform.position, center );
			}
			f.holder = item->holder;
			f.itemKind = item->kind;
			f.socket = item->socket;
			f.stowed = item->stowed != 0;
			f.inUse = sim.ItemInUse( f.netId );
			f.slotIndex = item->slot;
			f.slotCount = uint8_t( StackOf( *item ) ); // (an item's: how many it is)
			if ( sim.HasBoard( e ) )
			{
				f.board = sim.GetBoard( e );
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

		f.slot = kNoSlot; // (a prop's is its driver's: a slot only while one drives it)
		if ( e.has<StaticGeometry>() )
		{
			f.kind = VisualKind::Static;
		}
		else if ( const DrivenProp* prop = e.try_get<DrivenProp>() )
		{
			// A prop a player drives: drawn as the prop, where its body is (below the mover's centre).
			f.kind = VisualKind::Prop;
			f.slot = e.get<Character>().slot;
			f.transform.position.y -= prop->drop;
		}
		else if ( const Character* c = e.try_get<Character>() )
		{
			f.kind = VisualKind::Player;
			f.slot = c->slot;
		}
		else if ( const IdleBody* idle = e.try_get<IdleBody>() )
		{
			// A character's body nobody drives: drawn as its character, standing (no animation state).
			f.kind = VisualKind::Player;
			f.slot = kNoSlot;
			f.character = uint8_t( sim.CharacterOf( e ) );
			if ( const Slots* slots = e.try_get<Slots>() )
			{
				f.slotIndex = slots->selected;
				f.slotCount = slots->count;
			}
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
			f.linked = sim.EntityHold( f.netId, f.linkEnd, f.linkHolds, f.linkMotion, &f.linkMissed );
			f.motionsOn = sim.EntityMotionsOn( f.netId );
			f.character = uint8_t( sim.CharacterOf( e ) );
			if ( const Slots* slots = e.try_get<Slots>() )
			{
				f.slotIndex = slots->selected;
				f.slotCount = slots->count;
				f.container = slots->open;
			}
		}
		if ( const Container* box = e.try_get<Container>() )
		{
			f.slotCount = box->slots; // (a chest's: how many it has)
		}
		if ( sim.HasBoard( e ) )
		{
			f.board = sim.GetBoard( e );
			f.hasBoard = true;
		}
		if ( const Velocity* v = e.try_get<Velocity>() )
		{
			f.velocity = v->linear;
		}
		if ( sim.HasAnimState( e ) && f.kind == VisualKind::Player )
		{
			// (A driven prop has a state but nothing to pose with it.)
			f.anim = sim.GetAnimState( e );
			f.hasAnim = true;
		}
		out.entities.push_back( f );
	}
}

} // namespace cb::present
