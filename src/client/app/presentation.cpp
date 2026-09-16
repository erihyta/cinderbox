#include "presentation.h"

#include "game_client.h"
#include "scripts/scripts.h"

#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <string>

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

// Bone box sizes by joint name (Mixamo naming; anything unknown gets a size from its length).
struct BoneStyle
{
	float width;
	float depth;
	bool body; // body colour instead of limb colour
};

BoneStyle StyleFor( const char* name, float length )
{
	std::string n = name;
	auto has = [&]( const char* part ) { return n.find( part ) != std::string::npos; };
	if ( has( "HeadTop" ) )
		return { 0.26f, 0.26f, true };
	if ( has( "Spine" ) )
		return { 0.34f, 0.2f, true };
	if ( has( "Neck" ) || has( "Head" ) )
		return { 0.1f, 0.1f, true };
	if ( has( "Shoulder" ) )
		return { 0.1f, 0.1f, true };
	if ( has( "UpLeg" ) )
		return { 0.15f, 0.16f, false };
	if ( has( "Leg" ) )
		return { 0.12f, 0.13f, false };
	if ( has( "Toe" ) || has( "Foot" ) )
		return { 0.1f, 0.1f, false };
	if ( has( "Hand" ) )
		return { 0.07f, 0.04f, false };
	if ( has( "Arm" ) )
		return { 0.1f, 0.1f, false };
	float w = std::clamp( length * 0.4f, 0.02f, 0.12f );
	return { w, w, false };
}

Vector3 Column( const ozz::math::Float4x4& m, int c )
{
	float v[4];
	ozz::math::StorePtrU( m.cols[c], v );
	return { v[0], v[1], v[2] };
}

// A box spanning from `a` to `b`, oriented by `refX` (usually the parent joint's X axis).
void DrawBone( Vector3 a, Vector3 b, Vector3 refX, float width, float depth, Color color )
{
	Vector3 axis = Vector3Subtract( b, a );
	float length = Vector3Length( axis );
	if ( length < 1e-4f )
	{
		return;
	}
	Vector3 y = Vector3Scale( axis, 1.0f / length );
	Vector3 x = Vector3Subtract( refX, Vector3Scale( y, Vector3DotProduct( refX, y ) ) );
	if ( Vector3Length( x ) < 1e-3f )
	{
		x = std::fabs( y.x ) < 0.9f ? Vector3{ 1, 0, 0 } : Vector3{ 0, 0, 1 };
		x = Vector3Subtract( x, Vector3Scale( y, Vector3DotProduct( x, y ) ) );
	}
	x = Vector3Normalize( x );
	Vector3 z = Vector3CrossProduct( x, y );
	Vector3 mid = Vector3Lerp( a, b, 0.5f );

	Matrix m = { 0 };
	m.m0 = x.x * width;
	m.m1 = x.y * width;
	m.m2 = x.z * width;
	m.m4 = y.x * length;
	m.m5 = y.y * length;
	m.m6 = y.z * length;
	m.m8 = z.x * depth;
	m.m9 = z.y * depth;
	m.m10 = z.z * depth;
	m.m12 = mid.x;
	m.m13 = mid.y;
	m.m14 = mid.z;
	m.m15 = 1.0f;

	rlPushMatrix();
	rlMultMatrixf( MatrixToFloat( m ) );
	DrawCube( { 0, 0, 0 }, 1.0f, 1.0f, 1.0f, color );
	DrawCubeWires( { 0, 0, 0 }, 1.0f, 1.0f, 1.0f, Fade( BLACK, 0.35f ) );
	rlPopMatrix();
}

} // namespace

void DrawSkeleton( Vector3 feet, Quaternion rotation, float scale, const anim::PoseEvaluator* eval, Color color )
{
	Color body = color;
	Color limbs = ColorBrightness( color, -0.25f );

	// Skeleton space: feet at the origin, facing +Z.
	PushPose( feet, rotation );
	rlScalef( scale, scale, scale );

	if ( eval != nullptr )
	{
		const auto& skeleton = eval->Set().Skeleton();
		const auto& models = eval->Models();
		auto parents = skeleton.joint_parents();
		auto names = skeleton.joint_names();

		for ( int j = 0; j < skeleton.num_joints(); ++j )
		{
			int p = parents[j];
			if ( p < 0 )
			{
				continue;
			}
			Vector3 a = Column( models[p], 3 );
			Vector3 b = Column( models[j], 3 );
			BoneStyle style = StyleFor( names[j], Vector3Distance( a, b ) );
			DrawBone( a, b, Vector3Normalize( Column( models[p], 0 ) ), style.width, style.depth, style.body ? body : limbs );

			// Nose on the head so facing is readable.
			if ( std::string( names[j] ).find( "HeadTop" ) != std::string::npos )
			{
				Vector3 mid = Vector3Lerp( a, b, 0.45f );
				DrawCube( { mid.x, mid.y, mid.z + 0.14f }, 0.06f, 0.06f, 0.06f, DARKGRAY );
			}
		}
	}
	else
	{
		DrawCube( { 0.0f, 0.9f, 0.0f }, 0.5f, 1.8f, 0.3f, body );
	}
	rlPopMatrix();
}

namespace
{

void DrawPlayer( const RenderPose& pose, const Visual& v, const PlayerAnim* anim )
{
	Vector3 feet = { pose.position.x, pose.position.y - kFeetOffset, pose.position.z };
	DrawSkeleton( feet, pose.rotation, pose.scale, anim ? anim->evaluator.get() : nullptr, v.color );

	if ( v.isLocalPlayer )
	{
		Vector3 top = { feet.x, feet.y + 2.1f, feet.z };
		DrawCylinder( top, 0.12f, 0.0f, 0.2f, 4, GOLD );
	}
}

} // namespace

Presentation::Presentation( std::shared_ptr<const anim::AnimSet> animSet )
	: m_animSet( std::move( animSet ) )
{
	m_world.component<SimLink>();
	m_world.component<Visual>();
	m_world.component<TickPoses>();
	m_world.component<RenderPose>();
	m_world.component<PlayerAnim>();
	m_world.set<AnimLibrary>( { m_animSet } );
	m_world.set<FrameTiming>( {} );
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
		const AnimState& state = simEntity.get<AnimState>();
		e.set<PlayerAnim>( { state, state, nullptr } );
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
	m_world.set<FrameTiming>( { client.TickAlpha() } );
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

		if ( const AnimState* state = se.try_get<AnimState>() )
		{
			PlayerAnim& pa = ve.get_mut<PlayerAnim>();
			if ( created || reset )
			{
				pa.previous = *state;
			}
			else if ( advanced )
			{
				pa.previous = pa.current;
			}
			pa.current = *state;
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
			DrawPlayer( pose, v, e.try_get<PlayerAnim>() );
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
