#include "simulation.h"

#include "anim_controller.h"
#include "box3d_shim.h"
#include "detmath.h"
#include "level.h"
#include "util.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cb
{

namespace
{

// Character tuning. Changing any of these changes simulation results.
constexpr float kCapsuleRadius = 0.3f;
constexpr float kCapsuleHalfHeight = 0.5f; // center to sphere center
constexpr float kWalkSpeed = 3.0f;
constexpr float kSprintSpeed = 6.5f;
constexpr float kAccelerate = 12.0f;
constexpr float kFriction = 6.0f;
constexpr float kStopSpeed = 1.0f;
constexpr float kMinSpeed = 0.01f;
constexpr float kGravity = 18.0f;
constexpr float kJumpSpeed = 6.5f;
constexpr float kTurnRate = 12.0f; // rad/s
constexpr float kPogoHertz = 5.0f;
constexpr float kPogoDamping = 0.7f;
constexpr float kInputScale = 1.0f / 127.0f;
constexpr int kMoverIterations = 5;
constexpr int kMaxPlanes = 8;

constexpr b3Vec3 kGravityVector = { 0.0f, -10.0f, 0.0f };

constexpr uint32_t kSnapMagic = 0x43425331u; // 'CBS1'

bool SameShape( b3ShapeId a, b3ShapeId b )
{
	return a.index1 == b.index1 && a.world0 == b.world0 && a.generation == b.generation;
}

struct MoverContext
{
	b3ShapeId self;
	b3Pos origin;
	int count;
	b3CollisionPlane planes[kMaxPlanes];
	b3Pos points[kMaxPlanes];
	b3ShapeId shapes[kMaxPlanes];
};

bool MoverFilter( b3ShapeId shapeId, void* context )
{
	return SameShape( shapeId, static_cast<MoverContext*>( context )->self ) == false;
}

bool CollectPlanes( b3ShapeId shapeId, const b3PlaneResult* results, int count, void* context )
{
	auto* ctx = static_cast<MoverContext*>( context );
	if ( SameShape( shapeId, ctx->self ) )
	{
		return true;
	}

	for ( int i = 0; i < count && ctx->count < kMaxPlanes; ++i )
	{
		ctx->planes[ctx->count] = { results[i].plane, FLT_MAX, 0.0f, true };
		ctx->points[ctx->count] = b3OffsetPos( ctx->origin, results[i].point );
		ctx->shapes[ctx->count] = shapeId;
		ctx->count += 1;
	}
	return true;
}

b3Quat MakeRotation( float yaw, float pitch )
{
	b3Quat qYaw = b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, yaw );
	b3Quat qPitch = b3MakeQuatFromAxisAngle( b3Vec3{ 1.0f, 0.0f, 0.0f }, pitch );
	return b3MulQuat( qYaw, qPitch );
}

void AppendBytes( std::vector<uint8_t>& out, const void* data, size_t size )
{
	const auto* p = static_cast<const uint8_t*>( data );
	out.insert( out.end(), p, p + size );
}

template <typename T>
void AppendValue( std::vector<uint8_t>& out, const T& value )
{
	AppendBytes( out, &value, sizeof( T ) );
}

struct Reader
{
	const uint8_t* data;
	size_t size;
	size_t cursor = 0;

	const uint8_t* Take( size_t n )
	{
		if ( cursor + n > size )
		{
			std::fprintf( stderr, "Simulation: truncated snapshot\n" );
			std::abort();
		}
		const uint8_t* p = data + cursor;
		cursor += n;
		return p;
	}

	template <typename T>
	T Read()
	{
		T value;
		std::memcpy( &value, Take( sizeof( T ) ), sizeof( T ) );
		return value;
	}
};

// flecs (OS API init counter) and Box3D (static world table) are not safe to create or destroy
// worlds from several threads at once. Bots and tests run many simulations on many threads.
std::mutex& LifetimeMutex()
{
	static std::mutex mutex;
	return mutex;
}

flecs::world CreateWorldLocked()
{
	std::lock_guard<std::mutex> lock( LifetimeMutex() );
	return flecs::world();
}

} // namespace

Simulation::Simulation( const SimConfig& config )
	: m_config( config )
	, m_world( CreateWorldLocked() )
{
	m_arena = std::make_unique<PhysicsArena>( size_t( config.physicsArenaMB ) * 1024 * 1024 );
	m_globals.rngState = config.seed;

	RegisterComponents();

	std::unique_lock<std::mutex> lock( LifetimeMutex() );
	PhysicsArena::Scope scope( *m_arena );
	b3WorldDef def = b3DefaultWorldDef();
	def.gravity = kGravityVector;
	def.workerCount = 1;
	def.enqueueTask = nullptr;
	def.finishTask = nullptr;
	m_physicsWorld = b3CreateWorld( &def );
	lock.unlock();

	BuildLevel();
}

Simulation::~Simulation()
{
	std::lock_guard<std::mutex> lock( LifetimeMutex() );
	{
		PhysicsArena::Scope scope( *m_arena );
		b3DestroyWorld( m_physicsWorld );
	}
	m_world.release();
}

template <typename T>
void Simulation::RegisterSnapComponent()
{
	flecs::entity c = m_world.component<T>();
	uint32_t size = std::is_empty_v<T> ? 0 : uint32_t( sizeof( T ) );
	m_snapComponents.push_back( { c.id(), size } );
}

void Simulation::RegisterComponents()
{
	// Order is part of the snapshot format. Append only.
	RegisterSnapComponent<NetId>();
	RegisterSnapComponent<Transform>();
	RegisterSnapComponent<Velocity>();
	RegisterSnapComponent<Shape>();
	RegisterSnapComponent<PhysicsBody>();
	RegisterSnapComponent<Character>();
	RegisterSnapComponent<Prop>();
	RegisterSnapComponent<StaticGeometry>();
	RegisterSnapComponent<AnimState>();

	if ( m_snapComponents.size() > 32 )
	{
		std::fprintf( stderr, "Simulation: too many snapshot components\n" );
		std::abort();
	}
}

flecs::entity Simulation::FindEntity( uint32_t netId ) const
{
	auto it = std::lower_bound( m_entities.begin(), m_entities.end(), netId,
								[]( const EntityRef& r, uint32_t id ) { return r.netId < id; } );
	if ( it == m_entities.end() || it->netId != netId )
	{
		return flecs::entity();
	}
	return flecs::entity( m_world, it->entity );
}

flecs::entity Simulation::CreateEntity()
{
	uint32_t netId = m_globals.nextNetId++;
	flecs::entity e = m_world.entity();
	e.set<NetId>( { netId } );
	// NetIds only grow, so appending keeps the list sorted.
	m_entities.push_back( { netId, e.id() } );
	return e;
}

void Simulation::DestroyEntity( flecs::entity e )
{
	if ( const PhysicsBody* pb = e.try_get<PhysicsBody>() )
	{
		b3DestroyBody( BodyOf( *pb ) );
	}

	uint32_t netId = e.get<NetId>().value;
	auto it = std::lower_bound( m_entities.begin(), m_entities.end(), netId,
								[]( const EntityRef& r, uint32_t id ) { return r.netId < id; } );
	if ( it != m_entities.end() && it->netId == netId )
	{
		m_entities.erase( it );
	}
	e.destruct();
}

b3ShapeId Simulation::CreateShape( b3BodyId body, const Shape& shape, uint64_t category )
{
	b3ShapeDef def = b3DefaultShapeDef();
	def.density = 1.0f;
	def.baseMaterial.friction = 0.6f;
	def.filter.categoryBits = category;
	def.filter.maskBits = ~uint64_t( 0 );

	switch ( shape.kind )
	{
		case ShapeKind::Box:
		{
			b3BoxHull hull = b3MakeBoxHull( shape.halfExtents.x, shape.halfExtents.y, shape.halfExtents.z );
			return b3CreateHullShape( body, &def, &hull.base );
		}
		case ShapeKind::Sphere:
		{
			b3Sphere sphere = { { 0.0f, 0.0f, 0.0f }, shape.halfExtents.x };
			return b3CreateSphereShape( body, &def, &sphere );
		}
		case ShapeKind::Capsule:
		{
			b3Capsule capsule = { { 0.0f, -shape.halfExtents.y, 0.0f }, { 0.0f, shape.halfExtents.y, 0.0f }, shape.halfExtents.x };
			return b3CreateCapsuleShape( body, &def, &capsule );
		}
	}
	return {};
}

void Simulation::BuildLevel()
{
	const LevelLayout& layout = GetLevelLayout();

	for ( const LevelBox& box : layout.statics )
	{
		Transform t{ box.center, MakeRotation( box.yaw, box.pitch ) };
		Shape shape{ ShapeKind::Box, {}, box.halfExtents };

		b3BodyDef bodyDef = b3DefaultBodyDef();
		bodyDef.type = b3_staticBody;
		bodyDef.position = t.position;
		bodyDef.rotation = t.rotation;
		b3BodyId body = b3CreateBody( m_physicsWorld, &bodyDef );
		b3ShapeId shapeId = CreateShape( body, shape, CatStatic );

		flecs::entity e = CreateEntity();
		e.set<Transform>( t );
		e.set<Shape>( shape );
		e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
		e.add<StaticGeometry>();
	}

	for ( const LevelProp& prop : layout.props )
	{
		CreateProp( prop.kind, prop.position, b3Quat{ { 0.0f, 0.0f, 0.0f }, 1.0f }, prop.halfExtents, { 0.0f, 0.0f, 0.0f }, 0, 0 );
	}
}

flecs::entity Simulation::CreateProp( ShapeKind kind, b3Vec3 position, b3Quat rotation, b3Vec3 halfExtents, b3Vec3 velocity,
									  uint32_t owner, uint32_t lifetimeTicks )
{
	flecs::entity e = CreateEntity();
	Shape shape{ kind, {}, halfExtents };

	b3BodyDef bodyDef = b3DefaultBodyDef();
	bodyDef.type = b3_dynamicBody;
	bodyDef.position = position;
	bodyDef.rotation = rotation;
	bodyDef.linearVelocity = velocity;
	b3BodyId body = b3CreateBody( m_physicsWorld, &bodyDef );
	b3ShapeId shapeId = CreateShape( body, shape, CatProp );

	uint32_t despawn = lifetimeTicks > 0 ? m_globals.tick + lifetimeTicks : 0;
	e.set<Transform>( { position, rotation } );
	e.set<Velocity>( { velocity, { 0.0f, 0.0f, 0.0f } } );
	e.set<Shape>( shape );
	e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
	e.set<Prop>( { owner, m_globals.tick, despawn } );
	return e;
}

// Box3D ids embed the world slot, which differs between processes. Components store them with
// world0 = 0 so hashes and portable snapshots match everywhere; the slot is patched in on use.
PhysicsBody Simulation::MakePhysicsBody( b3BodyId body, b3ShapeId shape )
{
	body.world0 = 0;
	shape.world0 = 0;
	return { body, shape };
}

b3BodyId Simulation::BodyOf( const PhysicsBody& pb ) const
{
	b3BodyId id = pb.body;
	id.world0 = uint16_t( m_physicsWorld.index1 - 1 );
	return id;
}

b3ShapeId Simulation::ShapeOf( const PhysicsBody& pb ) const
{
	b3ShapeId id = pb.shape;
	id.world0 = uint16_t( m_physicsWorld.index1 - 1 );
	return id;
}

b3Vec3 Simulation::SpawnPoint( PlayerSlot slot ) const
{
	const LevelLayout& layout = GetLevelLayout();
	float col = float( slot % 8 );
	float row = float( slot / 8 );
	return { layout.spawnCenter.x - 5.25f + 1.5f * col, layout.spawnCenter.y, layout.spawnCenter.z + 1.5f * row };
}

flecs::entity Simulation::CreatePlayer( PlayerSlot slot )
{
	flecs::entity e = CreateEntity();
	uint32_t netId = e.get<NetId>().value;

	Transform t{ SpawnPoint( slot ), detmath::YawRotation( 0.0f ) };
	Shape shape{ ShapeKind::Capsule, {}, { kCapsuleRadius, kCapsuleHalfHeight, 0.0f } };

	b3BodyDef bodyDef = b3DefaultBodyDef();
	bodyDef.type = b3_kinematicBody;
	bodyDef.position = t.position;
	bodyDef.rotation = t.rotation;
	b3BodyId body = b3CreateBody( m_physicsWorld, &bodyDef );
	b3ShapeId shapeId = CreateShape( body, shape, CatPlayer );

	Character c;
	c.slot = slot;

	e.set<Transform>( t );
	e.set<Velocity>( {} );
	e.set<Shape>( shape );
	e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
	e.set<Character>( c );
	e.set<AnimState>( {} );

	m_globals.playerNetIds[slot] = netId;
	return e;
}

void Simulation::ApplyEvents( const InputFrame& frame )
{
	for ( const PlayerEvent& ev : frame.events )
	{
		if ( ev.slot >= kMaxPlayers )
		{
			continue;
		}

		uint32_t& netId = m_globals.playerNetIds[ev.slot];
		if ( ev.type == PlayerEventType::Join && netId == 0 )
		{
			CreatePlayer( ev.slot );
		}
		else if ( ev.type == PlayerEventType::Leave && netId != 0 )
		{
			flecs::entity e = FindEntity( netId );
			if ( e.is_valid() )
			{
				DestroyEntity( e );
			}
			netId = 0;
		}
	}
}

void Simulation::MoveCharacters( const InputFrame& frame )
{
	m_spawnRequests.clear();

	// Slot order == deterministic order, and independent of entity creation history.
	for ( int slot = 0; slot < kMaxPlayers; ++slot )
	{
		uint32_t netId = m_globals.playerNetIds[slot];
		if ( netId == 0 )
		{
			continue;
		}

		flecs::entity e = FindEntity( netId );
		const PlayerInput& in = frame.inputs[slot];
		Character c = e.get<Character>();
		Transform t = e.get<Transform>();
		PhysicsBody pb = e.get<PhysicsBody>();

		uint8_t pressed = uint8_t( in.buttons & ~c.prevButtons );
		if ( pressed & BtnSpawnProp )
		{
			m_spawnRequests.push_back( netId );
		}

		MoveCharacter( c, t, pb, in, pressed );
		c.prevButtons = in.buttons;

		AnimState anim = e.get<AnimState>();
		UpdateAnimState( anim, c, m_globals.tick, m_config.TimeStep() );
		e.set<AnimState>( anim );

		e.set<Character>( c );
		e.set<Transform>( t );
		e.set<Velocity>( { c.velocity, { 0.0f, 0.0f, 0.0f } } );
	}
}

void Simulation::MoveCharacter( Character& c, Transform& t, const PhysicsBody& pb, const PlayerInput& in, uint8_t pressed )
{
	const float dt = m_config.TimeStep();

	// Camera-relative wish direction
	float camYaw = detmath::YawToRadians( in.cameraYaw );
	b3Vec3 forward = detmath::YawForward( camYaw );
	b3Vec3 right = detmath::YawRight( camYaw );
	float throttleForward = float( std::clamp<int>( in.moveForward, -127, 127 ) ) * kInputScale;
	float throttleRight = float( std::clamp<int>( in.moveRight, -127, 127 ) ) * kInputScale;

	if ( c.grounded )
	{
		c.sprinting = ( in.buttons & BtnSprint ) ? 1 : 0;
	}

	// Jump (edge triggered)
	if ( ( pressed & BtnJump ) && c.grounded )
	{
		c.velocity.y = kJumpSpeed;
		c.grounded = 0;
		c.lastJumpTick = m_globals.tick;
	}

	// Ground friction (horizontal only)
	b3Vec3 v = c.velocity;
	float speed = b3Length( b3Vec3{ v.x, 0.0f, v.z } );
	if ( speed < kMinSpeed )
	{
		v.x = 0.0f;
		v.z = 0.0f;
	}
	else if ( c.grounded )
	{
		float control = speed < kStopSpeed ? kStopSpeed : speed;
		float newSpeed = std::max( 0.0f, speed - control * kFriction * dt );
		float ratio = newSpeed / speed;
		v.x *= ratio;
		v.z *= ratio;
	}

	float maxSpeed = c.sprinting ? kSprintSpeed : kWalkSpeed;
	b3Vec3 desired = b3Add( b3MulSV( maxSpeed * throttleForward, forward ), b3MulSV( maxSpeed * throttleRight, right ) );
	float desiredSpeed = 0.0f;
	b3Vec3 desiredDir = b3GetLengthAndNormalize( &desiredSpeed, desired );
	if ( desiredSpeed > maxSpeed )
	{
		desiredSpeed = maxSpeed;
	}

	if ( c.grounded )
	{
		v.y = 0.0f;
	}

	float airControl = c.grounded ? 1.0f : 0.3f;
	float currentSpeed = b3Dot( v, desiredDir );
	float addSpeed = desiredSpeed - currentSpeed;
	if ( addSpeed > 0.0f )
	{
		float accel = std::min( addSpeed, airControl * kAccelerate * maxSpeed * dt );
		v = b3MulAdd( v, accel, desiredDir );
	}

	v.y -= kGravity * dt;

	// Pogo spring keeps the capsule hovering above the ground, which smooths steps and slopes.
	b3Capsule capsule = { { 0.0f, -kCapsuleHalfHeight, 0.0f }, { 0.0f, kCapsuleHalfHeight, 0.0f }, kCapsuleRadius };
	float pogoRest = 3.0f * kCapsuleRadius;
	float rayLength = pogoRest + kCapsuleRadius;
	b3Pos rayOrigin = b3Add( t.position, capsule.center1 );
	b3QueryFilter groundFilter = { CatPlayer, CatStatic | CatProp, 0, nullptr };
	b3RayResult ray = b3World_CastRayClosest( m_physicsWorld, rayOrigin, { 0.0f, -rayLength, 0.0f }, groundFilter );

	bool wasGrounded = c.grounded != 0;
	if ( ray.hit == false || v.y > 0.0f )
	{
		c.grounded = 0;
		c.pogoVelocity = 0.0f;
	}
	else
	{
		c.grounded = 1;
		float current = ray.fraction * rayLength;
		float omega = 2.0f * detmath::kPi * kPogoHertz;
		float omegaH = omega * dt;
		c.pogoVelocity = ( c.pogoVelocity - omega * omegaH * ( current - pogoRest ) ) /
						 ( 1.0f + 2.0f * kPogoDamping * omegaH + omegaH * omegaH );
	}

	if ( c.grounded )
	{
		c.groundTicks = wasGrounded ? c.groundTicks + 1 : 0;
		c.airTicks = 0;
	}
	else
	{
		c.airTicks = wasGrounded ? 0 : c.airTicks + 1;
		c.groundTicks = 0;
	}

	// Move and slide
	b3Vec3 startPosition = t.position;
	b3Vec3 target = b3Add( t.position, b3MulSV( dt, b3Add( v, b3Vec3{ 0.0f, c.pogoVelocity, 0.0f } ) ) );
	b3QueryFilter moverFilter = { CatPlayer, ~uint64_t( 0 ), 0, nullptr };

	MoverContext ctx;
	ctx.self = ShapeOf( pb );
	ctx.count = 0;
	for ( int iteration = 0; iteration < kMoverIterations; ++iteration )
	{
		ctx.count = 0;
		ctx.origin = t.position;
		b3World_CollideMover( m_physicsWorld, t.position, &capsule, moverFilter, CollectPlanes, &ctx );

		b3Vec3 targetDelta = b3Sub( target, t.position );
		b3PlaneSolverResult solved = b3SolvePlanes( targetDelta, ctx.planes, ctx.count );
		float fraction = b3World_CastMover( m_physicsWorld, t.position, &capsule, solved.delta, moverFilter, MoverFilter, &ctx );
		b3Vec3 delta = b3MulSV( fraction, solved.delta );
		t.position = b3Add( t.position, delta );

		if ( b3LengthSquared( delta ) < 0.0001f )
		{
			break;
		}
	}

	// Push dynamic bodies we are touching
	for ( int i = 0; i < ctx.count; ++i )
	{
		b3BodyId other = b3Shape_GetBody( ctx.shapes[i] );
		if ( b3Body_GetType( other ) != b3_dynamicBody )
		{
			continue;
		}

		b3Pos point = ctx.points[i];
		b3Vec3 normal = b3Neg( ctx.planes[i].plane.normal );
		float invMass = b3Body_GetInverseMass( other );
		b3Matrix3 invI = b3Body_GetWorldInverseRotationalInertia( other );
		b3Vec3 r = b3SubPos( point, b3Body_GetWorldCenter( other ) );
		b3Vec3 rn = b3Cross( r, normal );
		float k = invMass + b3Dot( rn, b3MulMV( invI, rn ) );
		float normalMass = k > 0.0f ? 1.0f / k : 0.0f;
		b3Vec3 vOther = b3Add( b3Body_GetLinearVelocity( other ), b3Cross( b3Body_GetAngularVelocity( other ), r ) );
		float vn = b3Dot( b3Sub( vOther, v ), normal );
		float impulse = std::max( -normalMass * vn, 0.0f );
		if ( impulse > 0.0f )
		{
			b3Body_ApplyLinearImpulse( other, b3MulSV( impulse, normal ), point, true );
		}
	}

	v = b3ClipVector( v, ctx.planes, ctx.count );
	c.velocity = v;

	// Face the direction of travel
	b3Vec3 moved = b3Sub( t.position, startPosition );
	float horizontalSq = moved.x * moved.x + moved.z * moved.z;
	if ( horizontalSq > ( 0.2f * dt ) * ( 0.2f * dt ) && desiredSpeed > 0.0f )
	{
		float targetYaw = detmath::Atan2( moved.x, moved.z );
		float diff = detmath::WrapAngle( targetYaw - c.facingYaw );
		float maxTurn = kTurnRate * dt;
		diff = std::clamp( diff, -maxTurn, maxTurn );
		c.facingYaw = detmath::WrapAngle( c.facingYaw + diff );
	}
	t.rotation = detmath::YawRotation( c.facingYaw );

	// Drive the kinematic body so props feel the motion during the physics step.
	b3Body_SetTargetTransform( BodyOf( pb ), { t.position, t.rotation }, dt, true );
}

void Simulation::SpawnProps()
{
	uint32_t lifetime = m_config.PropLifetimeTicks();
	for ( uint32_t ownerId : m_spawnRequests )
	{
		flecs::entity owner = FindEntity( ownerId );
		const Character& c = owner.get<Character>();
		const Transform& t = owner.get<Transform>();

		b3Vec3 fwd = detmath::YawForward( c.facingYaw );
		b3Vec3 pos = b3Add( t.position, b3Add( b3MulSV( 1.2f, fwd ), b3Vec3{ 0.0f, 0.6f, 0.0f } ) );
		b3Vec3 vel = b3Add( c.velocity, b3Add( b3MulSV( 3.0f, fwd ), b3Vec3{ 0.0f, 2.0f, 0.0f } ) );

		uint64_t& rng = m_globals.rngState;
		bool sphere = ( NextRandom( rng ) & 1 ) != 0;
		float size = RandomRange( rng, 0.2f, 0.45f );
		b3Vec3 half = sphere ? b3Vec3{ size, 0.0f, 0.0f } : b3Vec3{ size, size, size };

		CreateProp( sphere ? ShapeKind::Sphere : ShapeKind::Box, pos, detmath::YawRotation( c.facingYaw ), half, vel, ownerId,
					lifetime );
	}
}

void Simulation::ExpireProps()
{
	uint32_t tick = m_globals.tick;
	m_scratch.clear();
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		const Prop* p = e.try_get<Prop>();
		if ( p != nullptr && p->despawnTick != 0 && tick >= p->despawnTick )
		{
			m_scratch.push_back( r );
		}
	}
	for ( const EntityRef& r : m_scratch )
	{
		DestroyEntity( flecs::entity( m_world, r.entity ) );
	}
}

void Simulation::EnforcePropCaps()
{
	// Spawned props in NetId order == spawn order, so the front of each list is the oldest.
	uint32_t perOwner[kMaxPlayers] = {};
	uint32_t total = 0;
	m_scratch.clear();

	// First pass: count.
	for ( const EntityRef& r : m_entities )
	{
		const Prop* p = flecs::entity( m_world, r.entity ).try_get<Prop>();
		if ( p == nullptr || p->owner == 0 )
		{
			continue;
		}
		total += 1;
		flecs::entity owner = FindEntity( p->owner );
		if ( owner.is_valid() )
		{
			perOwner[owner.get<Character>().slot] += 1;
		}
	}

	// Second pass: oldest first, drop while over a cap.
	for ( const EntityRef& r : m_entities )
	{
		const Prop* p = flecs::entity( m_world, r.entity ).try_get<Prop>();
		if ( p == nullptr || p->owner == 0 )
		{
			continue;
		}

		flecs::entity owner = FindEntity( p->owner );
		uint32_t* ownerCount = owner.is_valid() ? &perOwner[owner.get<Character>().slot] : nullptr;
		bool overOwner = ownerCount != nullptr && *ownerCount > m_config.propsPerPlayer;
		bool overGlobal = total > m_config.propsGlobal;
		if ( overOwner || overGlobal )
		{
			m_scratch.push_back( r );
			total -= 1;
			if ( ownerCount != nullptr )
			{
				*ownerCount -= 1;
			}
		}
	}

	for ( const EntityRef& r : m_scratch )
	{
		DestroyEntity( flecs::entity( m_world, r.entity ) );
	}
}

void Simulation::SyncFromPhysics()
{
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		if ( e.has<Prop>() == false )
		{
			continue;
		}

		b3BodyId body = BodyOf( e.get<PhysicsBody>() );
		b3WorldTransform xf = b3Body_GetTransform( body );
		e.set<Transform>( { xf.p, xf.q } );
		e.set<Velocity>( { b3Body_GetLinearVelocity( body ), b3Body_GetAngularVelocity( body ) } );
	}
}

void Simulation::HandleOutOfBounds()
{
	float killY = m_config.killY;
	m_scratch.clear();

	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		const Transform* t = e.try_get<Transform>();
		if ( t == nullptr || t->position.y >= killY || e.has<StaticGeometry>() )
		{
			continue;
		}

		if ( e.has<Character>() )
		{
			Character c = e.get<Character>();
			Transform respawn{ SpawnPoint( c.slot ), detmath::YawRotation( 0.0f ) };
			c.velocity = { 0.0f, 0.0f, 0.0f };
			c.pogoVelocity = 0.0f;
			c.facingYaw = 0.0f;
			c.grounded = 0;
			c.airTicks = 0;
			c.groundTicks = 0;
			e.set<Character>( c );
			e.set<AnimState>( {} );
			e.set<Transform>( respawn );
			e.set<Velocity>( {} );

			b3BodyId body = BodyOf( e.get<PhysicsBody>() );
			b3Body_SetTransform( body, respawn.position, respawn.rotation );
			b3Body_SetLinearVelocity( body, { 0.0f, 0.0f, 0.0f } );
		}
		else
		{
			m_scratch.push_back( r );
		}
	}

	for ( const EntityRef& r : m_scratch )
	{
		DestroyEntity( flecs::entity( m_world, r.entity ) );
	}
}

void Simulation::Step( const InputFrame& frame )
{
	if ( frame.tick != m_globals.tick )
	{
		std::fprintf( stderr, "Simulation::Step: frame for tick %u, expected %u\n", frame.tick, m_globals.tick );
		std::abort();
	}

	PhysicsArena::Scope scope( *m_arena );

	ApplyEvents( frame );
	MoveCharacters( frame );
	SpawnProps();
	ExpireProps();
	EnforcePropCaps();

	b3World_Step( m_physicsWorld, m_config.TimeStep(), int( m_config.subSteps ) );

	SyncFromPhysics();
	HandleOutOfBounds();

	m_globals.tick += 1;
}

void Simulation::SerializeEcs( std::vector<uint8_t>& out ) const
{
	out.clear();
	AppendValue( out, kSnapMagic );
	AppendValue( out, m_globals );
	AppendValue( out, uint32_t( m_entities.size() ) );

	ecs_world_t* world = m_world.c_ptr();
	for ( const EntityRef& r : m_entities )
	{
		uint32_t mask = 0;
		for ( size_t i = 0; i < m_snapComponents.size(); ++i )
		{
			if ( ecs_has_id( world, r.entity, m_snapComponents[i].id ) )
			{
				mask |= uint32_t( 1 ) << i;
			}
		}

		AppendValue( out, r.netId );
		AppendValue( out, mask );
		for ( size_t i = 0; i < m_snapComponents.size(); ++i )
		{
			const SnapComponent& sc = m_snapComponents[i];
			if ( ( mask & ( uint32_t( 1 ) << i ) ) && sc.size > 0 )
			{
				AppendBytes( out, ecs_get_id( world, r.entity, sc.id ), sc.size );
			}
		}
	}
}

void Simulation::DeserializeEcs( const std::vector<uint8_t>& in )
{
	Reader rd{ in.data(), in.size() };
	if ( rd.Read<uint32_t>() != kSnapMagic )
	{
		std::fprintf( stderr, "Simulation: bad snapshot magic\n" );
		std::abort();
	}
	m_globals = rd.Read<SimGlobals>();
	uint32_t count = rd.Read<uint32_t>();

	ecs_world_t* world = m_world.c_ptr();
	std::vector<EntityRef> restored;
	restored.reserve( count );

	size_t existing = 0;
	for ( uint32_t n = 0; n < count; ++n )
	{
		uint32_t netId = rd.Read<uint32_t>();
		uint32_t mask = rd.Read<uint32_t>();

		// Both lists are sorted: delete live entities that the snapshot does not have.
		while ( existing < m_entities.size() && m_entities[existing].netId < netId )
		{
			ecs_delete( world, m_entities[existing].entity );
			++existing;
		}

		flecs::entity_t entity;
		if ( existing < m_entities.size() && m_entities[existing].netId == netId )
		{
			entity = m_entities[existing].entity;
			++existing;
		}
		else
		{
			entity = ecs_new( world );
		}

		for ( size_t i = 0; i < m_snapComponents.size(); ++i )
		{
			const SnapComponent& sc = m_snapComponents[i];
			if ( mask & ( uint32_t( 1 ) << i ) )
			{
				if ( sc.size > 0 )
				{
					ecs_set_id( world, entity, sc.id, sc.size, rd.Take( sc.size ) );
				}
				else
				{
					ecs_add_id( world, entity, sc.id );
				}
			}
			else if ( ecs_has_id( world, entity, sc.id ) )
			{
				ecs_remove_id( world, entity, sc.id );
			}
		}

		restored.push_back( { netId, entity } );
	}

	while ( existing < m_entities.size() )
	{
		ecs_delete( world, m_entities[existing].entity );
		++existing;
	}

	m_entities = std::move( restored );
}

void Simulation::Save( Snapshot& out )
{
	out.tick = m_globals.tick;
	SerializeEcs( out.ecs );
	out.hash = HashBytes( kHashSeed, out.ecs.data(), out.ecs.size() );

	m_arena->Save( out.physics );
	size_t arenaBytes = out.physics.size();
	size_t worldBytes = cbx_WorldStructSize();
	out.physics.resize( arenaBytes + worldBytes );
	std::memcpy( out.physics.data() + arenaBytes, cbx_WorldStructPtr( m_physicsWorld ), worldBytes );
}

void Simulation::Load( const Snapshot& snapshot )
{
	DeserializeEcs( snapshot.ecs );

	size_t worldBytes = cbx_WorldStructSize();
	size_t arenaBytes = snapshot.physics.size() - worldBytes;
	m_arena->Restore( snapshot.physics.data(), arenaBytes );
	std::memcpy( cbx_WorldStructPtr( m_physicsWorld ), snapshot.physics.data() + arenaBytes, worldBytes );
}

uint64_t Simulation::ComputeHash()
{
	SerializeEcs( m_hashScratch );
	return HashBytes( kHashSeed, m_hashScratch.data(), m_hashScratch.size() );
}

} // namespace cb

namespace cb
{

namespace
{
void AppendToVector( void* context, const void* data, size_t size )
{
	AppendBytes( *static_cast<std::vector<uint8_t>*>( context ), data, size );
}
} // namespace

void Simulation::SavePortable( std::vector<uint8_t>& out )
{
	PhysicsArena::Scope scope( *m_arena );
	SerializeEcs( m_hashScratch );
	out.clear();
	AppendValue( out, uint32_t( m_hashScratch.size() ) );
	AppendBytes( out, m_hashScratch.data(), m_hashScratch.size() );
	cbx_SerializeWorld( m_physicsWorld, AppendToVector, &out );
}

bool Simulation::LoadPortable( const std::vector<uint8_t>& in )
{
	Reader rd{ in.data(), in.size() };
	uint32_t ecsSize = rd.Read<uint32_t>();
	const uint8_t* ecs = rd.Take( ecsSize );

	PhysicsArena::Scope scope( *m_arena );
	if ( cbx_DeserializeWorld( m_physicsWorld, in.data() + rd.cursor, in.size() - rd.cursor ) == false )
	{
		return false;
	}
	m_hashScratch.assign( ecs, ecs + ecsSize );
	DeserializeEcs( m_hashScratch );
	return true;
}

} // namespace cb
