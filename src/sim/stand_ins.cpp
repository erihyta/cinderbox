// The characters' states, for the hybrid network model (M248; ROADMAP step 3): the characters' side
// of a server's world as bytes without the physics engine's internals, and a client's characters
// made to agree with it, every one but its own a stand-in (simulation.h: SaveState, ApplyState).
// Bodies are described here, and built again from the description, also for moving one between
// the two worlds (M247).
//
// Layout of a state (little-endian), each entity self-contained so a wire codec can line two
// states up entity by entity (net/state_codec.h) without knowing the components:
//   u32 magic 'CBS2'
//   u32 size, then SimGlobals and the global board
//   u32 entities, then each, in NetId order:
//     u32 netId, u32 size, then its record: u32 mask (a bit per snapshot component), the components
//     u32 size of its bodies' description (0: it has none), then u64 its hash and the description:
//       u8 bodies (1: a PhysicsBody), then each body:
//         u8 type (0xFF: no body), u8 flags (1 enabled, 2 bullet), u8 locks (a bit each),
//         f32 gravity scale, linear damping, angular damping, b3MassData,
//         u8 shapes, then each: u8 type, b3Filter, f32 density, friction, restitution, rolling,
//         then a sphere (centre, radius), a capsule (two centres, radius), a hull (u16 points,
//         the points) or nothing (a mesh, a height field: static level pieces, never rebuilt).
// The physics handles in a record (PhysicsBody, RagdollBodies) are the server's and never read.

#include "simulation.h"

#include "util.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cb
{

namespace
{

constexpr uint32_t kStateMagic = 0x32534243u; // 'CBS2'
constexpr uint8_t kNoBody = 0xFF;
constexpr int kMaxShapes = 64;

template <typename T>
void Put( std::vector<uint8_t>& out, const T& value )
{
	const auto* p = reinterpret_cast<const uint8_t*>( &value );
	out.insert( out.end(), p, p + sizeof( T ) );
}

b3BodyId InWorld( b3BodyId id, b3WorldId world )
{
	id.world0 = uint16_t( world.index1 - 1 );
	return id;
}

// The compact state's grids (simulation.h, SaveState).
constexpr float kPlaceGrid = 1024.0f;
constexpr float kTurnGrid = 32768.0f;
constexpr float kSpeedGrid = 256.0f;

float RoundedTo( float v, float grid )
{
	return std::nearbyint( v * grid ) / grid;
}
b3Vec3 RoundedVec( b3Vec3 v, float grid )
{
	return { RoundedTo( v.x, grid ), RoundedTo( v.y, grid ), RoundedTo( v.z, grid ) };
}
Transform Rounded( const Transform& t )
{
	b3Quat q = { RoundedVec( t.rotation.v, kTurnGrid ), RoundedTo( t.rotation.s, kTurnGrid ) };
	const float length = std::sqrt( q.v.x * q.v.x + q.v.y * q.v.y + q.v.z * q.v.z + q.s * q.s );
	if ( length > 0.0f )
	{
		q = { { q.v.x / length, q.v.y / length, q.v.z / length }, q.s / length };
	}
	else
	{
		q = t.rotation;
	}
	return { RoundedVec( t.position, kPlaceGrid ), q };
}

// One entity of the server's image, read but not applied.
struct EntityRecord
{
	uint32_t netId = 0;
	uint32_t mask = 0;
	const uint8_t* components[32] = {};
	// Its bodies' description (null: none).
	const uint8_t* bodies = nullptr;
	uint32_t bodiesSize = 0;
	uint64_t bodiesHash = 0;
};

// The server's component of a record, copied out (false: it has none).
template <typename T>
bool RecordValue( const EntityRecord& record, int index, T& out )
{
	if ( index < 0 || ( record.mask & ( uint32_t( 1 ) << index ) ) == 0 || record.components[index] == nullptr )
	{
		return false;
	}
	std::memcpy( &out, record.components[index], sizeof( T ) );
	return true;
}

} // namespace

int Simulation::SnapIndex( flecs::entity_t id ) const
{
	for ( size_t i = 0; i < m_snapComponents.size(); ++i )
	{
		if ( m_snapComponents[i].id == id )
		{
			return int( i );
		}
	}
	return -1;
}

void Simulation::DescribeBody( b3BodyId body, std::vector<uint8_t>& out ) const
{
	if ( b3Body_IsValid( body ) == false )
	{
		Put( out, kNoBody );
		return;
	}
	Put( out, uint8_t( b3Body_GetType( body ) ) );
	Put( out, uint8_t( ( b3Body_IsEnabled( body ) ? 1 : 0 ) | ( b3Body_IsBullet( body ) ? 2 : 0 ) ) );
	const b3MotionLocks locks = b3Body_GetMotionLocks( body );
	Put( out, uint8_t( ( locks.linearX ? 1 : 0 ) | ( locks.linearY ? 2 : 0 ) | ( locks.linearZ ? 4 : 0 ) | ( locks.angularX ? 8 : 0 ) |
					   ( locks.angularY ? 16 : 0 ) | ( locks.angularZ ? 32 : 0 ) ) );
	Put( out, b3Body_GetGravityScale( body ) );
	Put( out, b3Body_GetLinearDamping( body ) );
	Put( out, b3Body_GetAngularDamping( body ) );
	Put( out, b3Body_GetMassData( body ) );

	b3ShapeId shapes[kMaxShapes];
	const int count = std::min( b3Body_GetShapes( body, shapes, kMaxShapes ), kMaxShapes );
	Put( out, uint8_t( count ) );
	for ( int i = 0; i < count; ++i )
	{
		const b3ShapeId shape = shapes[i];
		const b3ShapeType type = b3Shape_GetType( shape );
		Put( out, uint8_t( type ) );
		Put( out, b3Shape_GetFilter( shape ) );
		const b3SurfaceMaterial material = b3Shape_GetSurfaceMaterial( shape );
		Put( out, b3Shape_GetDensity( shape ) );
		Put( out, material.friction );
		Put( out, material.restitution );
		Put( out, material.rollingResistance );
		switch ( type )
		{
			case b3_sphereShape:
				Put( out, b3Shape_GetSphere( shape ) );
				break;
			case b3_capsuleShape:
				Put( out, b3Shape_GetCapsule( shape ) );
				break;
			case b3_hullShape:
			{
				const b3HullData* hull = b3Shape_GetHull( shape );
				const int points = hull != nullptr ? hull->vertexCount : 0;
				Put( out, uint16_t( points ) );
				if ( points > 0 )
				{
					const b3Vec3* p = b3GetHullPoints( hull );
					const auto* bytes = reinterpret_cast<const uint8_t*>( p );
					out.insert( out.end(), bytes, bytes + sizeof( b3Vec3 ) * size_t( points ) );
				}
				break;
			}
			default:
				break; // a mesh, a height field: static level pieces, which every world has
		}
	}
}

void Simulation::DescribeBodies( flecs::entity e, std::vector<uint8_t>& out ) const
{
	out.clear();
	if ( const RagdollBodies* rb = e.try_get<RagdollBodies>() )
	{
		Put( out, uint8_t( kRagdollParts ) );
		for ( b3BodyId body : rb->body )
		{
			DescribeBody( InWorld( body, m_physicsWorld ), out );
		}
	}
	else if ( const PhysicsBody* pb = e.try_get<PhysicsBody>() )
	{
		Put( out, uint8_t( 1 ) );
		DescribeBody( BodyOf( *pb ), out );
	}
}

b3BodyId Simulation::BuildBody( ByteReader& r, b3WorldId world, BuildAs as, Transform at, b3Vec3 linear, b3Vec3 angular, b3ShapeId& first )
{
	first = {};
	const uint8_t type = r.Read<uint8_t>();
	if ( type == kNoBody || r.Ok() == false )
	{
		return {};
	}
	const uint8_t flags = r.Read<uint8_t>();
	const uint8_t locks = r.Read<uint8_t>();
	const float gravityScale = r.Read<float>();
	const float linearDamping = r.Read<float>();
	const float angularDamping = r.Read<float>();
	const b3MassData mass = r.Read<b3MassData>();
	const int shapes = r.Read<uint8_t>();
	if ( r.Ok() == false || type > uint8_t( b3_dynamicBody ) )
	{
		r.Fail();
		return {};
	}

	b3BodyDef def = b3DefaultBodyDef();
	const b3BodyType serverType = b3BodyType( type );
	// A stand-in is placed, never simulated: kinematic, unless the server's never moves.
	def.type = as == BuildAs::Dynamic							? b3_dynamicBody
			   : as == BuildAs::Placed && serverType != b3_staticBody ? b3_kinematicBody
																	 : serverType;
	def.position = at.position;
	def.rotation = at.rotation;
	if ( def.type != b3_staticBody )
	{
		def.linearVelocity = linear;
		def.angularVelocity = angular;
	}
	def.gravityScale = gravityScale;
	def.linearDamping = linearDamping;
	def.angularDamping = angularDamping;
	def.isBullet = ( flags & 2 ) != 0;
	def.motionLocks = { ( locks & 1 ) != 0, ( locks & 2 ) != 0, ( locks & 4 ) != 0, ( locks & 8 ) != 0, ( locks & 16 ) != 0, ( locks & 32 ) != 0 };
	b3BodyId body = b3CreateBody( world, &def );

	for ( int i = 0; i < shapes && r.Ok(); ++i )
	{
		const uint8_t shapeType = r.Read<uint8_t>();
		b3ShapeDef sd = b3DefaultShapeDef();
		sd.filter = r.Read<b3Filter>();
		sd.density = r.Read<float>();
		sd.baseMaterial.friction = r.Read<float>();
		sd.baseMaterial.restitution = r.Read<float>();
		sd.baseMaterial.rollingResistance = r.Read<float>();
		// (As the simulation makes its shapes: MakeShapeDef in simulation.cpp.)
		sd.enableHitEvents = sd.filter.categoryBits != CatStatic;
		// (A dynamic one as described gets the described mass below; one made dynamic weighs its shapes.)
		sd.updateBodyMass = def.type != b3_dynamicBody || as == BuildAs::Dynamic;
		b3ShapeId made = {};
		switch ( shapeType )
		{
			case b3_sphereShape:
			{
				const b3Sphere sphere = r.Read<b3Sphere>();
				if ( r.Ok() )
				{
					made = b3CreateSphereShape( body, &sd, &sphere );
				}
				break;
			}
			case b3_capsuleShape:
			{
				const b3Capsule capsule = r.Read<b3Capsule>();
				if ( r.Ok() )
				{
					made = b3CreateCapsuleShape( body, &sd, &capsule );
				}
				break;
			}
			case b3_hullShape:
			{
				const int count = r.Read<uint16_t>();
				const uint8_t* bytes = r.Take( sizeof( b3Vec3 ) * size_t( count ) );
				if ( bytes == nullptr || count < 4 )
				{
					break;
				}
				std::vector<b3Vec3> points( static_cast<size_t>( count ) );
				std::memcpy( points.data(), bytes, sizeof( b3Vec3 ) * size_t( count ) );
				if ( b3HullData* hull = b3CreateHull( points.data(), count, B3_MAX_HULL_VERTICES ) )
				{
					made = b3CreateHullShape( body, &sd, hull ); // (the world keeps a copy)
					b3DestroyHull( hull );
				}
				break;
			}
			default:
				break;
		}
		if ( first.index1 == 0 && b3Shape_IsValid( made ) )
		{
			first = made;
		}
	}
	if ( def.type == b3_dynamicBody && as != BuildAs::Dynamic )
	{
		b3Body_SetMassData( body, mass ); // the server's, exactly
	}
	if ( ( flags & 1 ) == 0 )
	{
		b3Body_Disable( body );
	}
	return body;
}

PhysicsBody Simulation::MoveBodyToWorld( b3BodyId body, b3WorldId to )
{
	std::vector<uint8_t> description;
	DescribeBody( body, description );
	const b3WorldTransform xf = b3Body_GetTransform( body );
	const b3Vec3 linear = b3Body_GetLinearVelocity( body );
	const b3Vec3 angular = b3Body_GetAngularVelocity( body );
	b3DestroyBody( body );
	ByteReader r( description.data(), description.size() );
	b3ShapeId first;
	b3BodyId moved = BuildBody( r, to, BuildAs::Described, { xf.p, xf.q }, linear, angular, first );
	IndexesStale();
	return MakePhysicsBody( moved, first );
}

void Simulation::DestroyBodies( flecs::entity e )
{
	if ( const PhysicsBody* pb = e.try_get<PhysicsBody>() )
	{
		if ( b3Body_IsValid( BodyOf( *pb ) ) )
		{
			b3DestroyBody( BodyOf( *pb ) );
		}
		e.remove<PhysicsBody>();
	}
	if ( const RagdollBodies* rb = e.try_get<RagdollBodies>() )
	{
		for ( b3BodyId body : rb->body )
		{
			if ( b3Body_IsValid( InWorld( body, m_physicsWorld ) ) )
			{
				b3DestroyBody( InWorld( body, m_physicsWorld ) ); // (and its joints)
			}
		}
		e.remove<RagdollBodies>();
	}
}

void Simulation::RemoveForState( flecs::entity e )
{
	DestroyBodies( e );
	e.destruct();
}

std::vector<std::pair<std::string, uint32_t>> Simulation::StateComponents() const
{
	std::vector<std::pair<std::string, uint32_t>> out;
	for ( const SnapComponent& sc : m_snapComponents )
	{
		const char* name = ecs_get_name( m_world.c_ptr(), sc.id );
		out.push_back( { name != nullptr ? name : "?", sc.size } );
	}
	return out;
}

bool Simulation::IsStandIn( uint32_t netId ) const
{
	flecs::entity e = m_followsServer ? FindEntity( netId ) : flecs::entity();
	return e.is_valid() && e.has<StandIn>();
}

bool Simulation::PhysicsSide( flecs::entity e ) const
{
	if ( e.has<RagdollBodies>() || e.has<Pusher>() )
	{
		return true;
	}
	const PhysicsBody* pb = e.try_get<PhysicsBody>();
	return pb != nullptr && pb->body.world0 == 0;
}

bool Simulation::CharacterSide( flecs::entity e ) const
{
	if ( PhysicsSide( e ) )
	{
		return false;
	}
	if ( e.has<Character>() || e.has<PhysicsBody>() )
	{
		return true; // (a body not in the physics world is in the characters')
	}
	const HeldItem* held = e.try_get<HeldItem>();
	return held != nullptr && held->holder != 0;
}

void Simulation::SaveState( std::vector<uint8_t>& out, bool compact, uint32_t only )
{
	PhysicsArena::Scope scope( *m_arena );
	out.clear();
	Put( out, kStateMagic );
	const uint32_t head = uint32_t( sizeof( SimGlobals ) + m_globalBoard.size() * 4 );
	Put( out, head );
	Put( out, m_globals );
	const auto* board = reinterpret_cast<const uint8_t*>( m_globalBoard.data() );
	out.insert( out.end(), board, board + m_globalBoard.size() * 4 );
	const size_t countAt = out.size();
	Put( out, uint32_t( 0 ) );

	ecs_world_t* world = m_world.c_ptr();
	std::vector<uint8_t> description;
	const int transformIndex = compact ? SnapIndex( m_world.component<Transform>().id() ) : -1;
	const int velocityIndex = compact ? SnapIndex( m_world.component<Velocity>().id() ) : -1;
	uint32_t count = 0;
	for ( const EntityRef& ref : m_entities )
	{
		flecs::entity e( m_world, ref.entity );
		if ( CharacterSide( e ) == false )
		{
			continue;
		}
		if ( only != 0 && ref.netId != only )
		{
			const HeldItem* held = e.try_get<HeldItem>();
			if ( held == nullptr || held->holder != only )
			{
				continue;
			}
		}
		count += 1;
		Put( out, ref.netId );
		const size_t sizeAt = out.size();
		Put( out, uint32_t( 0 ) );
		uint32_t mask = 0;
		for ( size_t i = 0; i < m_snapComponents.size(); ++i )
		{
			mask |= ecs_has_id( world, ref.entity, m_snapComponents[i].id ) ? uint32_t( 1 ) << i : 0;
		}
		Put( out, mask );
		for ( size_t i = 0; i < m_snapComponents.size(); ++i )
		{
			const SnapComponent& sc = m_snapComponents[i];
			if ( ( mask & ( uint32_t( 1 ) << i ) ) && sc.size > 0 )
			{
				const auto* p = static_cast<const uint8_t*>( ecs_get_id( world, ref.entity, sc.id ) );
				const size_t at = out.size();
				out.insert( out.end(), p, p + sc.size );
				if ( int( i ) == transformIndex )
				{
					Transform t;
					std::memcpy( &t, out.data() + at, sizeof( t ) );
					t = Rounded( t );
					std::memcpy( out.data() + at, &t, sizeof( t ) );
				}
				else if ( int( i ) == velocityIndex )
				{
					Velocity v;
					std::memcpy( &v, out.data() + at, sizeof( v ) );
					v = { RoundedVec( v.linear, kSpeedGrid ), RoundedVec( v.angular, kSpeedGrid ) };
					std::memcpy( out.data() + at, &v, sizeof( v ) );
				}
			}
		}
		const uint32_t record = uint32_t( out.size() - sizeAt - sizeof( uint32_t ) );
		std::memcpy( out.data() + sizeAt, &record, sizeof( record ) );

		if ( e.has<PhysicsBody>() )
		{
			DescribeBodies( e, description );
			Put( out, uint32_t( description.size() ) );
			Put( out, HashBytes( kHashSeed, description.data(), description.size() ) );
			out.insert( out.end(), description.begin(), description.end() );
		}
		else
		{
			Put( out, uint32_t( 0 ) );
		}
	}
	std::memcpy( out.data() + countAt, &count, sizeof( count ) );
}

bool Simulation::ApplyState( const uint8_t* data, size_t size, PlayerSlot own, StateApply which )
{
	// First everything is read and checked; only then is anything changed.
	ByteReader r( data, size );
	if ( r.Read<uint32_t>() != kStateMagic )
	{
		return false;
	}
	const uint32_t head = r.Read<uint32_t>();
	if ( head != sizeof( SimGlobals ) + m_globalBoard.size() * 4 )
	{
		return false;
	}
	const SimGlobals globals = r.Read<SimGlobals>();
	const uint8_t* board = r.Take( m_globalBoard.size() * 4 );
	const uint32_t count = r.Read<uint32_t>();
	if ( r.Ok() == false || count > size / 12 )
	{
		return false;
	}
	std::vector<EntityRecord> records( count );
	for ( uint32_t n = 0; n < count && r.Ok(); ++n )
	{
		EntityRecord& rec = records[n];
		rec.netId = r.Read<uint32_t>();
		const uint32_t recordSize = r.Read<uint32_t>();
		const uint8_t* record = r.Take( recordSize );
		if ( record == nullptr || ( n > 0 && rec.netId <= records[n - 1].netId ) )
		{
			return false;
		}
		ByteReader er( record, recordSize );
		rec.mask = er.Read<uint32_t>();
		if ( m_snapComponents.size() < 32 && ( rec.mask >> m_snapComponents.size() ) != 0 )
		{
			return false;
		}
		for ( size_t i = 0; i < m_snapComponents.size(); ++i )
		{
			if ( ( rec.mask & ( uint32_t( 1 ) << i ) ) && m_snapComponents[i].size > 0 )
			{
				rec.components[i] = er.Take( m_snapComponents[i].size );
			}
		}
		if ( er.Ok() == false || er.AtEnd() == false )
		{
			return false;
		}
		const uint32_t bodySize = r.Read<uint32_t>();
		if ( bodySize > 0 )
		{
			rec.bodiesHash = r.Read<uint64_t>();
			rec.bodies = r.Take( bodySize );
			rec.bodiesSize = bodySize;
			if ( rec.bodies == nullptr )
			{
				return false;
			}
		}
	}
	if ( r.Ok() == false || r.AtEnd() == false )
	{
		return false;
	}

	PhysicsArena::Scope scope( *m_arena );
	m_followsServer = true;
	m_world.component<StandIn>();
	m_world.component<StateBody>();
	const int physicsIndex = SnapIndex( m_world.component<PhysicsBody>().id() );
	const int ragdollIndex = SnapIndex( m_world.component<RagdollBodies>().id() );
	const int pusherIndex = SnapIndex( m_world.component<Pusher>().id() );
	const int heldIndex = SnapIndex( m_world.component<HeldItem>().id() );
	const int transformIndex = SnapIndex( m_world.component<Transform>().id() );
	const int velocityIndex = SnapIndex( m_world.component<Velocity>().id() );

	const uint32_t ownNetId = own < kMaxPlayers ? globals.playerNetIds[own] : 0;
	if ( which == StateApply::Others )
	{
		// What the characters' side has of the globals: whose the slots are, the next character's
		// NetId (never back: what this world made meanwhile keeps its own), the mods' fields.
		std::memcpy( m_globals.playerNetIds, globals.playerNetIds, sizeof( m_globals.playerNetIds ) );
		m_globals.nextCharacterId = std::max( m_globals.nextCharacterId, globals.nextCharacterId );
		if ( m_globalBoard.empty() == false )
		{
			std::memcpy( m_globalBoard.data(), board, m_globalBoard.size() * 4 );
		}
	}
	// Whether an entity (here, or as the server has it) is the own character's, and whether this
	// application is about it.
	auto mineHere = [&]( flecs::entity e, uint32_t netId ) {
		if ( ownNetId == 0 )
		{
			return false;
		}
		const HeldItem* held = e.try_get<HeldItem>();
		return netId == ownNetId || ( held != nullptr && held->holder == ownNetId );
	};
	auto about = [&]( bool mine ) { return which == StateApply::Own ? mine : mine == false; };

	ecs_world_t* world = m_world.c_ptr();
	std::vector<EntityRef> restored;
	restored.reserve( m_entities.size() + records.size() );
	std::vector<uint8_t> description;
	size_t existing = 0;
	// What is here and not in the state: gone from the characters' side if it was on it (and is
	// what this application is about); anything else stays.
	auto passOver = [&]( const EntityRef& ref ) {
		flecs::entity e( m_world, ref.entity );
		if ( CharacterSide( e ) && about( mineHere( e, ref.netId ) ) )
		{
			RemoveForState( e );
			return;
		}
		restored.push_back( ref );
	};
	for ( const EntityRecord& rec : records )
	{
		while ( existing < m_entities.size() && m_entities[existing].netId < rec.netId )
		{
			passOver( m_entities[existing] );
			++existing;
		}
		HeldItem held;
		const bool mine = ownNetId != 0 && ( rec.netId == ownNetId || ( RecordValue( rec, heldIndex, held ) && held.holder == ownNetId ) );
		flecs::entity e;
		if ( existing < m_entities.size() && m_entities[existing].netId == rec.netId )
		{
			e = flecs::entity( m_world, m_entities[existing].entity );
			++existing;
			// Not about it, or still in this world's physics (its event not reached yet): as it is.
			if ( about( mine ) == false || PhysicsSide( e ) )
			{
				restored.push_back( { rec.netId, e.id() } );
				continue;
			}
		}
		else if ( about( mine ) )
		{
			e = flecs::entity( m_world, ecs_new( world ) );
		}
		else
		{
			continue;
		}

		// Its components, but the physics handles (this world's own).
		for ( size_t i = 0; i < m_snapComponents.size(); ++i )
		{
			if ( int( i ) == physicsIndex || int( i ) == ragdollIndex || int( i ) == pusherIndex )
			{
				continue;
			}
			const SnapComponent& sc = m_snapComponents[i];
			if ( rec.mask & ( uint32_t( 1 ) << i ) )
			{
				if ( sc.size > 0 )
				{
					ecs_set_id( world, e.id(), sc.id, sc.size, rec.components[i] );
				}
				else
				{
					ecs_add_id( world, e.id(), sc.id );
				}
			}
			else if ( ecs_has_id( world, e.id(), sc.id ) )
			{
				ecs_remove_id( world, e.id(), sc.id );
			}
		}

		// Its body (the characters' world's): kept when built from the description the server has
		// now, else built again.
		const bool standIn = mine == false;
		if ( rec.bodies == nullptr )
		{
			DestroyBodies( e );
			e.remove<StateBody>();
		}
		else
		{
			bool same = false;
			if ( e.has<PhysicsBody>() )
			{
				const StateBody* sb = e.try_get<StateBody>();
				if ( sb != nullptr && standIn && e.has<StandIn>() )
				{
					same = sb->hash == rec.bodiesHash;
				}
				else
				{
					DescribeBodies( e, description );
					same = HashBytes( kHashSeed, description.data(), description.size() ) == rec.bodiesHash;
				}
			}
			Transform t;
			Velocity v;
			RecordValue( rec, transformIndex, t );
			RecordValue( rec, velocityIndex, v );
			if ( same == false )
			{
				DestroyBodies( e );
				ByteReader br( rec.bodies, rec.bodiesSize );
				if ( br.Read<uint8_t>() == 1 )
				{
					b3ShapeId shape;
					b3BodyId body = BuildBody( br, m_characterWorld, standIn ? BuildAs::Placed : BuildAs::Described, t, {}, {}, shape );
					if ( b3Body_IsValid( body ) )
					{
						e.set<PhysicsBody>( MakePhysicsBody( body, shape ) );
					}
				}
			}
			e.set<StateBody>( { rec.bodiesHash } );

			// Where the server has it; a stand-in stays there (nothing moves it until the next state).
			if ( const PhysicsBody* pb = e.try_get<PhysicsBody>() )
			{
				const b3BodyId body = BodyOf( *pb );
				if ( b3Body_IsValid( body ) && b3Body_GetType( body ) != b3_staticBody )
				{
					b3Body_SetTransform( body, t.position, t.rotation );
					b3Body_SetLinearVelocity( body, {} );
					b3Body_SetAngularVelocity( body, {} );
				}
			}
		}

		if ( standIn )
		{
			e.add<StandIn>();
		}
		else
		{
			e.remove<StandIn>();
		}
		restored.push_back( { rec.netId, e.id() } );
	}
	while ( existing < m_entities.size() )
	{
		passOver( m_entities[existing] );
		++existing;
	}
	m_entities = std::move( restored );
	IndexesStale();
	return true;
}

} // namespace cb
