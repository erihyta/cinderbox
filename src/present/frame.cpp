#include "frame.h"

#include "simulation.h"

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
	out.entities.clear();
	out.entities.reserve( sim.Entities().size() );

	for ( const Simulation::EntityRef& ref : sim.Entities() )
	{
		flecs::entity e( sim.World(), ref.entity );
		if ( e.has<Shape>() == false )
		{
			continue; // ragdolls: not drawn yet
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
