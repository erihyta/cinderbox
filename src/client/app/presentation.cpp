#include "presentation.h"

#include "game_client.h"
#include "scripts/scripts.h"

#include "raymath.h"
#include "rlgl.h"

#include <cmath>

namespace cb::present
{

namespace
{

constexpr float kCorrectionTime = 0.08f; // seconds for a rollback correction to fade
constexpr float kMaxCorrection = 2.0f;	 // larger corrections snap
constexpr float kFeetOffset = 1.38f;	 // capsule center to the ground while standing

constexpr Color kPlayerColors[] = {
	{ 230, 80, 70, 255 },  { 70, 140, 230, 255 }, { 90, 200, 110, 255 }, { 200, 120, 220, 255 },
	{ 240, 160, 60, 255 }, { 60, 200, 200, 255 }, { 180, 180, 90, 255 }, { 150, 110, 80, 255 },
};

Vector3 ToRay( b3Vec3 v )
{
	return { v.x, v.y, v.z };
}

Quaternion ToRay( b3Quat q )
{
	return { q.v.x, q.v.y, q.v.z, q.s };
}

uint32_t Mix( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;
	return x;
}

Color PropColor( uint32_t netId, bool sphere )
{
	uint32_t h = Mix( netId );
	unsigned char r = uint8_t( 120 + ( h & 0x7F ) );
	unsigned char g = uint8_t( 100 + ( ( h >> 8 ) & 0x7F ) );
	unsigned char b = uint8_t( 90 + ( ( h >> 16 ) & 0x7F ) );
	return sphere ? Color{ b, r, g, 255 } : Color{ r, g, b, 255 };
}

void PushPose( Vector3 position, Quaternion rotation )
{
	rlPushMatrix();
	rlTranslatef( position.x, position.y, position.z );
	Vector3 axis;
	float angle;
	QuaternionToAxisAngle( rotation, &axis, &angle );
	if ( angle != 0.0f )
	{
		rlRotatef( angle * RAD2DEG, axis.x, axis.y, axis.z );
	}
}

void DrawBlock( Vector3 center, Vector3 size, Color color )
{
	DrawCube( center, size.x, size.y, size.z, color );
	DrawCubeWires( center, size.x, size.y, size.z, Fade( BLACK, 0.35f ) );
}

// A limb hanging from a pivot, swung around the local X axis.
void DrawLimb( Vector3 pivot, float swing, Vector3 size, Color color )
{
	rlPushMatrix();
	rlTranslatef( pivot.x, pivot.y, pivot.z );
	rlRotatef( swing * RAD2DEG, 1.0f, 0.0f, 0.0f );
	DrawBlock( { 0.0f, -0.5f * size.y, 0.0f }, size, color );
	rlPopMatrix();
}

void DrawPlayer( const RenderPose& pose, const Visual& v, const PlayerMotion* motion )
{
	PushPose( pose.position, pose.rotation );
	rlScalef( pose.scale, pose.scale, pose.scale );

	float phase = motion ? motion->phase : 0.0f;
	bool grounded = motion ? motion->grounded : true;
	float speed = motion ? motion->groundSpeed : 0.0f;

	// Placeholder locomotion: swing amplitude grows with speed. Replaced by ozz in M3.
	float amount = std::fmin( speed / 6.5f, 1.0f );
	float swing = std::sin( phase * 2.0f * PI ) * ( 0.25f + 0.55f * amount );
	float bob = std::fabs( std::sin( phase * 2.0f * PI ) ) * 0.05f * amount;
	float legL = grounded ? swing : 0.5f;
	float legR = grounded ? -swing : -0.2f;
	float armL = grounded ? -swing : -2.4f;
	float armR = grounded ? swing : -2.4f;

	Color body = v.color;
	Color limbs = ColorBrightness( v.color, -0.25f );
	float feet = -kFeetOffset;
	float hip = feet + 0.8f + bob;

	DrawLimb( { -0.12f, hip, 0.0f }, legL, { 0.16f, 0.8f, 0.18f }, limbs );
	DrawLimb( { 0.12f, hip, 0.0f }, legR, { 0.16f, 0.8f, 0.18f }, limbs );
	DrawBlock( { 0.0f, hip + 0.37f, 0.0f }, { 0.48f, 0.74f, 0.28f }, body );
	DrawLimb( { -0.33f, hip + 0.68f, 0.0f }, armL, { 0.14f, 0.66f, 0.14f }, limbs );
	DrawLimb( { 0.33f, hip + 0.68f, 0.0f }, armR, { 0.14f, 0.66f, 0.14f }, limbs );
	DrawBlock( { 0.0f, hip + 0.94f, 0.0f }, { 0.32f, 0.32f, 0.32f }, ColorBrightness( body, 0.2f ) );
	// Nose shows which way the player faces (+Z local).
	DrawBlock( { 0.0f, hip + 0.94f, 0.2f }, { 0.08f, 0.08f, 0.1f }, DARKGRAY );

	rlPopMatrix();

	if ( v.isLocalPlayer )
	{
		Vector3 top = { pose.position.x, pose.position.y + feet + 1.15f * 2.0f, pose.position.z };
		DrawCylinder( top, 0.12f, 0.0f, 0.2f, 4, GOLD );
	}
}

} // namespace

Presentation::Presentation()
{
	m_world.component<SimLink>();
	m_world.component<Visual>();
	m_world.component<TickPoses>();
	m_world.component<RenderPose>();
	m_world.component<PlayerMotion>();
	m_world.component<SpawnEffect>();
	m_world.component<DestroyEffect>();
	scripts::RegisterAll( m_world );
}

flecs::entity Presentation::CreateVisual( Simulation& sim, flecs::entity simEntity, uint32_t netId, bool withEffect )
{
	Visual v;
	const Shape& shape = simEntity.get<Shape>();
	v.shape = shape.kind;
	v.halfExtents = shape.halfExtents;

	if ( simEntity.has<StaticGeometry>() )
	{
		v.kind = VisualKind::Static;
		float shade = 0.35f + 0.1f * std::fmin( simEntity.get<Transform>().position.y, 3.0f );
		unsigned char c = uint8_t( 255.0f * std::fmin( shade, 0.75f ) );
		v.color = { c, c, uint8_t( c + 12 ), 255 };
		withEffect = false;
	}
	else if ( const Character* ch = simEntity.try_get<Character>() )
	{
		v.kind = VisualKind::Player;
		v.slot = ch->slot;
		v.color = kPlayerColors[ch->slot % ( sizeof( kPlayerColors ) / sizeof( kPlayerColors[0] ) )];
	}
	else
	{
		v.kind = VisualKind::Prop;
		v.color = PropColor( netId, shape.kind == ShapeKind::Sphere );
	}
	(void)sim;

	flecs::entity e = m_world.entity();
	e.set<SimLink>( { netId } );
	e.set<Visual>( v );
	e.set<TickPoses>( {} );
	e.set<RenderPose>( {} );
	if ( v.kind == VisualKind::Player )
	{
		e.set<PlayerMotion>( {} );
	}
	if ( withEffect )
	{
		e.set<SpawnEffect>( {} );
		e.get_mut<RenderPose>().scale = 0.0f;
	}
	return e;
}

void Presentation::Update( GameClient& client, float frameSeconds )
{
	Sync( client, frameSeconds );
	m_world.progress( frameSeconds );
}

void Presentation::Sync( GameClient& client, float frameSeconds )
{
	RollbackSession* session = client.Session();
	if ( session == nullptr )
	{
		return;
	}

	Simulation& sim = session->Sim();
	bool reset = client.ResetGeneration() != m_resetGeneration;
	m_resetGeneration = client.ResetGeneration();
	uint32_t tick = sim.Tick();
	bool advanced = tick != m_lastTick;
	m_lastTick = tick;
	bool rolledBack = client.GetStats().rolledBackLastFrame;
	float alpha = client.TickAlpha();
	float decay = std::exp( -frameSeconds / kCorrectionTime );
	uint32_t localNetId = sim.Globals().playerNetIds[client.Slot()];
	++m_syncStamp;
	m_localPlayer = flecs::entity();

	for ( const Simulation::EntityRef& ref : sim.Entities() )
	{
		flecs::entity se( sim.World(), ref.entity );
		const Transform& t = se.get<Transform>();

		auto found = m_byNetId.find( ref.netId );
		bool created = found == m_byNetId.end();
		flecs::entity ve;
		if ( created )
		{
			ve = CreateVisual( sim, se, ref.netId, reset == false );
			found = m_byNetId.emplace( ref.netId, Entry{ ve.id(), 0 } ).first;
		}
		else
		{
			ve = flecs::entity( m_world, found->second.entity );
		}
		found->second.stamp = m_syncStamp;

		Visual& visual = ve.get_mut<Visual>();
		visual.isLocalPlayer = ref.netId == localNetId;
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
		Vector3 before = rp.position;

		if ( created || reset )
		{
			tp.prevPosition = t.position;
			tp.prevRotation = t.rotation;
			rp.correction = {};
		}
		else if ( advanced )
		{
			tp.prevPosition = tp.position;
			tp.prevRotation = tp.rotation;
		}
		tp.position = t.position;
		tp.rotation = t.rotation;
		if ( const Velocity* vel = se.try_get<Velocity>() )
		{
			tp.velocity = vel->linear;
		}

		Vector3 interpolated = Vector3Lerp( ToRay( tp.prevPosition ), ToRay( tp.position ), alpha );
		rp.rotation = QuaternionNlerp( ToRay( tp.prevRotation ), ToRay( tp.rotation ), alpha );

		if ( rolledBack && !created && !reset )
		{
			// Whatever moved beyond normal motion this frame is a correction: fade it out.
			Vector3 expected = Vector3Add( before, Vector3Scale( ToRay( tp.velocity ), frameSeconds ) );
			Vector3 jump = Vector3Subtract( expected, Vector3Add( interpolated, rp.correction ) );
			rp.correction = Vector3Add( rp.correction, jump );
			if ( Vector3Length( rp.correction ) > kMaxCorrection )
			{
				rp.correction = {};
			}
		}
		rp.correction = Vector3Scale( rp.correction, decay );
		rp.position = Vector3Add( interpolated, rp.correction );

		if ( const Character* ch = se.try_get<Character>() )
		{
			PlayerMotion& m = ve.get_mut<PlayerMotion>();
			m.groundSpeed = std::sqrt( tp.velocity.x * tp.velocity.x + tp.velocity.z * tp.velocity.z );
			m.verticalSpeed = tp.velocity.y;
			m.grounded = ch->grounded != 0;
			m.sprinting = ch->sprinting != 0;
			m.airTicks = ch->airTicks;
			m.groundTicks = ch->groundTicks;
		}
	}

	// Anything the simulation no longer has plays its destroy effect (or vanishes on a reset).
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
			if ( reset || ve.get<Visual>().kind == VisualKind::Static )
			{
				ve.destruct();
			}
			else
			{
				ve.add<DestroyEffect>();
			}
		}
		it = m_byNetId.erase( it );
	}
}

bool Presentation::LocalPlayerPosition( Vector3& out ) const
{
	if ( m_localPlayer.is_valid() == false || m_localPlayer.is_alive() == false )
	{
		return false;
	}
	out = m_localPlayer.get<RenderPose>().position;
	return true;
}

void Presentation::Render()
{
	m_world.each( [&]( flecs::entity e, const Visual& v, const RenderPose& pose ) {
		if ( pose.scale <= 0.001f )
		{
			return;
		}

		if ( v.kind == VisualKind::Player )
		{
			DrawPlayer( pose, v, e.try_get<PlayerMotion>() );
			return;
		}

		Vector3 size = { 2.0f * v.halfExtents.x * pose.scale, 2.0f * v.halfExtents.y * pose.scale,
						 2.0f * v.halfExtents.z * pose.scale };
		PushPose( pose.position, pose.rotation );
		switch ( v.shape )
		{
			case ShapeKind::Box:
				DrawBlock( { 0, 0, 0 }, size, v.color );
				break;
			case ShapeKind::Sphere:
			{
				float r = v.halfExtents.x * pose.scale;
				DrawSphereEx( { 0, 0, 0 }, r, 10, 12, v.color );
				// A band so rolling is visible.
				DrawCylinderWires( { 0, -0.05f * r, 0 }, r * 1.01f, r * 1.01f, 0.1f * r, 12, Fade( BLACK, 0.5f ) );
				break;
			}
			case ShapeKind::Capsule:
			{
				float r = v.halfExtents.x * pose.scale;
				float h = v.halfExtents.y * pose.scale;
				DrawCapsule( { 0, -h, 0 }, { 0, h, 0 }, r, 10, 6, v.color );
				break;
			}
		}
		rlPopMatrix();
	} );
}

} // namespace cb::present
