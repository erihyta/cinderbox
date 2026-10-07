#include "simulation.h"

#include "anim_controller.h"
#include "anim_graph.h"
#include "box3d_shim.h"
#include "detmath.h"
#include "level.h"
#include "motions.h"
#include "mover.h"
#include "ragdoll.h"
#include "util.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cb
{

namespace
{

using mover::kCapsuleHalfHeight;
using mover::kCapsuleRadius;

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

// How commands carry a rotation: a unit quaternion's x, y, z, with w >= 0 following from them.
// Anything that is not one (NaN, too long) becomes the nearest sane rotation.
b3Quat CommandRotation( const Float3& f )
{
	float xyz = f.x * f.x + f.y * f.y + f.z * f.z;
	if ( ( xyz >= 0.0f ) == false || xyz > 1e6f )
	{
		return b3Quat{ { 0.0f, 0.0f, 0.0f }, 1.0f };
	}
	if ( xyz > 1.0f )
	{
		float scale = 1.0f / std::sqrt( xyz );
		return b3Quat{ { f.x * scale, f.y * scale, f.z * scale }, 0.0f };
	}
	return b3Quat{ { f.x, f.y, f.z }, std::sqrt( 1.0f - xyz ) };
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
	RegisterSnapComponent<HeldItem>();
	RegisterSnapComponent<MoveOverrides>();
	RegisterSnapComponent<MotionState>();
	RegisterSnapComponent<MotionHold>();
	RegisterSnapComponent<Slots>();

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
void Simulation::PutItemInWorld( flecs::entity item, b3Vec3 grip, b3Quat rotation, b3Vec3 velocity )
{
	HeldItem held = item.get<HeldItem>();
	ItemShape look = ItemShapeOf( held.kind );
	// The body sits at the shape's centre, turned as the shape says; the item's frame (its grip) is
	// `center` away from it.
	Shape shape;
	shape.kind = look.kind == 1 ? ShapeKind::Sphere : ShapeKind::Box;
	shape.halfExtents = { look.half.x, look.half.y, look.half.z };
	b3Vec3 position = b3Add( grip, b3RotateVector( rotation, b3Vec3{ look.center.x, look.center.y, look.center.z } ) );
	float volume = shape.kind == ShapeKind::Sphere ? 4.18879f * look.half.x * look.half.x * look.half.x
												   : 8.0f * look.half.x * look.half.y * look.half.z;

	b3BodyDef bodyDef = b3DefaultBodyDef();
	bodyDef.type = b3_dynamicBody;
	const b3Quat turn = { { look.turn[0], look.turn[1], look.turn[2] }, look.turn[3] };
	const b3Quat bodyRotation = b3NormalizeQuat( b3MulQuat( rotation, turn ) );
	bodyDef.position = position;
	bodyDef.rotation = bodyRotation;
	bodyDef.linearVelocity = velocity;
	b3BodyId body = b3CreateBody( m_physicsWorld, &bodyDef );
	ShapeMaterial material;
	material.density = look.mass / std::max( volume, 1e-6f );
	b3ShapeId shapeId = CreateShape( body, shape, CatProp, material );

	held.holder = 0;
	held.socket = 0;
	held.stowed = 0;
	held.slot = kNoSlot;
	item.set<HeldItem>( held );
	item.set<Transform>( { position, bodyRotation } );
	item.set<Velocity>( { velocity, { 0.0f, 0.0f, 0.0f } } );
	item.set<Shape>( shape );
	item.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
}

void Simulation::TakeItemFromWorld( flecs::entity item, uint32_t holder, uint8_t socket )
{
	if ( const PhysicsBody* pb = item.try_get<PhysicsBody>() )
	{
		b3DestroyBody( BodyOf( *pb ) );
	}
	item.remove<PhysicsBody>();
	item.remove<Shape>();
	item.remove<Velocity>();
	HeldItem held = item.get<HeldItem>();
	held.holder = holder;
	held.socket = socket;
	item.set<HeldItem>( held );
}

// --- Slots ------------------------------------------------------------------------------------------

uint8_t Simulation::SelectedSlot( uint32_t player ) const
{
	flecs::entity e = FindEntity( player );
	const Slots* slots = e.is_valid() ? e.try_get<Slots>() : nullptr;
	return slots != nullptr ? slots->selected : kNoSlot;
}

uint32_t Simulation::SlotItemOf( uint32_t holder, uint8_t slot ) const
{
	if ( holder == 0 || slot == kNoSlot )
	{
		return 0;
	}
	for ( const EntityRef& r : m_entities )
	{
		const HeldItem* item = flecs::entity( m_world, r.entity ).try_get<HeldItem>();
		if ( item != nullptr && item->holder == holder && item->slot == slot )
		{
			return r.netId;
		}
	}
	return 0;
}

void Simulation::SettleSlots( uint32_t holder, uint8_t selected )
{
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		const HeldItem* item = e.try_get<HeldItem>();
		if ( item == nullptr || item->holder != holder || item->slot == kNoSlot )
		{
			continue;
		}
		HeldItem want = *item;
		want.stowed = item->slot == selected ? 0 : 1;
		want.socket = want.stowed != 0 ? ItemShapeOf( item->kind ).holster : m_config.slotHand;
		if ( want.stowed != item->stowed || want.socket != item->socket )
		{
			e.set<HeldItem>( want );
		}
	}
}

void Simulation::ThrowOut( flecs::entity item, const Transform& from, uint16_t cameraYaw )
{
	float yaw = detmath::YawToRadians( cameraYaw );
	b3Vec3 ahead = detmath::YawForward( yaw );
	b3Vec3 grip = b3Add( b3Add( from.position, b3Vec3{ 0.0f, 0.05f, 0.0f } ), b3MulSV( 0.45f, ahead ) );
	PutItemInWorld( item, grip, detmath::YawRotation( yaw ), b3Add( b3MulSV( 1.0f, ahead ), b3Vec3{ 0.0f, 1.5f, 0.0f } ) );
}

bool Simulation::GiveSlot( flecs::entity player, flecs::entity item, bool select )
{
	uint32_t holder = player.get<NetId>().value;
	Slots slots = player.has<Slots>() ? player.get<Slots>() : Slots{ m_config.slots, kNoSlot, 0, 0 };
	HeldItem held = item.get<HeldItem>();
	const ItemShape rule = ItemShapeOf( held.kind );
	int slot = -1;
	if ( rule.slot >= 1 && rule.slot <= slots.count )
	{
		// The kind's own slot: what is there makes room.
		slot = rule.slot - 1;
		if ( uint32_t old = SlotItemOf( holder, uint8_t( slot ) ); old != 0 && FindEntity( old ) != item )
		{
			// To a free slot if there is one, else onto the floor.
			int room = -1;
			for ( int s = 0; s < int( slots.count ) && room < 0; ++s )
			{
				room = s != slot && SlotItemOf( holder, uint8_t( s ) ) == 0 ? s : -1;
			}
			flecs::entity moved = FindEntity( old );
			if ( room >= 0 )
			{
				HeldItem other = moved.get<HeldItem>();
				other.slot = uint8_t( room );
				moved.set<HeldItem>( other );
			}
			else
			{
				ThrowOut( moved, player.get<Transform>(), 0 );
			}
		}
	}
	else
	{
		for ( int s = 0; s < int( slots.count ) && slot < 0; ++s )
		{
			slot = SlotItemOf( holder, uint8_t( s ) ) == 0 ? s : -1;
		}
	}
	if ( slot < 0 )
	{
		return false;
	}
	held.holder = holder;
	held.slot = uint8_t( slot );
	item.set<HeldItem>( held );
	if ( select )
	{
		slots.selected = uint8_t( slot );
	}
	player.set<Slots>( slots );
	SettleSlots( holder, slots.selected );
	return true;
}

void Simulation::StepSlots( const InputFrame& frame )
{
	if ( m_config.slots == 0 )
	{
		return;
	}
	for ( int slot = 0; slot < kMaxPlayers; ++slot )
	{
		uint32_t netId = m_globals.playerNetIds[slot];
		if ( netId == 0 )
		{
			continue;
		}
		flecs::entity e = FindEntity( netId );
		const PlayerInput& in = frame.inputs[slot];
		const bool had = e.has<Slots>();
		Slots slots = had ? e.get<Slots>() : Slots{ m_config.slots, kNoSlot, in.intentSeq, 0 };
		if ( had && in.intentSeq == slots.seq )
		{
			continue;
		}
		// Once per intent: an input repeated while a packet is late, or guessed for someone else,
		// has the same count and does nothing again.
		const bool act = had && e.get<Character>().dead == 0;
		slots.seq = in.intentSeq;
		if ( act && in.intent == uint8_t( SlotIntent::Select ) && ( in.intentA < slots.count || in.intentA == kNoSlot ) )
		{
			// A slot's key again, or no slot at all: empty hands.
			slots.selected = slots.selected == in.intentA ? kNoSlot : in.intentA;
		}
		else if ( act && in.intent == uint8_t( SlotIntent::Move ) && in.intentA < slots.count && in.intentB < slots.count &&
				  in.intentA != in.intentB )
		{
			flecs::entity a = FindEntity( SlotItemOf( netId, in.intentA ) );
			flecs::entity b = FindEntity( SlotItemOf( netId, in.intentB ) );
			for ( const auto& [item, to] : { std::pair{ a, in.intentB }, std::pair{ b, in.intentA } } )
			{
				if ( item.is_valid() )
				{
					HeldItem moved = item.get<HeldItem>();
					moved.slot = to;
					item.set<HeldItem>( moved );
				}
			}
		}
		else if ( act && in.intent == uint8_t( SlotIntent::Drop ) )
		{
			uint8_t which = in.intentA == kNoSlot ? slots.selected : in.intentA;
			if ( flecs::entity item = FindEntity( SlotItemOf( netId, which ) ); which < slots.count && item.is_valid() )
			{
				ThrowOut( item, e.get<Transform>(), in.cameraYaw );
			}
		}
		e.set<Slots>( slots );
		SettleSlots( netId, slots.selected );
	}
}

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
	const AnimState& old = e.get<AnimState>();
	AnimState anim;
	anim.aiming = old.aiming;
	for ( int l = 0; l < kMaxAnimLayers; ++l )
	{
		// The stances stay (the mod still has the weapon out).
		anim.stances[l] = old.stances[l];
	}
	e.set<AnimState>( anim );
	e.set<Transform>( t );
	e.set<Velocity>( {} );
	if ( const MotionHold* hold = e.try_get<MotionHold>(); hold != nullptr && hold->on != 0 )
	{
		MotionHold off = *hold;
		off.on = 0;
		e.set<MotionHold>( off );
	}

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
			// What it carried goes with it, in use or stowed.
			std::vector<flecs::entity> carried;
			for ( const EntityRef& r : m_entities )
			{
				flecs::entity e( m_world, r.entity );
				if ( const HeldItem* item = e.try_get<HeldItem>(); item != nullptr && item->holder == netId )
				{
					carried.push_back( e );
				}
			}
			for ( flecs::entity e : carried )
			{
				DestroyEntity( e );
			}
			flecs::entity e = FindEntity( netId );
			if ( e.is_valid() )
			{
				DestroyEntity( e );
			}
			netId = 0;
		}
	}
}

void Simulation::FollowHolders()
{
	// A held item is where its holder is (presentation puts it in the socket).
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		if ( const HeldItem* item = e.try_get<HeldItem>(); item != nullptr && item->holder != 0 )
		{
			flecs::entity holder = FindEntity( item->holder );
			if ( holder.is_valid() && holder.has<Transform>() )
			{
				e.set<Transform>( holder.get<Transform>() );
			}
		}
	}
}

void Simulation::RecordModEvent( const ModEventRecord& record )
{
	m_globals.modEvents[m_globals.modEventCount % kModEventHistory] = record;
	m_globals.modEventCount += 1;
}

void Simulation::MoveCharacters( const InputFrame& frame )
{
	// What the players ask of their slots, first: everything below asks what is in the hand.
	StepSlots( frame );
	// What everyone holds, for state machines that ask ("attack and melee.bat").
	m_heldScratch.clear();
	if ( m_animGraph || m_motions )
	{
		for ( const EntityRef& r : m_entities )
		{
			flecs::entity e( m_world, r.entity );
			if ( const HeldItem* item = e.try_get<HeldItem>(); item != nullptr && item->holder != 0 && item->stowed == 0 )
			{
				m_heldScratch.push_back( { item->holder, item->kind } );
			}
		}
	}
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
			if ( m_motions )
			{
				MotionState motion = e.has<MotionState>() ? e.get<MotionState>() : MotionState{};
				motion.prevActions = in.actions;
				e.set<MotionState>( motion );
				if ( const MotionHold* hold = e.try_get<MotionHold>(); hold != nullptr && hold->on != 0 )
				{
					MotionHold off = *hold;
					off.on = 0;
					e.set<MotionHold>( off );
				}
			}
			continue;
		}
		Transform t = e.get<Transform>();
		PhysicsBody pb = e.get<PhysicsBody>();

		uint8_t pressed = uint8_t( in.buttons & ~c.prevButtons );

		// What everything that reads the player's items asks for.
		uint16_t held[16];
		uint32_t heldCount = 0;
		for ( const auto& [holder, kind] : m_heldScratch )
		{
			if ( holder == netId && heldCount < 16 )
			{
				held[heldCount++] = kind;
			}
		}

		// The mods' motions (a dash, a double jump), before the mover: on the world as the last tick
		// and this tick's commands left it, by this tick's input. A frozen player does none.
		if ( m_motions )
		{
			MotionState motion = e.has<MotionState>() ? e.get<MotionState>() : MotionState{};
			{
				const AnimState& before = e.get<AnimState>();
				Blackboard board = e.has<Blackboard>() ? e.get<Blackboard>() : Blackboard{};
				// Conditions read the board as the tick found it: a motion that turns a field on does
				// not start the motion that waits for it until the next tick.
				const Blackboard boardBefore = board;
				AnimGraphInputs values;
				values.builtins[AnimExpr::Speed] = before.groundSpeed;
				values.builtins[AnimExpr::ForwardSpeed] = before.legsBackward != 0 ? -before.groundSpeed : before.groundSpeed;
				values.builtins[AnimExpr::VerticalSpeed] = c.velocity.y;
				values.builtins[AnimExpr::Grounded] = c.grounded != 0 ? 1.0f : 0.0f;
				values.builtins[AnimExpr::AirborneTime] = c.grounded != 0 ? 0.0f : float( c.airTicks ) * m_config.TimeStep();
				values.builtins[AnimExpr::Jumped] = c.lastJumpTick != 0 && c.lastJumpTick + 1 == m_globals.tick ? 1.0f : 0.0f;
				values.builtins[AnimExpr::Aiming] = before.aiming != 0 ? 1.0f : 0.0f;
				values.builtins[AnimExpr::Backward] = before.legsBackward != 0 ? 1.0f : 0.0f;
				values.builtins[AnimExpr::MoveForward] = before.moveForward;
				values.builtins[AnimExpr::MoveRight] = before.moveRight;
				values.state = &before;
				values.board = boardBefore.values;
				values.input = &in;
				// A frozen player presses nothing.
				values.pressedActions = c.frozen ? uint16_t( 0 ) : uint16_t( in.actions & ~motion.prevActions );
				values.pressedButtons = c.frozen ? uint8_t( 0 ) : pressed;
				// What a probe of its holds on to, if one is out, and whether that is still there.
				int heldMotion = -1;
				bool holdAlive = false;
				if ( const MotionHold* out = e.try_get<MotionHold>(); out != nullptr && out->on != 0 )
				{
					b3Vec3 where = {};
					heldMotion = out->motion;
					holdAlive = HoldPoint( *out, where );
				}
				values.builtins[AnimExpr::Linked] = heldMotion >= 0 ? 1.0f : 0.0f;
				values.globalBoard = m_globals.board;
				values.events = m_globals.modEvents;
				values.eventCount = m_globals.modEventCount;
				values.tick = m_globals.tick;
				values.netId = netId;
				values.heldKinds = held;
				values.heldCount = heldCount;

				MotionInputs motionIn;
				motionIn.input = &in;
				motionIn.pressedButtons = pressed;
				motionIn.canAct = c.frozen == 0;
				motionIn.netId = netId;
				motionIn.tick = m_globals.tick;
				motionIn.tickRate = m_config.tickRate;
				motionIn.values = &values;
				struct Thrower
				{
					Simulation* sim;
					flecs::entity e;
					const Transform* t;
					const PlayerInput* in;
				} thrower{ this, e, &t, &in };
				motionIn.user = &thrower;
				motionIn.held = heldMotion;
				motionIn.holdAlive = holdAlive;
				motionIn.attach = []( void* user, const Motion& m, size_t index, uint32_t& holdTick ) {
					auto* by = static_cast<Thrower*>( user );
					return by->sim->AttachHold( by->e, *by->t, *by->in, m, index, holdTick );
				};
				bool boardChanged = false;
				m_motionEvents.clear();
				m_motionActive.clear();
				RunMotions( *m_motions, motionIn, motion, c, board, boardChanged, m_motionEvents, m_motionActive );
				if ( boardChanged )
				{
					e.set<Blackboard>( board );
				}
				// A hold whose motion ended (its condition, a new throw) is let go.
				if ( const MotionHold* out = e.try_get<MotionHold>(); out != nullptr && out->on != 0 )
				{
					const MotionSlot& slot = motion.slots[out->motion];
					if ( slot.lastTick == 0 || slot.untilTick <= m_globals.tick )
					{
						MotionHold off = *out;
						off.on = 0;
						e.set<MotionHold>( off );
					}
				}
				// An event of a motion that threw a probe this tick is at where it will hold.
				b3Vec3 eventPoint = t.position;
				if ( const MotionHold* thrown = e.try_get<MotionHold>(); thrown != nullptr && thrown->on != 0 && thrown->startTick == m_globals.tick )
				{
					HoldPoint( *thrown, eventPoint );
				}
				for ( ModEventRecord& record : m_motionEvents )
				{
					record.netIdA = netId;
					record.tick = m_globals.tick;
					record.point = eventPoint;
					RecordModEvent( record );
				}
				// The effects read the state the motions just decided (the parameters that hold).
				e.set<MotionState>( motion );
				ApplyMotionEffects( e, c, t, in, motion, boardBefore, m_motionActive );
			}
		}

		// Frozen: the mover still runs (gravity, the ground, being pushed), with no intent.
		PlayerInput still;
		still.cameraYaw = in.cameraYaw;
		mover::Move( { m_physicsWorld, BodyOf( pb ), ShapeOf( pb ) }, MoveOf( e ), m_config.TimeStep(), m_globals.tick,
					 c.frozen ? still : in, c.frozen ? uint8_t( 0 ) : pressed, c, t );
		c.prevButtons = in.buttons;

		AnimState anim = e.get<AnimState>();
		UpdateAnimState( anim, c, in, m_globals.tick, m_config.TimeStep() );
		if ( m_animGraph )
		{
			// The character's own state machine, on what this tick's movement and commands left.
			Blackboard board = e.has<Blackboard>() ? e.get<Blackboard>() : Blackboard{};
			AnimGraphInputs graphIn;
			float speed = anim.groundSpeed;
			graphIn.builtins[AnimExpr::Speed] = speed;
			graphIn.builtins[AnimExpr::ForwardSpeed] = anim.legsBackward != 0 ? -speed : speed;
			graphIn.builtins[AnimExpr::VerticalSpeed] = c.velocity.y;
			graphIn.builtins[AnimExpr::Grounded] = c.grounded != 0 ? 1.0f : 0.0f;
			graphIn.builtins[AnimExpr::AirborneTime] = c.grounded != 0 ? 0.0f : float( c.airTicks ) * m_config.TimeStep();
			graphIn.builtins[AnimExpr::Jumped] = c.lastJumpTick != 0 && c.lastJumpTick == m_globals.tick ? 1.0f : 0.0f;
			graphIn.builtins[AnimExpr::Aiming] = anim.aiming != 0 ? 1.0f : 0.0f;
			graphIn.builtins[AnimExpr::Backward] = anim.legsBackward != 0 ? 1.0f : 0.0f;
			graphIn.builtins[AnimExpr::MoveForward] = anim.moveForward;
			graphIn.builtins[AnimExpr::MoveRight] = anim.moveRight;
			graphIn.board = board.values;
			graphIn.globalBoard = m_globals.board;
			graphIn.events = m_globals.modEvents;
			graphIn.eventCount = m_globals.modEventCount;
			graphIn.tick = m_globals.tick;
			graphIn.netId = netId;
			graphIn.heldKinds = held;
			graphIn.heldCount = heldCount;
			m_markerScratch.clear();
			UpdateAnimGraph( anim, *m_animGraph, m_animPacks, graphIn, m_config.TimeStep(), m_markerScratch );
			for ( int event : m_markerScratch )
			{
				// A marker the playing clip crossed: the mod event of its name, from this player.
				ModEventRecord record;
				record.type = uint16_t( event );
				record.netIdA = netId;
				record.tick = m_globals.tick;
				record.point = t.position;
				RecordModEvent( record );
			}
		}
		e.set<AnimState>( anim );

		e.set<Character>( c );
		e.set<Transform>( t );
		e.set<Velocity>( { c.velocity, { 0.0f, 0.0f, 0.0f } } );
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
	FollowHolders();
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
	if ( target & kItemTargetBit )
	{
		uint32_t slot = target & 0xFFu;
		uint32_t socket = ( target >> 8 ) & 0xFFu;
		uint32_t holder = slot < uint32_t( kMaxPlayers ) ? m_globals.playerNetIds[slot] : 0;
		return holder != 0 ? HeldItemOf( holder, socket ) : 0;
	}
	return target;
}

uint32_t Simulation::HeldItemOf( uint32_t holder, uint32_t socket ) const
{
	if ( holder == 0 )
	{
		return 0; // items in the world are held by no one
	}
	// NetId order: the same answer everywhere.
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		if ( const HeldItem* item = e.try_get<HeldItem>() )
		{
			if ( item->holder == holder && item->socket == socket && item->stowed == 0 )
			{
				return r.netId;
			}
		}
	}
	return 0;
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
			RecordModEvent( record );
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

		case CommandType::SpawnItem:
		{
			uint32_t holder = ResolveTarget( command.target );
			if ( command.target == 0 )
			{
				// Straight into the world.
				flecs::entity item = CreateEntity();
				item.set<HeldItem>( { 0, command.index, 0, 0 } );
				PutItemInWorld( item, ToVec( command.a ), CommandRotation( command.c ), ToVec( command.b ) );
				return;
			}
			flecs::entity player = FindEntity( holder );
			if ( holder == 0 || player.is_valid() == false || player.has<Character>() == false )
			{
				return;
			}
			if ( m_config.slots > 0 )
			{
				// Into a slot (its kind's own, or the first free one); with none, onto the floor.
				flecs::entity item = CreateEntity();
				item.set<HeldItem>( { 0, command.index, kNoSocket, 1 } );
				item.set<Transform>( player.get<Transform>() );
				if ( GiveSlot( player, item, false ) == false )
				{
					ThrowOut( item, player.get<Transform>(), 0 );
				}
				return;
			}
			if ( command.value == 1 )
			{
				flecs::entity item = CreateEntity();
				item.set<HeldItem>( { holder, command.index, command.mode, 1 } );
				item.set<Transform>( player.get<Transform>() );
				return;
			}
			if ( uint32_t old = HeldItemOf( holder, command.mode ) )
			{
				// Dropped, not destroyed: it may be one someone picked up.
				const Transform& at = player.get<Transform>();
				PutItemInWorld( FindEntity( old ), b3Add( at.position, b3Vec3{ 0.0f, 0.4f, 0.0f } ), at.rotation, b3Vec3{ 0.0f, 0.0f, 0.0f } );
			}
			flecs::entity item = CreateEntity();
			item.set<HeldItem>( { holder, command.index, command.mode, 0 } );
			item.set<Transform>( player.get<Transform>() );
			return;
		}

		case CommandType::DropItem:
		{
			flecs::entity item = FindEntity( ResolveTarget( command.target ) );
			const HeldItem* held = item.is_valid() ? item.try_get<HeldItem>() : nullptr;
			if ( held == nullptr || held->holder == 0 )
			{
				return;
			}
			PutItemInWorld( item, ToVec( command.a ), CommandRotation( command.c ), ToVec( command.b ) );
			return;
		}

		case CommandType::PickUpItem:
		{
			uint32_t holder = ResolveTarget( command.target );
			flecs::entity player = FindEntity( holder );
			flecs::entity item = FindEntity( ResolveTarget( command.other ) );
			const HeldItem* held = item.is_valid() ? item.try_get<HeldItem>() : nullptr;
			bool stowed = command.value == 1;
			if ( holder != 0 && player.is_valid() && player.has<Character>() && held != nullptr && held->holder == 0 && m_config.slots > 0 )
			{
				// Into a slot, and out into the hand; with no slot for it, it stays where it lies.
				const ItemShape rule = ItemShapeOf( held->kind );
				bool room = rule.slot >= 1 && rule.slot <= m_config.slots;
				for ( uint8_t s = 0; s < m_config.slots && room == false; ++s )
				{
					room = SlotItemOf( holder, s ) == 0;
				}
				if ( room )
				{
					TakeItemFromWorld( item, holder, kNoSocket );
					item.set<Transform>( player.get<Transform>() );
					GiveSlot( player, item, true );
				}
				return;
			}
			if ( holder == 0 || player.is_valid() == false || player.has<Character>() == false || held == nullptr ||
				 held->holder != 0 || ( stowed == false && HeldItemOf( holder, command.mode ) != 0 ) )
			{
				return;
			}
			TakeItemFromWorld( item, holder, command.mode );
			if ( stowed )
			{
				HeldItem taken = item.get<HeldItem>();
				taken.stowed = 1;
				item.set<HeldItem>( taken );
			}
			item.set<Transform>( player.get<Transform>() );
			return;
		}

		case CommandType::MoveItem:
		{
			flecs::entity item = FindEntity( ResolveTarget( command.target ) );
			const HeldItem* held = item.is_valid() ? item.try_get<HeldItem>() : nullptr;
			if ( held == nullptr || held->holder == 0 )
			{
				return;
			}
			bool stowed = command.value == 1;
			if ( stowed == false )
			{
				uint32_t there = HeldItemOf( held->holder, command.mode );
				if ( there != 0 && FindEntity( there ) != item )
				{
					return;
				}
			}
			HeldItem moved = *held;
			moved.socket = command.mode;
			moved.stowed = stowed ? 1 : 0;
			item.set<HeldItem>( moved );
			return;
		}

		case CommandType::SetMove:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() == false || e.has<Character>() == false || command.index >= kMoveParams )
			{
				return;
			}
			MoveOverrides set = e.has<MoveOverrides>() ? e.get<MoveOverrides>() : MoveOverrides{};
			uint32_t bit = uint32_t( 1 ) << command.index;
			if ( command.mode == 1 )
			{
				set.mask |= bit;
				set.values[command.index] = ClampMoveParam( command.index, command.a.x );
			}
			else
			{
				set.mask &= ~bit;
				set.values[command.index] = 0.0f;
			}
			e.set<MoveOverrides>( set );
			return;
		}

		case CommandType::SwapLayer:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() == false || e.has<AnimState>() == false || command.index >= kMaxAnimLayers || command.value < 0 ||
				 command.value > 255 )
			{
				return;
			}
			AnimState a = e.get<AnimState>();
			AnimGraphLayerState& layer = a.graph[command.index];
			if ( layer.source != uint8_t( command.value ) )
			{
				layer.source = uint8_t( command.value );
				layer.started = 0; // starts over in the new layer's start state
			}
			e.set<AnimState>( a );
			return;
		}

		case CommandType::Stance:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() == false || e.has<AnimState>() == false || command.index >= kMaxAnimLayers ||
				 command.value < 0 || command.value > kMaxStances )
			{
				return;
			}
			AnimState a = e.get<AnimState>();
			a.stances[command.index] = uint8_t( command.value );
			e.set<AnimState>( a );
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

	// The body weighs what the player did (its mass, a movement parameter): the parts share it by
	// their volume.
	float volume = 0.0f;
	for ( int i = 0; i < PartCount; ++i )
	{
		const b3Vec3 s = kParts[i].size;
		switch ( kParts[i].shape )
		{
			case ShapeKind::Box:
				volume += 8.0f * s.x * s.y * s.z;
				break;
			case ShapeKind::Sphere:
				volume += ( 4.0f / 3.0f ) * detmath::kPi * s.x * s.x * s.x;
				break;
			case ShapeKind::Capsule:
				volume += detmath::kPi * s.x * s.x * ( 2.0f * s.y + ( 4.0f / 3.0f ) * s.x );
				break;
		}
	}
	const float density = MoveOf( player )[MoveParam::Mass] / volume;

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
		ShapeMaterial material{ density, ragdoll::kFriction, 0.0f };
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

MoveParams Simulation::MoveOf( flecs::entity e ) const
{
	MoveParams params = m_config.move;
	if ( const MoveOverrides* set = e.try_get<MoveOverrides>() )
	{
		for ( int i = 0; i < kMoveParams; ++i )
		{
			if ( set->mask & ( uint32_t( 1 ) << i ) )
			{
				params.values[i] = set->values[i];
			}
		}
	}
	// A motion that is on has the last word, for as long as it lasts.
	if ( m_motions )
	{
		if ( const MotionState* motion = e.try_get<MotionState>() )
		{
			ApplyMotionParams( *m_motions, *motion, m_globals.tick, params );
		}
	}
	return params;
}

// --- Motions: holds and effects ---------------------------------------------------------------------

namespace
{

// A rope never reels in shorter than this, and nothing nearer than this is worth a throw.
constexpr float kHoldMinLength = 1.0f;
// Past a rope's end the two are brought back together at this rate (1/s of the excess), up to a speed.
constexpr float kRopeStiffness = 10.0f;
constexpr float kRopeMaxSpeed = 20.0f;

} // namespace

bool Simulation::HoldPoint( const MotionHold& hold, b3Vec3& point ) const
{
	if ( hold.anchor == 0 )
	{
		point = hold.point;
		return true;
	}
	flecs::entity a = FindEntity( hold.anchor );
	if ( a.is_valid() == false )
	{
		return false;
	}
	if ( const Character* other = a.try_get<Character>() )
	{
		if ( other->dead != 0 )
		{
			return false;
		}
		point = b3Add( a.get<Transform>().position, hold.point );
		return true;
	}
	if ( const PhysicsBody* pb = a.try_get<PhysicsBody>() )
	{
		point = b3Body_GetWorldPoint( BodyOf( *pb ), hold.point );
		return true;
	}
	return false;
}

bool Simulation::AttachHold( flecs::entity e, const Transform& t, const PlayerInput& in, const Motion& m, size_t index, uint32_t& holdTick )
{
	uint32_t self = e.get<NetId>().value;
	float yaw = detmath::YawToRadians( in.cameraYaw );
	b3CosSin pitch = detmath::CosSin( float( in.cameraPitch ) * ( detmath::kTwoPi / 65536.0f ) );
	b3Vec3 forward = detmath::YawForward( yaw );
	b3Vec3 look = { forward.x * pitch.cosine, pitch.sine, forward.z * pitch.cosine };

	// What is under the crosshair: along the camera's line from where it passes the player (the
	// point a third-person camera orbits, moved to a shoulder if the camera is). Then the line itself
	// goes from the player to that point, so something in between stops it.
	b3Vec3 from = b3Add( t.position, b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } );
	b3Vec3 view = from;
	if ( ViewMode( in.view ) == ViewMode::ShoulderRight || ViewMode( in.view ) == ViewMode::ShoulderLeft )
	{
		view = b3MulAdd( view, ViewMode( in.view ) == ViewMode::ShoulderRight ? kShoulderOffset : -kShoulderOffset, detmath::YawRight( yaw ) );
	}
	RayHit seen;
	if ( CastRay( view, b3MulSV( m.probeRange, look ), self, seen ) == false )
	{
		return false;
	}
	RayHit hit = seen;
	b3Vec3 to = b3Sub( seen.point, from );
	RayHit nearer;
	if ( CastRay( from, b3MulSV( 1.02f, to ), self, nearer ) )
	{
		hit = nearer;
	}
	float distance = b3Distance( hit.point, from );
	if ( distance < kHoldMinLength )
	{
		return false;
	}

	MotionHold hold;
	hold.point = hit.point;
	flecs::entity a = FindEntity( hit.netId );
	if ( a.is_valid() && a.has<Character>() )
	{
		hold.anchor = hit.netId;
		hold.point = b3Sub( hit.point, a.get<Transform>().position );
	}
	else if ( a.is_valid() && a.has<PhysicsBody>() && a.has<StaticGeometry>() == false && a.has<RagdollBodies>() == false )
	{
		b3BodyId body = BodyOf( a.get<PhysicsBody>() );
		if ( b3Body_GetType( body ) == b3_dynamicBody )
		{
			hold.anchor = hit.netId;
			hold.point = b3Body_GetLocalPoint( body, hit.point );
		}
	}
	hold.length = 0.0f; // a link measures its rope when the probe takes hold
	hold.startTick = m_globals.tick;
	hold.holdTick = m_globals.tick + ( m.probeTravel > 0.0f ? uint32_t( distance / m.probeTravel * float( m_config.tickRate ) + 0.5f ) : 0u );
	hold.motion = uint8_t( index );
	hold.on = 1;
	e.set<MotionHold>( hold );
	holdTick = hold.holdTick;
	return true;
}

// One end of an effect: who it acts on, and how to move it.
struct Simulation::MotionEnd
{
	enum class Kind
	{
		None,	// nobody (a field that names nothing, a probe that found the world)
		World,	// a point that does not move
		Self,	// the player the motion runs for: `c`, not yet stored
		Player, // another player
		Body,	// a dynamic body
	};
	Kind kind = Kind::None;
	flecs::entity entity;
	b3BodyId body = {};
	b3Vec3 point = {}; // where it is (a player's chest, the point on a body)
	float mass = 0.0f; // 0: it does not move
};

Simulation::MotionEnd Simulation::ResolveEnd( flecs::entity self, const Transform& t, const MotionTarget& target, const MotionHold* hold,
											   const Blackboard& board ) const
{
	MotionEnd end;
	auto player = [&]( flecs::entity who, MotionEnd::Kind kind ) {
		end.kind = kind;
		end.entity = who;
		end.point = b3Add( kind == MotionEnd::Kind::Self ? t.position : who.get<Transform>().position, b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } );
		end.mass = MoveOf( who )[MoveParam::Mass];
	};
	auto entity = [&]( flecs::entity who, const b3Vec3* at ) {
		if ( who.is_valid() == false )
		{
			return;
		}
		if ( who == self )
		{
			player( who, MotionEnd::Kind::Self );
		}
		else if ( const Character* other = who.try_get<Character>() )
		{
			if ( other->dead == 0 )
			{
				player( who, MotionEnd::Kind::Player );
				end.point = at != nullptr ? *at : end.point;
			}
		}
		else if ( const PhysicsBody* pb = who.try_get<PhysicsBody>(); pb != nullptr && who.has<RagdollBodies>() == false )
		{
			b3BodyId body = BodyOf( *pb );
			if ( b3Body_GetType( body ) == b3_dynamicBody )
			{
				end.kind = MotionEnd::Kind::Body;
				end.entity = who;
				end.body = body;
				end.point = at != nullptr ? *at : b3Body_GetWorldCenter( body );
				end.mass = b3Body_GetMass( body );
			}
		}
	};
	switch ( target.kind )
	{
		case MotionTarget::Kind::Self:
			player( self, MotionEnd::Kind::Self );
			break;
		case MotionTarget::Kind::Hit:
		{
			b3Vec3 at = {};
			if ( hold == nullptr || HoldPoint( *hold, at ) == false )
			{
				break;
			}
			if ( hold->anchor == 0 )
			{
				end.kind = MotionEnd::Kind::World;
				end.point = at;
				break;
			}
			entity( FindEntity( hold->anchor ), &at );
			break;
		}
		case MotionTarget::Kind::Field:
			if ( target.known )
			{
				entity( FindEntity( uint32_t( board.values[target.slot] ) ), nullptr );
			}
			break;
	}
	return end;
}

b3Vec3 Simulation::EndVelocity( const MotionEnd& end, const Character& c ) const
{
	switch ( end.kind )
	{
		case MotionEnd::Kind::Self:
			return c.velocity;
		case MotionEnd::Kind::Player:
			return end.entity.get<Character>().velocity;
		case MotionEnd::Kind::Body:
			return b3Body_GetWorldPointVelocity( end.body, end.point );
		default:
			return { 0.0f, 0.0f, 0.0f };
	}
}

void Simulation::PushEnd( const MotionEnd& end, Character& c, b3Vec3 change )
{
	auto pushed = []( Character& who, b3Vec3 by ) {
		who.velocity = b3Add( who.velocity, by );
		if ( by.y > 0.0f && who.velocity.y > 0.0f )
		{
			who.grounded = 0; // lifted off, as a jump is
		}
	};
	switch ( end.kind )
	{
		case MotionEnd::Kind::Self:
			pushed( c, change );
			break;
		case MotionEnd::Kind::Player:
		{
			// A player with a lower slot has moved already this tick: it feels this on its next.
			Character other = end.entity.get<Character>();
			pushed( other, change );
			end.entity.set<Character>( other );
			break;
		}
		case MotionEnd::Kind::Body:
			b3Body_ApplyLinearImpulse( end.body, b3MulSV( end.mass, change ), end.point, true );
			break;
		default:
			break;
	}
}

void Simulation::ApplyMotionEffects( flecs::entity e, Character& c, const Transform& t, const PlayerInput& in, const MotionState& state,
									 const Blackboard& board, const std::vector<MotionActive>& active )
{
	const float dt = m_config.TimeStep();
	for ( const MotionActive& on : active )
	{
		const Motion& m = m_motions->list[on.index];
		const MotionSlot& slot = state.slots[on.index];
		// What the motion's probe holds on to: its effects wait for it to take hold.
		const MotionHold* found = e.try_get<MotionHold>();
		MotionHold hold = found != nullptr ? *found : MotionHold{};
		const bool holds = hold.on != 0 && hold.motion == on.index;
		if ( m.probe && ( holds == false || m_globals.tick < hold.holdTick ) )
		{
			continue;
		}
		bool holdChanged = false;
		const MotionEnd self = ResolveEnd( e, t, MotionTarget{}, nullptr, board );
		const MotionTarget hitTarget{ MotionTarget::Kind::Hit, 0, true };
		const MotionEnd hit = m.probe ? ResolveEnd( e, t, hitTarget, &hold, board ) : MotionEnd{};

		for ( const MotionEffect& effect : m.effects )
		{
			const MotionEnd target = ResolveEnd( e, t, effect.target, holds ? &hold : nullptr, board );
			if ( target.kind == MotionEnd::Kind::None )
			{
				continue;
			}
			// The direction is the player's own, whoever the effect acts on; "to" is toward the target,
			// or for an effect on the player itself, toward what its probe found.
			b3Vec3 direction = {};
			if ( effect.frame == MotionFrame::To )
			{
				const MotionEnd& toward = target.kind == MotionEnd::Kind::Self ? hit : target;
				float distance = 0.0f;
				direction = b3GetLengthAndNormalize( &distance, b3Sub( toward.point, self.point ) );
				if ( toward.kind == MotionEnd::Kind::None || distance < 0.001f )
				{
					continue;
				}
			}
			else
			{
				direction = MotionDirection( effect.frame, effect.direction, c, in );
			}

			if ( effect.kind == MotionEffect::Kind::Impulse )
			{
				if ( on.started == false || target.kind == MotionEnd::Kind::World )
				{
					continue;
				}
				// What it replaces is taken away first: a jump in the air is the same jump.
				b3Vec3 had = EndVelocity( target, c );
				b3Vec3 keep = had;
				switch ( effect.replace )
				{
					case MotionEffect::Replace::None:
						break;
					case MotionEffect::Replace::Vertical:
						keep.y = 0.0f;
						break;
					case MotionEffect::Replace::Horizontal:
						keep.x = 0.0f;
						keep.z = 0.0f;
						break;
					case MotionEffect::Replace::All:
						keep = { 0.0f, 0.0f, 0.0f };
						break;
				}
				PushEnd( target, c, b3Add( b3Sub( keep, had ), b3MulSV( effect.strength, direction ) ) );
			}
			else if ( effect.kind == MotionEffect::Kind::Force )
			{
				if ( target.kind == MotionEnd::Kind::World || target.mass <= 0.0f )
				{
					continue;
				}
				const float ramp = MotionRamp( effect, m_globals.tick - slot.sinceTick, m_config.tickRate );
				const float along = b3Dot( EndVelocity( target, c ), direction );
				float change = 0.0f;
				switch ( effect.push )
				{
					case MotionEffect::Push::Acceleration:
						change = effect.strength * ramp * dt;
						break;
					case MotionEffect::Push::Force:
						change = effect.strength * ramp / target.mass * dt;
						break;
					case MotionEffect::Push::Velocity:
					{
						// Toward the speed, no faster than its rate lets it.
						float rate = std::fabs( effect.strength ) * ramp * dt;
						change = std::clamp( effect.speed - along, -rate, rate );
						break;
					}
				}
				if ( effect.push != MotionEffect::Push::Velocity && effect.speed > 0.0f )
				{
					// A top speed along the push: past it, it pushes no more.
					float room = change >= 0.0f ? effect.speed - along : effect.speed + along;
					change = room <= 0.0f ? 0.0f : ( change >= 0.0f ? std::min( change, room ) : std::max( change, -room ) );
				}
				if ( change == 0.0f )
				{
					continue;
				}
				PushEnd( target, c, b3MulSV( change, direction ) );
				if ( effect.react )
				{
					// The other end takes the same momentum the other way: the player, when the force
					// is on something else; what the probe found, when it is on the player.
					const MotionEnd& other = target.kind == MotionEnd::Kind::Self ? hit : self;
					if ( other.mass > 0.0f && other.kind != MotionEnd::Kind::None && other.kind != MotionEnd::Kind::World )
					{
						PushEnd( other, c, b3MulSV( -change * target.mass / other.mass, direction ) );
					}
				}
			}
			else if ( effect.kind == MotionEffect::Kind::Link )
			{
				// A rope between the player and the target. Its length is the effect's, or the distance
				// when the probe took hold; reeling shortens it.
				float distance = 0.0f;
				b3Vec3 toward = b3GetLengthAndNormalize( &distance, b3Sub( target.point, self.point ) );
				float length = effect.length;
				if ( length <= 0.0f )
				{
					if ( hold.length <= 0.0f )
					{
						hold.length = std::max( distance, kHoldMinLength );
					}
					hold.length = std::max( kHoldMinLength, hold.length - effect.reel * dt );
					holdChanged = true;
					length = hold.length;
				}
				if ( distance <= length || distance < 0.001f )
				{
					continue;
				}
				// Taut: the two may not part, and come back to the rope's length. What that takes is
				// shared by what they weigh: the world gives nothing, a light crate comes to the player.
				float need = std::min( ( distance - length ) * kRopeStiffness, kRopeMaxSpeed );
				float approach = b3Dot( b3Sub( EndVelocity( self, c ), EndVelocity( target, c ) ), toward );
				if ( approach >= need )
				{
					continue;
				}
				float mine = 1.0f / self.mass;
				float theirs = target.mass > 0.0f ? 1.0f / target.mass : 0.0f;
				float impulse = ( need - approach ) / ( mine + theirs );
				PushEnd( self, c, b3MulSV( impulse * mine, toward ) );
				if ( theirs > 0.0f )
				{
					PushEnd( target, c, b3MulSV( -impulse * theirs, toward ) );
				}
			}
		}
		if ( holdChanged )
		{
			e.set<MotionHold>( hold );
		}
	}
}

bool Simulation::EntityHold( uint32_t netId, b3Vec3& end, bool& holds, uint8_t& motion ) const
{
	flecs::entity e = FindEntity( netId );
	const MotionHold* hold = e.is_valid() ? e.try_get<MotionHold>() : nullptr;
	const Transform* t = e.is_valid() ? e.try_get<Transform>() : nullptr;
	b3Vec3 point = {};
	if ( hold == nullptr || t == nullptr || hold->on == 0 || HoldPoint( *hold, point ) == false )
	{
		return false;
	}
	motion = hold->motion;
	holds = m_globals.tick >= hold->holdTick;
	end = point;
	if ( holds == false && hold->holdTick > hold->startTick )
	{
		// Flying: from the player toward where it will hold.
		float along = float( m_globals.tick - hold->startTick ) / float( hold->holdTick - hold->startTick );
		b3Vec3 from = b3Add( t->position, b3Vec3{ 0.0f, kViewPivotHeight, 0.0f } );
		end = b3Add( from, b3MulSV( along, b3Sub( point, from ) ) );
	}
	return true;
}

MoveParams Simulation::PlayerMove( PlayerSlot slot ) const
{
	if ( slot >= kMaxPlayers || m_globals.playerNetIds[slot] == 0 )
	{
		return m_config.move;
	}
	flecs::entity e = FindEntity( m_globals.playerNetIds[slot] );
	return e.is_valid() ? MoveOf( e ) : m_config.move;
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
