// The physics world's inputs (M246; ROADMAP step 3). Characters read the physics world and never
// write it themselves: what they do to it goes in by these, so a simulation that follows another's
// physics (the hybrid network model's client, or a test) gets the same physics world whatever its
// own characters do.
//
// Pushers: a kinematic capsule per character, placed every tick where the character is, rounded,
// while it is near something pushed by characters (a ball, an item lying about, a ragdoll). They
// shove those and touch nothing else (components.h, CollisionMask). The server makes the places
// from its characters (MakePusherPlaces); a follower takes them from its frame.
//
// Physics events (types.h, PhysicsEvent): every write to a physics body that reads a character,
// queued while the characters act and applied after they move, before the physics step. A follower
// drops its own and applies its frame's.

#include "simulation.h"

#include "util.h"

#include "box3d/box3d.h"

#include <algorithm>
#include <cmath>

namespace cb
{

namespace
{

// How far beyond a character's capsule something pushable makes its pusher stand in the world.
constexpr float kPusherReach = 0.25f;

int32_t OnGrid( float v )
{
	return int32_t( std::lround( v * kPusherGrid ) );
}

uint16_t Millimetres( float v )
{
	return uint16_t( std::clamp( std::lround( v * 1000.0f ), 1l, 65535l ) );
}

bool FoundOne( b3ShapeId, void* context )
{
	*static_cast<bool*>( context ) = true;
	return false; // (one is enough)
}

} // namespace

void Simulation::MakePusherPlaces( std::vector<PusherPlace>& out )
{
	out.clear();
	const float dt = m_config.TimeStep();
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		const Character* c = e.try_get<Character>();
		if ( c == nullptr || c->dead || e.has<PhysicsBody>() == false || StandsIn( e ) )
		{
			continue;
		}
		float radius = 0.0f, halfHeight = 0.0f;
		if ( const DrivenProp* driven = e.try_get<DrivenProp>() )
		{
			radius = driven->radius;
			halfHeight = driven->halfHeight;
		}
		else if ( const Shape* shape = e.try_get<Shape>(); shape != nullptr && shape->kind == ShapeKind::Capsule )
		{
			radius = shape->halfExtents.x;
			halfHeight = shape->halfExtents.y;
		}
		if ( radius <= 0.0f )
		{
			continue;
		}
		const b3Vec3 at = e.get<Transform>().position;
		// Near anything pushable (it may get there this tick)?
		const float reach = radius + kPusherReach + b3Length( c->velocity ) * dt * 2.0f;
		const b3AABB box = { { at.x - reach, at.y - halfHeight - reach, at.z - reach }, { at.x + reach, at.y + halfHeight + reach, at.z + reach } };
		const b3QueryFilter filter = { CatPusher, CatPushable | CatRagdoll, 0, nullptr };
		bool found = false;
		b3World_OverlapAABB( m_physicsWorld, box, filter, FoundOne, &found );
		if ( found == false )
		{
			continue;
		}
		out.push_back( { r.netId, OnGrid( at.x ), OnGrid( at.y ), OnGrid( at.z ), Millimetres( radius ), Millimetres( halfHeight ) } );
	}
}

void Simulation::PlacePushers( const InputFrame& frame )
{
	m_physicsMade.tick = frame.tick;
	if ( m_followsPhysics )
	{
		m_physicsMade.pushers = frame.pushers;
	}
	else
	{
		MakePusherPlaces( m_physicsMade.pushers );
	}

	const float dt = m_config.TimeStep();
	const uint16_t world0 = uint16_t( m_physicsWorld.index1 - 1 );
	auto inWorld = [world0]( b3BodyId id ) {
		id.world0 = world0;
		return id;
	};
	// The places are in NetId order, as the entities are: one walk over both.
	size_t next = 0;
	const std::vector<PusherPlace>& places = m_physicsMade.pushers;
	for ( const EntityRef& r : m_entities )
	{
		while ( next < places.size() && places[next].netId < r.netId )
		{
			++next; // (a place for nothing this world has: a follower's characters may differ)
		}
		flecs::entity e( m_world, r.entity );
		const PusherPlace* place = next < places.size() && places[next].netId == r.netId ? &places[next] : nullptr;
		Pusher* pusher = e.try_get_mut<Pusher>();
		if ( place == nullptr )
		{
			if ( pusher != nullptr && pusher->active != 0 )
			{
				b3Body_Disable( inWorld( pusher->body ) );
				pusher->active = 0;
			}
			continue;
		}
		const b3Vec3 at = { float( place->x ) / kPusherGrid, float( place->y ) / kPusherGrid, float( place->z ) / kPusherGrid };
		if ( pusher != nullptr && ( pusher->radius != place->radius || pusher->halfHeight != place->halfHeight ) )
		{
			b3DestroyBody( inWorld( pusher->body ) );
			e.remove<Pusher>();
			pusher = nullptr;
		}
		if ( pusher == nullptr )
		{
			b3BodyDef def = b3DefaultBodyDef();
			def.type = b3_kinematicBody;
			def.position = at;
			b3BodyId body = b3CreateBody( m_physicsWorld, &def );
			const float radius = float( place->radius ) / 1000.0f;
			const float halfHeight = float( place->halfHeight ) / 1000.0f;
			b3Capsule capsule = { { 0.0f, -halfHeight, 0.0f }, { 0.0f, halfHeight, 0.0f }, radius };
			b3ShapeDef shapeDef = b3DefaultShapeDef();
			shapeDef.filter.categoryBits = CatPusher;
			shapeDef.filter.maskBits = CollisionMask( CatPusher );
			b3ShapeId shape = b3CreateCapsuleShape( body, &shapeDef, &capsule );
			body.world0 = 0;
			shape.world0 = 0;
			e.set<Pusher>( { body, shape, place->radius, place->halfHeight, 1 } );
			continue;
		}
		b3BodyId body = inWorld( pusher->body );
		if ( pusher->active == 0 )
		{
			// Back where it is now, from wherever it was left: no sweep across the world between.
			b3Body_Enable( body );
			b3Body_SetTransform( body, at, b3Quat{ { 0.0f, 0.0f, 0.0f }, 1.0f } );
			b3Body_SetLinearVelocity( body, { 0.0f, 0.0f, 0.0f } );
			pusher->active = 1;
			continue;
		}
		// Moved there over the step: what it meets is shoved at its speed.
		b3Body_SetTargetTransform( body, { at, b3Quat{ { 0.0f, 0.0f, 0.0f }, 1.0f } }, dt, true );
	}
	IndexesStale();
}

flecs::entity Simulation::EntityWithId( uint32_t netId )
{
	if ( flecs::entity e = FindEntity( netId ); e.is_valid() )
	{
		return e;
	}
	flecs::entity e = m_world.entity();
	e.set<NetId>( { netId } );
	auto it = std::lower_bound( m_entities.begin(), m_entities.end(), netId, []( const EntityRef& r, uint32_t id ) { return r.netId < id; } );
	m_entities.insert( it, { netId, e.id() } );
	m_globals.nextNetId = std::max( m_globals.nextNetId, netId + 1 );
	IndexesStale();
	return e;
}

void Simulation::QueueIntoWorld( flecs::entity item, b3Vec3 grip, b3Quat rotation, b3Vec3 velocity, uint32_t propOwner, uint32_t lifetimeTicks )
{
	if ( item.is_valid() == false || item.has<HeldItem>() == false )
	{
		return;
	}
	// Out of the hand at once (what the character does with its slots goes on without it); into
	// the world when the events are applied.
	HeldItem held = item.get<HeldItem>();
	held.holder = 0;
	held.socket = 0;
	held.stowed = 0;
	held.slot = kNoSlot;
	item.set<HeldItem>( held );
	IndexesStale();

	PhysicsEvent event;
	event.type = PhysicsEventType::IntoWorld;
	event.netId = item.get<NetId>().value;
	event.kind = held.kind;
	event.count = held.count;
	if ( HasBoard( item ) )
	{
		const Blackboard board = GetBoard( item );
		event.board.assign( board.values.begin(), board.values.end() );
	}
	event.at = { grip.x, grip.y, grip.z };
	event.turn[0] = rotation.v.x;
	event.turn[1] = rotation.v.y;
	event.turn[2] = rotation.v.z;
	event.turn[3] = rotation.s;
	event.vector = { velocity.x, velocity.y, velocity.z };
	event.owner = propOwner;
	event.ticks = lifetimeTicks;
	m_physicsQueue.push_back( std::move( event ) );
}

void Simulation::ApplyPhysicsEvent( PhysicsEvent& event )
{
	const b3Vec3 at = { event.at.x, event.at.y, event.at.z };
	const b3Vec3 vector = { event.vector.x, event.vector.y, event.vector.z };
	switch ( event.type )
	{
		case PhysicsEventType::Push:
		{
			flecs::entity e = FindEntity( event.netId );
			const PhysicsBody* pb = e.is_valid() && e.has<Character>() == false ? e.try_get<PhysicsBody>() : nullptr;
			if ( pb != nullptr && b3Body_GetType( BodyOf( *pb ) ) == b3_dynamicBody )
			{
				b3Body_ApplyLinearImpulse( BodyOf( *pb ), vector, at, true );
			}
			break;
		}
		case PhysicsEventType::Break:
		{
			if ( flecs::entity wall = FindEntity( event.netId ); wall.is_valid() )
			{
				BreakWall( wall, at, event.vector.x, event.vector.y, event.ticks );
			}
			break;
		}
		case PhysicsEventType::IntoWorld:
		{
			flecs::entity item = EntityWithId( event.netId );
			if ( item.has<Character>() )
			{
				break; // (not an item: a follower names it otherwise)
			}
			if ( const PhysicsBody* pb = item.try_get<PhysicsBody>() )
			{
				b3DestroyBody( BodyOf( *pb ) ); // (already lying about: put where it is said to be)
			}
			HeldItem held = item.has<HeldItem>() ? item.get<HeldItem>() : HeldItem{};
			held.holder = 0;
			held.kind = event.kind;
			held.count = event.count;
			held.socket = 0;
			held.stowed = 0;
			held.slot = kNoSlot;
			item.set<HeldItem>( held );
			if ( event.board.empty() == false )
			{
				Blackboard board;
				board.values.assign( event.board.data(), event.board.size() );
				SetBoard( item, board );
			}
			const b3Quat turn = { { event.turn[0], event.turn[1], event.turn[2] }, event.turn[3] };
			PutItemInWorld( item, at, turn, vector );
			if ( event.owner != 0 )
			{
				item.set<Prop>( { event.owner, m_globals.tick, event.ticks > 0 ? m_globals.tick + event.ticks : 0u } );
			}
			IndexesStale();
			break;
		}
		case PhysicsEventType::Ragdoll:
		{
			flecs::entity body = CreateRagdoll( event );
			event.made = body.get<NetId>().value;
			SimCommand hit;
			hit.type = CommandType::Impulse;
			hit.mode = event.mode;
			hit.a = event.hitAt;
			hit.b = event.hitVector;
			ApplyImpulse( body, hit );
			EnforceRagdollCap( event.count );
			break;
		}
	}
}

void Simulation::ApplyPhysicsEvents( const InputFrame& frame, uint8_t phase )
{
	// What this point of the step applies: the frame's, following another's physics (what its own
	// characters did is not the physics world's), or what was queued since the last point. Either
	// way the queue is empty after: nothing crosses into another tick (snapshots do not hold it).
	const size_t from = m_physicsMade.physics.size();
	if ( m_followsPhysics )
	{
		for ( const PhysicsEvent& event : frame.physics )
		{
			if ( event.phase == phase )
			{
				m_physicsMade.physics.push_back( event );
			}
		}
	}
	else
	{
		for ( PhysicsEvent& event : m_physicsQueue )
		{
			event.phase = phase;
			m_physicsMade.physics.push_back( std::move( event ) );
		}
	}
	m_physicsQueue.clear();
	for ( size_t i = from; i < m_physicsMade.physics.size(); ++i )
	{
		ApplyPhysicsEvent( m_physicsMade.physics[i] );
	}
	// (An event applied may queue more: a ragdoll's hit is not one; a break is the world's own.)
	m_physicsQueue.clear();
}

uint64_t Simulation::PhysicsHash() const
{
	const int handles[] = { SnapIndex( m_world.component<PhysicsBody>().id() ), SnapIndex( m_world.component<RagdollBodies>().id() ),
							SnapIndex( m_world.component<Pusher>().id() ) };
	std::vector<uint8_t> bytes;
	ecs_world_t* world = m_world.c_ptr();
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		if ( ( e.has<PhysicsBody>() == false && e.has<RagdollBodies>() == false ) || e.has<Character>() )
		{
			continue;
		}
		const auto* id = reinterpret_cast<const uint8_t*>( &r.netId );
		bytes.insert( bytes.end(), id, id + sizeof( r.netId ) );
		for ( size_t i = 0; i < m_snapComponents.size(); ++i )
		{
			const SnapComponent& sc = m_snapComponents[i];
			if ( std::find( std::begin( handles ), std::end( handles ), int( i ) ) != std::end( handles ) ||
				 ecs_has_id( world, r.entity, sc.id ) == false )
			{
				continue;
			}
			bytes.push_back( uint8_t( i ) );
			if ( sc.size > 0 )
			{
				const auto* p = static_cast<const uint8_t*>( ecs_get_id( world, r.entity, sc.id ) );
				bytes.insert( bytes.end(), p, p + sc.size );
			}
		}
	}
	return HashBytes( kHashSeed, bytes.data(), bytes.size() );
}

} // namespace cb
