#include "frame.h"

#include "simulation.h"

namespace cb::present
{

void CaptureFrame( Simulation& sim, PresentationFrame& out )
{
	out.tick = sim.Tick();
	out.tickSeconds = sim.Config().TimeStep();
	out.entities.clear();
	out.entities.reserve( sim.Entities().size() );

	for ( const Simulation::EntityRef& ref : sim.Entities() )
	{
		flecs::entity e( sim.World(), ref.entity );
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
