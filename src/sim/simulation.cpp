#include "simulation.h"

#include "anim_controller.h"
#include "box3d_shim.h"
#include "detmath.h"
#include "level.h"
#include "ragdoll.h"
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

// Footsteps. A stride is a distance, so the step rate follows the speed on its own.
constexpr float kStrideLength = 1.6f;
constexpr float kStepMinSpeed = 0.5f;

// Collisions approaching slower than this never become impacts.
constexpr float kImpactThreshold = 1.5f;

constexpr uint32_t kSnapMagic = 0x43425331u; // 'CBS1'

b3Vec3 ToVec( const Float3& f )
{
	return { f.x, f.y, f.z };
}

// Deterministic: inf - inf and NaN - NaN are NaN, which never compares equal.
bool IsFinite( float f )
{
	return f - f == 0.0f;
}

bool IsFinite( const Float3& f )
{
	return IsFinite( f.x ) && IsFinite( f.y ) && IsFinite( f.z );
}

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

} // namespace

std::mutex& WorldLifetimeMutex()
{
	static std::mutex mutex;
	return mutex;
}

flecs::world CreateFlecsWorld()
{
	std::lock_guard<std::mutex> lock( WorldLifetimeMutex() );
	return flecs::world();
}

void ReleaseFlecsWorld( flecs::world& world )
{
	std::lock_guard<std::mutex> lock( WorldLifetimeMutex() );
	world.release();
}

Simulation::Simulation( const SimConfig& config, const LevelLayout& map )
	: m_config( config )
	, m_map( map )
	, m_world( CreateFlecsWorld() )
{
	m_arena = std::make_unique<PhysicsArena>( size_t( config.physicsArenaMB ) * 1024 * 1024 );
	m_globals.rngState = config.seed;

	RegisterComponents();

	std::unique_lock<std::mutex> lock( WorldLifetimeMutex() );
	PhysicsArena::Scope scope( *m_arena );
	b3WorldDef def = b3DefaultWorldDef();
	def.gravity = kGravityVector;
	def.hitEventThreshold = kImpactThreshold;
	def.workerCount = 1;
	def.enqueueTask = nullptr;
	def.finishTask = nullptr;
	m_physicsWorld = b3CreateWorld( &def );
	lock.unlock();

	BuildLevel();
}

Simulation::~Simulation()
{
	{
		std::lock_guard<std::mutex> lock( WorldLifetimeMutex() );
		PhysicsArena::Scope scope( *m_arena );
		b3DestroyWorld( m_physicsWorld );
	}
	ReleaseFlecsWorld( m_world );
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
	RegisterSnapComponent<TemplateRef>();
	RegisterSnapComponent<Blackboard>();
	RegisterSnapComponent<Ragdoll>();
	RegisterSnapComponent<RagdollBodies>();
	RegisterSnapComponent<RagdollPose>();

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
	if ( const RagdollBodies* rb = e.try_get<RagdollBodies>() )
	{
		// Destroying a body destroys its joints with it.
		for ( b3BodyId body : rb->body )
		{
			body.world0 = uint16_t( m_physicsWorld.index1 - 1 );
			b3DestroyBody( body );
		}
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

b3ShapeId Simulation::CreateShape( b3BodyId body, const Shape& shape, uint64_t category, const ShapeMaterial& material )
{
	b3ShapeDef def = b3DefaultShapeDef();
	def.density = material.density;
	def.baseMaterial.friction = material.friction;
	def.baseMaterial.restitution = material.restitution;
	def.filter.categoryBits = category;
	def.filter.maskBits = ~uint64_t( 0 );
	// Either shape enabling hit events is enough, so the static level does not need them.
	def.enableHitEvents = category != CatStatic;

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
	const LevelLayout& layout = m_map;

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

	// Template instances come last, in the order the map lists them.
	for ( const LevelInstance& instance : layout.instances )
	{
		CreateFromTemplate( instance.templateIndex, instance.position, MakeRotation( instance.yaw, instance.pitch ),
							{ 0.0f, 0.0f, 0.0f }, 0 );
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

flecs::entity Simulation::CreateFromTemplate( uint32_t templateIndex, b3Vec3 position, b3Quat rotation, b3Vec3 extraVelocity,
											  uint32_t owner )
{
	if ( templateIndex >= m_map.templates.size() )
	{
		return {};
	}
	const EntityTemplate& t = m_map.templates[templateIndex];

	// Shape. The author picks a kind and the fields that kind uses; the rest are ignored.
	constexpr uint32_t kShape = Fnv32( "Shape" );
	Shape shape;
	shape.kind = ShapeKind( std::clamp( TemplateInt( t, kShape, Fnv32( "kind" ), 0 ), 0, int32_t( ShapeKind::Capsule ) ) );
	switch ( shape.kind )
	{
		case ShapeKind::Sphere:
			shape.halfExtents = { TemplateFloat( t, kShape, Fnv32( "radius" ), 0.5f ), 0.0f, 0.0f };
			break;
		case ShapeKind::Capsule:
		{
			float radius = TemplateFloat( t, kShape, Fnv32( "radius" ), 0.5f );
			float height = TemplateFloat( t, kShape, Fnv32( "height" ), 2.0f );
			// Box3D wants the distance between the two sphere centres, which cannot go negative.
			shape.halfExtents = { radius, std::max( 0.5f * height - radius, 0.0f ), 0.0f };
			break;
		}
		case ShapeKind::Box:
		default:
		{
			b3Vec3 size = TemplateVec3( t, kShape, Fnv32( "size" ), b3Vec3{ 1.0f, 1.0f, 1.0f } );
			shape.halfExtents = b3MulSV( 0.5f, size );
			break;
		}
	}

	// Body. Defaults come from Box3D, so an unauthored field behaves exactly as before.
	constexpr uint32_t kBody = Fnv32( "Body" );
	b3BodyDef bodyDef = b3DefaultBodyDef();
	int32_t bodyType = std::clamp( TemplateInt( t, kBody, Fnv32( "type" ), int32_t( b3_dynamicBody ) ), 0, 2 );
	bodyDef.type = b3BodyType( bodyType );
	bodyDef.gravityScale = TemplateFloat( t, kBody, Fnv32( "gravity_scale" ), bodyDef.gravityScale );
	bodyDef.linearDamping = TemplateFloat( t, kBody, Fnv32( "linear_damping" ), bodyDef.linearDamping );
	bodyDef.angularDamping = TemplateFloat( t, kBody, Fnv32( "angular_damping" ), bodyDef.angularDamping );

	constexpr uint32_t kVelocity = Fnv32( "Velocity" );
	b3Vec3 linear = b3Add( TemplateVec3( t, kVelocity, Fnv32( "linear" ), b3Vec3{ 0.0f, 0.0f, 0.0f } ), extraVelocity );
	b3Vec3 angular = TemplateVec3( t, kVelocity, Fnv32( "angular" ), b3Vec3{ 0.0f, 0.0f, 0.0f } );
	bodyDef.position = position;
	bodyDef.rotation = rotation;
	bodyDef.linearVelocity = linear;
	bodyDef.angularVelocity = angular;

	constexpr uint32_t kMaterial = Fnv32( "Material" );
	ShapeMaterial material;
	material.density = TemplateFloat( t, kMaterial, Fnv32( "density" ), material.density );
	material.friction = TemplateFloat( t, kMaterial, Fnv32( "friction" ), material.friction );
	material.restitution = TemplateFloat( t, kMaterial, Fnv32( "restitution" ), material.restitution );

	bool isStatic = bodyDef.type == b3_staticBody;
	b3BodyId body = b3CreateBody( m_physicsWorld, &bodyDef );
	b3ShapeId shapeId = CreateShape( body, shape, isStatic ? CatStatic : CatProp, material );

	flecs::entity e = CreateEntity();
	e.set<Transform>( { position, rotation } );
	e.set<Velocity>( { linear, angular } );
	e.set<Shape>( shape );
	e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
	e.set<TemplateRef>( { templateIndex } );

	constexpr uint32_t kProp = Fnv32( "Prop" );
	if ( TemplateHas( t, kProp ) )
	{
		float lifetime = TemplateFloat( t, kProp, Fnv32( "lifetime_seconds" ), 0.0f );
		uint32_t ticks = uint32_t( std::max( lifetime, 0.0f ) * float( m_config.tickRate ) );
		e.set<Prop>( { owner, m_globals.tick, ticks > 0 ? m_globals.tick + ticks : 0 } );
	}
	if ( isStatic )
	{
		// Level geometry: it does not fall, so it must not be culled by the kill plane either.
		e.add<StaticGeometry>();
	}
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
	const LevelLayout& layout = m_map;
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

void Simulation::PlaceCharacter( flecs::entity e, b3Vec3 position, float yaw )
{
	Character c = e.get<Character>();
	Transform t{ position, detmath::YawRotation( yaw ) };
	c.velocity = { 0.0f, 0.0f, 0.0f };
	c.pogoVelocity = 0.0f;
	c.facingYaw = yaw;
	c.grounded = 0;
	c.airTicks = 0;
	c.groundTicks = 0;
	c.stepDistance = 0.0f;
	e.set<Character>( c );
	// A fresh start for the animation, but aiming is a mod's decision (the pistol is still out), so
	// placing the character does not undo it.
	AnimState anim;
	anim.aiming = e.get<AnimState>().aiming;
	e.set<AnimState>( anim );
	e.set<Transform>( t );
	e.set<Velocity>( {} );

	b3BodyId body = BodyOf( e.get<PhysicsBody>() );
	b3Body_SetTransform( body, t.position, t.rotation );
	b3Body_SetLinearVelocity( body, { 0.0f, 0.0f, 0.0f } );
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
		if ( c.dead )
		{
			// Held buttons are still tracked, so a jump held through the respawn is not a press.
			c.prevButtons = in.buttons;
			e.set<Character>( c );
			continue;
		}
		Transform t = e.get<Transform>();
		PhysicsBody pb = e.get<PhysicsBody>();

		uint8_t pressed = uint8_t( in.buttons & ~c.prevButtons );

		// Frozen: the mover still runs (gravity, the ground, being pushed), with no intent.
		PlayerInput still;
		still.cameraYaw = in.cameraYaw;
		MoveCharacter( c, t, pb, c.frozen ? still : in, c.frozen ? uint8_t( 0 ) : pressed );
		c.prevButtons = in.buttons;

		AnimState anim = e.get<AnimState>();
		UpdateAnimState( anim, c, in, m_globals.tick, m_config.TimeStep() );
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
	b3QueryFilter groundFilter = { CatPlayer, CatStatic | CatProp | CatRagdoll, 0, nullptr };
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

	// Face where the camera looks (a mod chose it), or turn toward the direction of travel.
	b3Vec3 moved = b3Sub( t.position, startPosition );
	float horizontalSq = moved.x * moved.x + moved.z * moved.z;
	if ( c.faceCamera != 0 )
	{
		c.facingYaw = detmath::WrapAngle( detmath::YawToRadians( in.cameraYaw ) );
	}
	else if ( horizontalSq > ( 0.2f * dt ) * ( 0.2f * dt ) && desiredSpeed > 0.0f )
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
			continue;
		}
		const Ragdoll* rd = e.try_get<Ragdoll>();
		if ( rd != nullptr && rd->despawnTick != 0 && tick >= rd->despawnTick )
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
		if ( const RagdollBodies* rb = e.try_get<RagdollBodies>() )
		{
			RagdollPose pose;
			for ( int i = 0; i < kRagdollParts; ++i )
			{
				b3BodyId body = rb->body[i];
				body.world0 = uint16_t( m_physicsWorld.index1 - 1 );
				b3WorldTransform xf = b3Body_GetTransform( body );
				pose.part[i] = { xf.p, xf.q };
				pose.linear[i] = b3Body_GetLinearVelocity( body );
			}
			e.set<RagdollPose>( pose );
			e.set<Transform>( pose.part[ragdoll::Pelvis] );
			continue;
		}

		// Everything the physics engine moves: props and entities placed from a map template.
		// Static geometry never moves, and characters are driven by the mover instead.
		if ( e.has<PhysicsBody>() == false || e.has<StaticGeometry>() || e.has<Character>() )
		{
			continue;
		}

		b3BodyId body = BodyOf( e.get<PhysicsBody>() );
		b3WorldTransform xf = b3Body_GetTransform( body );
		e.set<Transform>( { xf.p, xf.q } );
		e.set<Velocity>( { b3Body_GetLinearVelocity( body ), b3Body_GetAngularVelocity( body ) } );
	}
}

void Simulation::CollectImpacts()
{
	b3ContactEvents events = b3World_GetContactEvents( m_physicsWorld );
	if ( events.hitCount <= 0 )
	{
		return;
	}

	// Shapes know nothing about entities, so map them back by shape index. Rebuilt only on ticks
	// that actually produced a hit, which is rare enough to keep this off the common path.
	BuildShapeLookup();
	auto netIdOf = [this]( b3ShapeId shape ) { return NetIdOfShape( shape ); };

	m_impactScratch.clear();
	for ( int i = 0; i < events.hitCount; ++i )
	{
		const b3ContactHitEvent& hit = events.hitEvents[i];
		ImpactRecord record;
		record.netIdA = netIdOf( hit.shapeIdA );
		record.netIdB = netIdOf( hit.shapeIdB );
		record.tick = m_globals.tick;
		record.speed = hit.approachSpeed;
		record.point = { hit.point.x, hit.point.y, hit.point.z };
		m_impactScratch.push_back( record );
	}

	// Box3D reports these in its own order. Sorting by strength, then by the entities involved,
	// keeps what lands in the state independent of that order, and keeps the loudest hits when
	// more happen in one tick than the ring can hold.
	std::sort( m_impactScratch.begin(), m_impactScratch.end(), []( const ImpactRecord& a, const ImpactRecord& b ) {
		if ( a.speed != b.speed )
		{
			return a.speed > b.speed;
		}
		if ( a.netIdA != b.netIdA )
		{
			return a.netIdA < b.netIdA;
		}
		return a.netIdB < b.netIdB;
	} );

	size_t count = std::min( m_impactScratch.size(), size_t( kImpactsPerTick ) );
	for ( size_t i = 0; i < count; ++i )
	{
		m_globals.impacts[m_globals.impactCount % kImpactHistory] = m_impactScratch[i];
		m_globals.impactCount += 1;
	}
}

void Simulation::UpdateFootsteps()
{
	const float dt = m_config.TimeStep();
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		Character* c = e.try_get_mut<Character>();
		if ( c == nullptr )
		{
			continue;
		}

		// Stride phase, not a timer: steps stay in step with how far the character actually moved,
		// so walking and sprinting sound right without a separate rate for each.
		float speed = b3Length( b3Vec3{ c->velocity.x, 0.0f, c->velocity.z } );
		if ( c->dead || c->grounded == 0 || speed < kStepMinSpeed )
		{
			c->stepDistance = 0.0f;
			continue;
		}
		c->stepDistance += speed * dt;
		if ( c->stepDistance >= kStrideLength )
		{
			c->stepDistance -= kStrideLength;
			c->stepCount += 1;
		}
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
			if ( c.dead )
			{
				continue; // its body is disabled and it does not move
			}
			// The engine only rescues the character; whether that was a death is a mod's call.
			c.fallCount += 1;
			e.set<Character>( c );
			PlaceCharacter( e, SpawnPoint( c.slot ), 0.0f );
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
	ApplyCommands( frame );
	MoveCharacters( frame );
	ExpireProps();
	EnforcePropCaps();

	b3World_Step( m_physicsWorld, m_config.TimeStep(), int( m_config.subSteps ) );

	CollectImpacts();
	SyncFromPhysics();
	UpdateFootsteps();
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

// --- Commands --------------------------------------------------------------------------------------

namespace cb
{

uint32_t Simulation::ResolveTarget( uint32_t target ) const
{
	if ( target & kSlotTargetBit )
	{
		uint32_t slot = target & ~kSlotTargetBit;
		return slot < uint32_t( kMaxPlayers ) ? m_globals.playerNetIds[slot] : 0;
	}
	return target;
}

void Simulation::ApplyCommands( const InputFrame& frame )
{
	for ( const SimCommand& command : frame.commands )
	{
		ApplyCommand( command );
	}
}

void Simulation::ApplyCommand( const SimCommand& command )
{
	// A server never sends these, but a bad value must not reach Box3D on anyone's machine.
	if ( IsFinite( command.a ) == false || IsFinite( command.b ) == false || IsFinite( command.c ) == false )
	{
		return;
	}

	switch ( command.type )
	{
		case CommandType::SetField:
		{
			if ( command.index >= kBoardSlots )
			{
				return;
			}
			if ( command.target == 0 )
			{
				m_globals.board[command.index] = command.value;
				return;
			}
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() == false )
			{
				return;
			}
			Blackboard board = e.has<Blackboard>() ? e.get<Blackboard>() : Blackboard{};
			board.values[command.index] = command.value;
			e.set<Blackboard>( board );
			return;
		}

		case CommandType::Event:
		{
			ModEventRecord record;
			record.type = command.index;
			record.netIdA = ResolveTarget( command.target );
			record.netIdB = ResolveTarget( command.other );
			record.tick = m_globals.tick;
			record.value = command.value;
			record.point = ToVec( command.a );
			record.vector = ToVec( command.b );
			m_globals.modEvents[m_globals.modEventCount % kModEventHistory] = record;
			m_globals.modEventCount += 1;
			return;
		}

		case CommandType::SpawnProp:
		{
			uint32_t owner = 0;
			if ( command.target != 0 )
			{
				owner = ResolveTarget( command.target );
				if ( owner == 0 || FindEntity( owner ).is_valid() == false )
				{
					return; // the player it was for has left
				}
			}
			b3Vec3 position = ToVec( command.a );
			b3Vec3 velocity = ToVec( command.b );
			b3Quat rotation = detmath::YawRotation( detmath::YawToRadians( command.index ) );
			uint32_t lifetime = command.other;

			if ( command.value >= 0 )
			{
				flecs::entity e = CreateFromTemplate( uint32_t( command.value ), position, rotation, velocity, owner );
				// Whatever the template says, something a player spawned expires and counts against
				// the caps; otherwise a mod could let players fill the world.
				if ( e.is_valid() && owner != 0 && ( e.has<Prop>() == false || e.get<Prop>().despawnTick == 0 ) )
				{
					uint32_t ticks = lifetime > 0 ? lifetime : m_config.PropLifetimeTicks();
					e.set<Prop>( { owner, m_globals.tick, m_globals.tick + ticks } );
				}
				return;
			}

			ShapeKind kind = ShapeKind( std::min<uint8_t>( command.mode, uint8_t( ShapeKind::Capsule ) ) );
			b3Vec3 half = ToVec( command.c );
			half = { std::clamp( half.x, 0.02f, 8.0f ), std::clamp( half.y, 0.0f, 8.0f ), std::clamp( half.z, 0.02f, 8.0f ) };
			if ( kind == ShapeKind::Box )
			{
				half.y = std::max( half.y, 0.02f );
			}
			CreateProp( kind, position, rotation, half, velocity, owner, lifetime );
			return;
		}

		case CommandType::Destroy:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			// Players come and go with join and leave events, never by command.
			if ( e.is_valid() && e.has<Character>() == false )
			{
				DestroyEntity( e );
			}
			return;
		}

		case CommandType::Impulse:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() )
			{
				ApplyImpulse( e, command );
			}
			return;
		}

		case CommandType::Kill:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() && e.has<Character>() )
			{
				KillPlayer( e, command );
			}
			return;
		}

		case CommandType::Respawn:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() && e.has<Character>() )
			{
				RespawnPlayer( e, command );
			}
			return;
		}

		case CommandType::Facing:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() && e.has<Character>() )
			{
				Character c = e.get<Character>();
				c.faceCamera = command.mode != 0 ? 1 : 0;
				e.set<Character>( c );
			}
			return;
		}

		case CommandType::Aim:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() && e.has<AnimState>() )
			{
				AnimState a = e.get<AnimState>();
				a.aiming = command.mode != 0 ? 1 : 0;
				e.set<AnimState>( a );
			}
			return;
		}

		case CommandType::Freeze:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() && e.has<Character>() )
			{
				Character c = e.get<Character>();
				c.frozen = command.mode != 0 ? 1 : 0;
				e.set<Character>( c );
			}
			return;
		}
	}
}

namespace
{

void PushBody( b3BodyId body, b3Vec3 point, b3Vec3 vector, uint8_t mode )
{
	if ( b3Body_GetType( body ) != b3_dynamicBody )
	{
		return;
	}
	b3Vec3 impulse = mode == ImpulseVelocity ? b3MulSV( b3Body_GetMass( body ), vector ) : vector;
	b3Body_ApplyLinearImpulse( body, impulse, point, true );
}

} // namespace

void Simulation::ApplyImpulse( flecs::entity e, const SimCommand& command )
{
	b3Vec3 point = ToVec( command.a );
	b3Vec3 vector = ToVec( command.b );

	if ( e.has<Character>() )
	{
		// Characters are moved by the mover, not by forces, and have no mass it knows about:
		// knockback is a change of velocity whatever the mode.
		Character c = e.get<Character>();
		if ( c.dead == 0 )
		{
			c.velocity = b3Add( c.velocity, vector );
			if ( vector.y > 0.0f )
			{
				// A grounded character has its vertical speed cleared by the mover; a push
				// upward has to leave the ground to count, the same way a jump does.
				c.grounded = 0;
			}
			e.set<Character>( c );
		}
		return;
	}

	if ( const RagdollBodies* rb = e.try_get<RagdollBodies>() )
	{
		// The part nearest the point takes the hit; the joints pass it on.
		int nearest = 0;
		float best = FLT_MAX;
		for ( int i = 0; i < kRagdollParts; ++i )
		{
			b3BodyId body = rb->body[i];
			body.world0 = uint16_t( m_physicsWorld.index1 - 1 );
			float d = b3DistanceSquared( b3Body_GetWorldCenter( body ), point );
			if ( d < best )
			{
				best = d;
				nearest = i;
			}
		}
		b3BodyId body = rb->body[nearest];
		body.world0 = uint16_t( m_physicsWorld.index1 - 1 );
		PushBody( body, point, vector, command.mode );
		return;
	}

	if ( const PhysicsBody* pb = e.try_get<PhysicsBody>() )
	{
		PushBody( BodyOf( *pb ), point, vector, command.mode );
	}
}

void Simulation::KillPlayer( flecs::entity e, const SimCommand& command )
{
	Character c = e.get<Character>();
	if ( c.dead )
	{
		return;
	}

	if ( command.mode == 1 )
	{
		flecs::entity body = CreateRagdoll( e, command.other );
		SimCommand hit = command;
		hit.mode = ImpulseVelocity;
		ApplyImpulse( body, hit );
		EnforceRagdollCap( command.value > 0 ? uint32_t( command.value ) : 0 );
	}

	c.dead = 1;
	c.velocity = { 0.0f, 0.0f, 0.0f };
	c.pogoVelocity = 0.0f;
	c.grounded = 0;
	c.sprinting = 0;
	c.airTicks = 0;
	c.groundTicks = 0;
	c.stepDistance = 0.0f;
	e.set<Character>( c );
	e.set<Velocity>( {} );
	// Out of the broadphase: nothing collides with it or hits it with a ray until it respawns.
	b3Body_Disable( BodyOf( e.get<PhysicsBody>() ) );
}

void Simulation::RespawnPlayer( flecs::entity e, const SimCommand& command )
{
	Character c = e.get<Character>();
	b3BodyId body = BodyOf( e.get<PhysicsBody>() );
	if ( c.dead )
	{
		c.dead = 0;
		e.set<Character>( c );
		b3Body_Enable( body );
	}
	if ( command.mode == 1 )
	{
		PlaceCharacter( e, ToVec( command.a ), detmath::YawToRadians( command.index ) );
	}
	else
	{
		PlaceCharacter( e, SpawnPoint( c.slot ), 0.0f );
	}
}

flecs::entity Simulation::CreateRagdoll( flecs::entity player, uint32_t lifetimeTicks )
{
	using namespace ragdoll;

	const Transform t = player.get<Transform>();
	const Character c = player.get<Character>();
	b3Quat facing = detmath::YawRotation( c.facingYaw );
	b3Vec3 feet = b3Sub( t.position, b3Vec3{ 0.0f, kFeetBelowCenter, 0.0f } );

	RagdollBodies bodies;
	b3BodyId live[PartCount];
	for ( int i = 0; i < PartCount; ++i )
	{
		const PartDef& part = kParts[i];
		b3BodyDef def = b3DefaultBodyDef();
		def.type = b3_dynamicBody;
		def.position = b3Add( feet, b3RotateVector( facing, part.center ) );
		def.rotation = facing;
		def.linearVelocity = c.velocity;
		def.linearDamping = kLinearDamping;
		def.angularDamping = kAngularDamping;
		live[i] = b3CreateBody( m_physicsWorld, &def );

		Shape shape{ part.shape, {}, part.size };
		ShapeMaterial material{ kDensity, ragdoll::kFriction, 0.0f };
		b3ShapeId shapeId = CreateShape( live[i], shape, CatRagdoll, material );

		PhysicsBody stored = MakePhysicsBody( live[i], shapeId );
		bodies.body[i] = stored.body;
		bodies.shape[i] = stored.shape;
	}

	// Joint frames point their z axis along the bone (spherical joints: the cone and twist axis)
	// or along the character's X (hinges: the axis a knee or elbow turns about). Every part starts
	// with the same orientation, so one frame rotation serves both sides of a joint.
	const b3Vec3 xAxis = { 1.0f, 0.0f, 0.0f };
	const b3Vec3 yAxis = { 0.0f, 1.0f, 0.0f };
	for ( int i = 0; i < PartCount; ++i )
	{
		const PartDef& part = kParts[i];
		if ( part.parent < 0 )
		{
			continue;
		}
		const PartDef& parent = kParts[part.parent];
		b3Transform frameA = { b3Sub( part.anchor, parent.center ), b3Quat_identity };
		b3Transform frameB = { b3Sub( part.anchor, part.center ), b3Quat_identity };

		if ( part.joint == JointKind::Hinge )
		{
			b3Quat q = b3MakeQuatFromAxisAngle( yAxis, 0.5f * detmath::kPi );
			frameA.q = q;
			frameB.q = q;
			b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
			def.base.bodyIdA = live[part.parent];
			def.base.bodyIdB = live[i];
			def.base.localFrameA = frameA;
			def.base.localFrameB = frameB;
			def.enableLimit = true;
			def.lowerAngle = part.lower;
			def.upperAngle = part.upper;
			b3CreateRevoluteJoint( m_physicsWorld, &def );
		}
		else
		{
			b3Quat q = b3MakeQuatFromAxisAngle( xAxis, -part.direction * 0.5f * detmath::kPi );
			frameA.q = q;
			frameB.q = q;
			b3SphericalJointDef def = b3DefaultSphericalJointDef();
			def.base.bodyIdA = live[part.parent];
			def.base.bodyIdB = live[i];
			def.base.localFrameA = frameA;
			def.base.localFrameB = frameB;
			def.enableConeLimit = true;
			def.coneAngle = part.cone;
			def.enableTwistLimit = true;
			def.lowerTwistAngle = part.lower;
			def.upperTwistAngle = part.upper;
			b3CreateSphericalJoint( m_physicsWorld, &def );
		}
	}

	RagdollPose pose;
	for ( int i = 0; i < PartCount; ++i )
	{
		pose.part[i] = { b3Add( feet, b3RotateVector( facing, kParts[i].center ) ), facing };
		pose.linear[i] = c.velocity;
	}

	flecs::entity e = CreateEntity();
	Ragdoll r;
	r.owner = player.get<NetId>().value;
	r.slot = c.slot;
	r.spawnTick = m_globals.tick;
	r.despawnTick = lifetimeTicks > 0 ? m_globals.tick + lifetimeTicks : 0;
	r.yaw = c.facingYaw;
	e.set<Ragdoll>( r );
	e.set<RagdollBodies>( bodies );
	e.set<RagdollPose>( pose );
	e.set<Transform>( pose.part[Pelvis] );
	return e;
}

void Simulation::EnforceRagdollCap( uint32_t cap )
{
	if ( cap == 0 )
	{
		return;
	}
	uint32_t count = 0;
	for ( const EntityRef& r : m_entities )
	{
		count += flecs::entity( m_world, r.entity ).has<Ragdoll>() ? 1 : 0;
	}
	// NetId order is creation order, so the oldest go first.
	m_scratch.clear();
	for ( const EntityRef& r : m_entities )
	{
		if ( count <= cap )
		{
			break;
		}
		if ( flecs::entity( m_world, r.entity ).has<Ragdoll>() )
		{
			m_scratch.push_back( r );
			count -= 1;
		}
	}
	for ( const EntityRef& r : m_scratch )
	{
		DestroyEntity( flecs::entity( m_world, r.entity ) );
	}
}

// --- Queries ---------------------------------------------------------------------------------------

void Simulation::BuildShapeLookup()
{
	m_shapeLookup.clear();
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		if ( const PhysicsBody* pb = e.try_get<PhysicsBody>() )
		{
			m_shapeLookup.push_back( { uint32_t( pb->shape.index1 ), r.netId } );
		}
		if ( const RagdollBodies* rb = e.try_get<RagdollBodies>() )
		{
			for ( const b3ShapeId& shape : rb->shape )
			{
				m_shapeLookup.push_back( { uint32_t( shape.index1 ), r.netId } );
			}
		}
	}
	std::sort( m_shapeLookup.begin(), m_shapeLookup.end() );
}

uint32_t Simulation::NetIdOfShape( b3ShapeId shape ) const
{
	uint32_t index = uint32_t( shape.index1 );
	auto it = std::lower_bound( m_shapeLookup.begin(), m_shapeLookup.end(), std::make_pair( index, uint32_t( 0 ) ) );
	return ( it != m_shapeLookup.end() && it->first == index ) ? it->second : 0;
}

const Character* Simulation::PlayerCharacter( PlayerSlot slot ) const
{
	if ( slot >= kMaxPlayers || m_globals.playerNetIds[slot] == 0 )
	{
		return nullptr;
	}
	flecs::entity e = FindEntity( m_globals.playerNetIds[slot] );
	return e.is_valid() ? e.try_get<Character>() : nullptr;
}

const Transform* Simulation::EntityTransform( uint32_t netId ) const
{
	flecs::entity e = FindEntity( netId );
	return e.is_valid() ? e.try_get<Transform>() : nullptr;
}

const AnimState* Simulation::EntityAnimState( uint32_t netId ) const
{
	flecs::entity e = FindEntity( netId );
	return e.is_valid() ? e.try_get<AnimState>() : nullptr;
}

int32_t Simulation::BoardValue( uint32_t netId, int slot ) const
{
	if ( slot < 0 || slot >= kBoardSlots )
	{
		return 0;
	}
	flecs::entity e = FindEntity( netId );
	const Blackboard* board = e.is_valid() ? e.try_get<Blackboard>() : nullptr;
	return board != nullptr ? board->values[slot] : 0;
}

namespace
{

struct RayContext
{
	const Simulation* sim;
	uint32_t ignore;
	RayHit* hit;
	bool found;
	bool skipPlayers;
};

} // namespace

// Friend-free access to the lookup: the callback only needs NetIdOfShape.
float Simulation::RayCallback( b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t, int, int, void* context )
{
	auto* ctx = static_cast<RayContext*>( context );
	uint32_t netId = ctx->sim->NetIdOfShape( shapeId );
	if ( netId != 0 && netId == ctx->ignore )
	{
		return -1.0f; // filter: carry on as if it were not there
	}
	if ( ctx->skipPlayers && netId != 0 )
	{
		for ( uint32_t player : ctx->sim->m_globals.playerNetIds )
		{
			if ( player == netId )
			{
				return -1.0f;
			}
		}
	}
	if ( ctx->found == false || fraction < ctx->hit->fraction )
	{
		ctx->hit->netId = netId;
		ctx->hit->point = point;
		ctx->hit->normal = normal;
		ctx->hit->fraction = fraction;
		ctx->found = true;
	}
	return fraction; // clip: only closer hits from here on
}

bool Simulation::CastRay( b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit, bool skipPlayers )
{
	PhysicsArena::Scope scope( *m_arena );
	BuildShapeLookup();
	hit = RayHit{};
	RayContext ctx{ this, ignoreNetId, &hit, false, skipPlayers };
	b3QueryFilter filter = { ~uint64_t( 0 ), ~uint64_t( 0 ), 0, nullptr };
	b3World_CastRay( m_physicsWorld, origin, translation, filter, &Simulation::RayCallback, &ctx );
	return ctx.found;
}

} // namespace cb
