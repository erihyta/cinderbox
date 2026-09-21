#include "mirror.h"

#include "scripts/scripts.h"
#include "simulation.h"

#include <cmath>
#include <mutex>

namespace cb::present
{

namespace
{

constexpr float kCorrectionTime = 0.08f; // seconds for a rollback correction to fade
constexpr float kMaxCorrection = 2.0f;	 // larger corrections snap

b3Quat Nlerp( b3Quat a, b3Quat b, float t )
{
	// Shortest path.
	if ( b3DotQuat( a, b ) < 0.0f )
	{
		b = b3NegateQuat( b );
	}
	return b3NLerp( a, b, t );
}

} // namespace

Mirror::Mirror( std::shared_ptr<const anim::AnimSet> animSet )
	: m_animSet( std::move( animSet ) )
	, m_world( CreateFlecsWorld() ) // the simulation may be creating worlds on another thread
{
	m_world.component<SimLink>();
	m_world.component<Visual>();
	m_world.component<TickPoses>();
	m_world.component<RenderPose>();
	m_world.component<PlayerAnim>();
	m_world.component<SpawnEffect>();
	m_world.component<DestroyEffect>();
	m_world.set<AnimLibrary>( { m_animSet } );
	m_world.set<FrameTiming>( {} );
	m_world.set_ctx( this );
	scripts::RegisterAll( m_world );

	m_world.observer<const Visual, const RenderPose>( "VisualRemoved" )
		.event( flecs::OnRemove )
		.each( []( flecs::iter& it, size_t i, const Visual& v, const RenderPose& p ) {
			auto* self = static_cast<Mirror*>( it.world().get_ctx() );
			if ( self != nullptr )
			{
				self->PushEvent( { EventType::Removed, it.entity( i ).id(), v.netId, v.kind, false, p.position } );
			}
		} );

	m_query = m_world.query<const Visual, const RenderPose>();
}

Mirror::~Mirror()
{
	m_world.set_ctx( nullptr );
	m_query = {};
	ReleaseFlecsWorld( m_world );
}

flecs::entity Mirror::CreateVisual( const FrameEntity& f, bool withEffect )
{
	Visual v;
	v.netId = f.netId;
	v.kind = f.kind;
	v.shape = f.shape;
	v.halfExtents = f.halfExtents;
	v.slot = f.slot;
	v.templateIndex = f.templateIndex;
	if ( f.kind == VisualKind::Static )
	{
		withEffect = false;
	}

	flecs::entity e = m_world.entity();
	e.set<SimLink>( { f.netId } );
	e.set<Visual>( v );
	e.set<TickPoses>( {} );
	e.set<RenderPose>( { f.transform.position, f.transform.rotation, {}, withEffect ? 0.0f : 1.0f } );
	if ( f.kind == VisualKind::Player && f.hasAnim )
	{
		e.set<PlayerAnim>( { f.anim, f.anim, nullptr } );
	}
	if ( withEffect )
	{
		e.set<SpawnEffect>( {} );
	}
	m_events.push_back( { EventType::Spawned, e.id(), f.netId, f.kind, withEffect, f.transform.position } );
	return e;
}

void Mirror::Update( const PresentationFrame& frame, float tickAlpha, float frameSeconds )
{
	m_events.clear();
	Sync( frame, tickAlpha, frameSeconds );
	m_world.set<FrameTiming>( { tickAlpha } );
	m_world.progress( frameSeconds );
}

void Mirror::Sync( const PresentationFrame& frame, float alpha, float frameSeconds )
{
	bool reset = frame.resetGeneration != m_resetGeneration;
	m_resetGeneration = frame.resetGeneration;
	bool advanced = frame.tick != m_lastTick;
	m_lastTick = frame.tick;
	float decay = std::exp( -frameSeconds / kCorrectionTime );
	++m_syncStamp;
	m_localPlayer = flecs::entity();

	for ( const FrameEntity& f : frame.entities )
	{
		auto found = m_byNetId.find( f.netId );
		bool created = found == m_byNetId.end();
		flecs::entity ve;
		if ( created )
		{
			ve = CreateVisual( f, reset == false );
			found = m_byNetId.emplace( f.netId, Entry{ ve.id(), 0 } ).first;
		}
		else
		{
			ve = flecs::entity( m_world, found->second.entity );
		}
		found->second.stamp = m_syncStamp;

		Visual& visual = ve.get_mut<Visual>();
		visual.isLocalPlayer = f.netId == frame.localNetId && frame.localNetId != 0;
		if ( visual.isLocalPlayer )
		{
			m_localPlayer = ve;
		}
		if ( visual.kind == VisualKind::Static && !created && !reset )
		{
			continue; // never moves
		}

		TickPoses& tp = ve.get_mut<TickPoses>();
		RenderPose& rp = ve.get_mut<RenderPose>();
		b3Vec3 before = rp.position;

		if ( created || reset )
		{
			tp.prevPosition = f.transform.position;
			tp.prevRotation = f.transform.rotation;
			rp.correction = {};
		}
		else if ( advanced )
		{
			tp.prevPosition = tp.position;
			tp.prevRotation = tp.rotation;
		}
		tp.position = f.transform.position;
		tp.rotation = f.transform.rotation;
		tp.velocity = f.velocity;

		b3Vec3 interpolated = b3Lerp( tp.prevPosition, tp.position, alpha );
		rp.rotation = Nlerp( tp.prevRotation, tp.rotation, alpha );

		if ( frame.rolledBack && !created && !reset )
		{
			// Whatever moved beyond normal motion this frame is a correction: fade it out.
			b3Vec3 expected = b3MulAdd( before, frameSeconds, tp.velocity );
			rp.correction = b3Sub( expected, interpolated );
			if ( b3Length( rp.correction ) > kMaxCorrection )
			{
				rp.correction = {};
			}
		}
		rp.correction = b3MulSV( decay, rp.correction );
		rp.position = b3Add( interpolated, rp.correction );

		// Footsteps are a counter in the simulation, so a renderer that skipped ticks still gets
		// one step per stride instead of one per frame. A count that went backwards means a
		// rollback or a reset, and is resynced without playing anything.
		if ( created || reset || f.stepCount < visual.stepCount )
		{
			visual.stepCount = f.stepCount;
		}
		else if ( f.stepCount > visual.stepCount )
		{
			visual.stepCount = f.stepCount;
			m_events.push_back( { EventType::Footstep, ve.id(), f.netId, visual.kind, false, rp.position } );
		}

		if ( f.hasAnim )
		{
			PlayerAnim& pa = ve.get_mut<PlayerAnim>();
			AnimMode oldMode = pa.current.mode;
			if ( created || reset )
			{
				pa.previous = f.anim;
			}
			else if ( advanced )
			{
				pa.previous = pa.current;
			}
			pa.current = f.anim;

			if ( !created && !reset && f.anim.mode != oldMode )
			{
				if ( f.anim.mode == AnimMode::JumpStart )
				{
					m_events.push_back( { EventType::Jumped, ve.id(), f.netId, visual.kind, false, rp.position } );
				}
				else if ( f.anim.mode == AnimMode::Land )
				{
					m_events.push_back( { EventType::Landed, ve.id(), f.netId, visual.kind, false, rp.position } );
				}
			}
		}
	}

	// Anything the frame no longer has plays its destroy effect (or vanishes on a reset).
	for ( auto it = m_byNetId.begin(); it != m_byNetId.end(); )
	{
		if ( it->second.stamp == m_syncStamp )
		{
			++it;
			continue;
		}
		flecs::entity ve( m_world, it->second.entity );
		if ( ve.is_alive() )
		{
			const Visual& v = ve.get<Visual>();
			if ( reset || v.kind == VisualKind::Static )
			{
				ve.destruct();
			}
			else
			{
				m_events.push_back( { EventType::Destroying, ve.id(), v.netId, v.kind, true, ve.get<RenderPose>().position } );
				ve.add<DestroyEffect>();
			}
		}
		it = m_byNetId.erase( it );
	}

	SyncImpacts( frame, reset );
}

// Impacts are a ring in the simulation with a count that only grows, so presentation replays
// exactly what it has not seen yet. A count that went backwards is a rollback or a reset: the
// impacts behind it were already played, or belong to a world that no longer exists.
void Mirror::SyncImpacts( const PresentationFrame& frame, bool reset )
{
	if ( reset || frame.impactCount < m_impactCount )
	{
		m_impactCount = frame.impactCount;
		return;
	}
	if ( frame.impacts.size() < kImpactHistory )
	{
		return;
	}

	uint32_t missed = frame.impactCount - m_impactCount;
	uint32_t replay = std::min( missed, kImpactHistory );
	for ( uint32_t i = 0; i < replay; ++i )
	{
		// Oldest of the ones still worth playing, first.
		uint32_t index = ( frame.impactCount - replay + i ) % kImpactHistory;
		const ImpactRecord& record = frame.impacts[index];

		Event event;
		event.type = EventType::Impact;
		event.netId = record.netIdA;
		event.otherNetId = record.netIdB;
		event.strength = record.speed;
		event.position = record.point;
		auto found = m_byNetId.find( record.netIdA );
		if ( found != m_byNetId.end() )
		{
			flecs::entity ve( m_world, found->second.entity );
			if ( ve.is_alive() )
			{
				event.visual = ve.id();
				event.kind = ve.get<Visual>().kind;
			}
		}
		m_events.push_back( event );
	}
	m_impactCount = frame.impactCount;
}

bool Mirror::LocalPlayer( RenderPose& out ) const
{
	if ( m_localPlayer.is_valid() == false || m_localPlayer.is_alive() == false )
	{
		return false;
	}
	out = m_localPlayer.get<RenderPose>();
	return true;
}

} // namespace cb::present
