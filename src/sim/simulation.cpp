#include "simulation.h"

#include "anim_controller.h"
#include "anim_graph.h"
#include "box3d_shim.h"
#include "detmath.h"
#include "level.h"
#include "map.h"
#include "motions.h"
#include "mover.h"
#include "ragdoll.h"
#include "util.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

// Yaw, then pitch, then roll (Godot's YXZ order). No roll leaves the product of the first two as it
// always was, so levels without roll build exactly as before.
b3Quat MakeRotation( float yaw, float pitch, float roll = 0.0f )
{
	b3Quat qYaw = b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, yaw );
	b3Quat qPitch = b3MakeQuatFromAxisAngle( b3Vec3{ 1.0f, 0.0f, 0.0f }, pitch );
	b3Quat turned = b3MulQuat( qYaw, qPitch );
	if ( roll == 0.0f )
	{
		return turned;
	}
	return b3MulQuat( turned, b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 0.0f, 1.0f }, roll ) );
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

namespace
{
std::atomic<uint32_t> s_newWorkers{ kPhysicsWorkers };
}

Simulation::Simulation( const SimConfig& config, const LevelLayout& map )
	: m_config( config )
	, m_map( map )
	, m_world( CreateFlecsWorld() )
{
	m_arena = std::make_unique<PhysicsArena>( size_t( config.physicsArenaMB ) * 1024 * 1024 );
	m_globals.rngState = config.seed;
	m_globalBoard.resize( config.globalFields );

	RegisterComponents();
	RegisterIndexes();

	std::unique_lock<std::mutex> lock( WorldLifetimeMutex() );
	PhysicsArena::Scope scope( *m_arena );
	b3WorldDef def = b3DefaultWorldDef();
	def.gravity = kGravityVector;
	def.hitEventThreshold = kImpactThreshold;
	// Workers: what a step's work is divided by, the same everywhere; which threads do it is the
	// process's choice (physics_threads.h).
	m_tasks = std::make_unique<PhysicsTasks>();
	m_tasks->arena = m_arena.get();
	def.workerCount = s_newWorkers.load();
	def.enqueueTask = &PhysicsThreads::Enqueue;
	def.finishTask = &PhysicsThreads::Finish;
	def.userTaskContext = m_tasks.get();
	m_physicsWorld = b3CreateWorld( &def );
	lock.unlock();

	BuildLevel();
}

void Simulation::SetWorkersForNewSimulations( uint32_t workers )
{
	s_newWorkers.store( workers );
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

flecs::entity_t Simulation::RegisterSizedComponent( const char* name, uint32_t size )
{
	ecs_world_t* world = m_world.c_ptr();
	ecs_entity_desc_t entity = {};
	entity.name = name;
	flecs::entity_t id = ecs_entity_init( world, &entity );
	if ( size > 0 )
	{
		ecs_component_desc_t component = {};
		component.entity = id;
		component.type.size = ecs_size_t( size );
		component.type.alignment = 4;
		id = ecs_component_init( world, &component );
	}
	m_snapComponents.push_back( { id, size } );
	return id;
}

bool Simulation::HasBoard( flecs::entity e ) const
{
	return m_config.fields > 0 && ecs_has_id( m_world.c_ptr(), e.id(), m_boardId );
}

Blackboard Simulation::GetBoard( flecs::entity e ) const
{
	Blackboard board;
	if ( HasBoard( e ) )
	{
		board.values.assign( static_cast<const int32_t*>( ecs_get_id( m_world.c_ptr(), e.id(), m_boardId ) ), m_config.fields );
	}
	return board;
}

// (What is past the fields the config has is dropped: nothing declared it.)
void Simulation::SetBoard( flecs::entity e, const Blackboard& board )
{
	if ( m_config.fields == 0 )
	{
		return;
	}
	m_sizedScratch.resize( size_t( m_config.fields ) * 4 );
	board.values.copy_to( reinterpret_cast<int32_t*>( m_sizedScratch.data() ), m_config.fields );
	ecs_set_id( m_world.c_ptr(), e.id(), m_boardId, m_sizedScratch.size(), m_sizedScratch.data() );
}

bool Simulation::HasMotionState( flecs::entity e ) const
{
	return m_config.motions > 0 && ecs_has_id( m_world.c_ptr(), e.id(), m_motionId );
}

// The block: the actions held last tick (a word of 64), then a slot per motion.
MotionState Simulation::GetMotionState( flecs::entity e ) const
{
	MotionState state;
	if ( HasMotionState( e ) )
	{
		const uint8_t* bytes = static_cast<const uint8_t*>( ecs_get_id( m_world.c_ptr(), e.id(), m_motionId ) );
		std::memcpy( &state.prevActions, bytes, 8 );
		state.slots.resize( m_config.motions );
		std::memcpy( static_cast<void*>( state.slots.data() ), bytes + 8, size_t( m_config.motions ) * sizeof( MotionSlot ) );
	}
	return state;
}

void Simulation::SetMotionState( flecs::entity e, const MotionState& state )
{
	if ( m_config.motions == 0 )
	{
		return;
	}
	m_sizedScratch.resize( 8 + size_t( m_config.motions ) * sizeof( MotionSlot ) );
	std::memcpy( m_sizedScratch.data(), &state.prevActions, 8 );
	state.slots.copy_to( reinterpret_cast<MotionSlot*>( m_sizedScratch.data() + 8 ), m_config.motions );
	ecs_set_id( m_world.c_ptr(), e.id(), m_motionId, m_sizedScratch.size(), m_sizedScratch.data() );
}

void Simulation::SetMotions( std::shared_ptr<const Motions> motions )
{
	if ( motions != nullptr && motions->list.size() > size_t( m_config.motions ) )
	{
		std::fprintf( stderr, "Simulation: %zu motions, but its config has slots for %u: they are not run\n", motions->list.size(),
					  unsigned( m_config.motions ) );
		motions = nullptr;
	}
	m_motions = std::move( motions );
	m_itemMotions.clear();
	for ( size_t i = 0; m_motions && i < m_motions->list.size(); ++i )
	{
		const Motion& m = m_motions->list[i];
		if ( m.ownerItem && m.itemKind >= 0 )
		{
			if ( m_itemMotions.size() <= size_t( m.itemKind ) )
			{
				m_itemMotions.resize( size_t( m.itemKind ) + 1 );
			}
			m_itemMotions[size_t( m.itemKind )].push_back( uint16_t( i ) );
		}
	}
}

bool Simulation::HasAnimState( flecs::entity e ) const
{
	return ecs_has_id( m_world.c_ptr(), e.id(), m_animId );
}

AnimState Simulation::GetAnimState( flecs::entity e ) const
{
	AnimState state;
	if ( HasAnimState( e ) )
	{
		UnpackAnimState( static_cast<const uint8_t*>( ecs_get_id( m_world.c_ptr(), e.id(), m_animId ) ), m_config.layers, state );
	}
	return state;
}

// (Layers past the ones the config has are dropped: no state machine of this server has them.)
void Simulation::SetAnimState( flecs::entity e, const AnimState& state )
{
	m_sizedScratch.resize( AnimStateBytes( m_config.layers ) );
	PackAnimState( state, m_config.layers, m_sizedScratch.data() );
	ecs_set_id( m_world.c_ptr(), e.id(), m_animId, m_sizedScratch.size(), m_sizedScratch.data() );
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
	m_animId = RegisterSizedComponent( "AnimState", uint32_t( AnimStateBytes( m_config.layers ) ) );
	RegisterSnapComponent<TemplateRef>();
	m_boardId = RegisterSizedComponent( "Blackboard", uint32_t( m_config.fields ) * 4 );
	RegisterSnapComponent<Ragdoll>();
	RegisterSnapComponent<RagdollBodies>();
	RegisterSnapComponent<RagdollPose>();
	RegisterSnapComponent<HeldItem>();
	RegisterSnapComponent<MoveOverrides>();
	m_motionId = RegisterSizedComponent( "MotionState", m_config.motions > 0 ? 8 + uint32_t( m_config.motions ) * uint32_t( sizeof( MotionSlot ) ) : 0 );
	RegisterSnapComponent<MotionHold>();
	RegisterSnapComponent<Slots>();
	RegisterSnapComponent<Container>();
	RegisterSnapComponent<WallPiece>();
	RegisterSnapComponent<MoveEffects>();
	RegisterSnapComponent<MoveValues>();
	RegisterSnapComponent<CharacterKind>();
	RegisterSnapComponent<IdleBody>();
	RegisterSnapComponent<DrivenProp>();
	RegisterSnapComponent<ModDriven>();
	RegisterSnapComponent<Pusher>();

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
	IndexesStale();
	return e;
}

void Simulation::DestroyEntity( flecs::entity e )
{
	if ( e.has<IdleBody>() )
	{
		// A body nobody drives (fallen out of the world): what it carried goes with it.
		const uint32_t self = e.get<NetId>().value;
		for ( uint32_t item : CarriedItemsOf( self ) )
		{
			if ( flecs::entity it = FindEntity( item ); it.is_valid() && it != e )
			{
				DestroyEntity( it );
			}
		}
	}
	if ( e.has<Container>() && e.has<Transform>() )
	{
		// What a chest held falls out of it.
		const uint32_t self = e.get<NetId>().value;
		const Transform at = e.get<Transform>();
		std::vector<flecs::entity> held;
		for ( const EntityRef& r : m_entities )
		{
			flecs::entity item( m_world, r.entity );
			const HeldItem* h = item.try_get<HeldItem>();
			if ( h != nullptr && h->holder == self )
			{
				held.push_back( item );
			}
		}
		for ( size_t i = 0; i < held.size(); ++i )
		{
			b3Vec3 above = b3Add( at.position, b3Vec3{ 0.15f * float( i % 3 ), 0.8f + 0.25f * float( i / 3 ), 0.0f } );
			PutItemInWorld( held[i], above, at.rotation, b3Vec3{ 0.0f, 1.0f, 0.0f } );
		}
	}
	if ( const PhysicsBody* pb = e.try_get<PhysicsBody>() )
	{
		b3DestroyBody( BodyOf( *pb ) );
	}
	if ( const Pusher* pusher = e.try_get<Pusher>() )
	{
		b3BodyId body = pusher->body;
		body.world0 = uint16_t( m_physicsWorld.index1 - 1 );
		b3DestroyBody( body );
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
		IndexesStale();
	}
	e.destruct();
}

namespace
{

b3ShapeDef MakeShapeDef( uint64_t category, const ShapeMaterial& material )
{
	b3ShapeDef def = b3DefaultShapeDef();
	def.density = material.density;
	def.baseMaterial.friction = material.friction;
	def.baseMaterial.restitution = material.restitution;
	def.baseMaterial.rollingResistance = material.rolling;
	def.filter.categoryBits = category;
	// Characters touch no body in the physics step (their movers meet solid ones with casts, their
	// pushers shove the pushable ones): components.h, CollisionMask.
	def.filter.maskBits = CollisionMask( category );
	// Either shape enabling hit events is enough, so the static level does not need them.
	def.enableHitEvents = category != CatStatic;
	return def;
}

} // namespace

b3ShapeId Simulation::CreateHullShape( b3BodyId body, const std::vector<b3Vec3>& points, uint64_t category, const ShapeMaterial& material )
{
	b3HullData* hull = b3CreateHull( points.data(), int( points.size() ), B3_MAX_HULL_VERTICES );
	if ( hull == nullptr )
	{
		return {};
	}
	b3ShapeDef def = MakeShapeDef( category, material );
	b3ShapeId id = b3CreateHullShape( body, &def, hull ); // (the world keeps a copy)
	b3DestroyHull( hull );
	return id;
}

b3ShapeId Simulation::CreatePiece( b3BodyId body, uint64_t category, const ShapeMaterial& material, ShapeKind kind, b3Vec3 h,
									b3Transform at, const std::vector<b3Vec3>* points, b3Vec3& bounds )
{
	const b3ShapeDef def = MakeShapeDef( category, material );
	auto reach = [&]( b3Vec3 point ) {
		bounds = { std::max( bounds.x, std::fabs( point.x ) ), std::max( bounds.y, std::fabs( point.y ) ),
				   std::max( bounds.z, std::fabs( point.z ) ) };
	};
	b3ShapeId id = {};
	switch ( kind )
	{
		case ShapeKind::Box:
		{
			b3BoxHull hull = b3MakeTransformedBoxHull( h.x, h.y, h.z, at );
			id = b3CreateHullShape( body, &def, &hull.base );
			for ( int corner = 0; corner < 8; ++corner )
			{
				reach( b3TransformPoint( at, { corner & 1 ? h.x : -h.x, corner & 2 ? h.y : -h.y, corner & 4 ? h.z : -h.z } ) );
			}
			break;
		}
		case ShapeKind::Sphere:
		{
			b3Sphere sphere = { at.p, h.x };
			id = b3CreateSphereShape( body, &def, &sphere );
			reach( b3Add( at.p, { h.x, h.x, h.x } ) );
			reach( b3Sub( at.p, { h.x, h.x, h.x } ) );
			break;
		}
		case ShapeKind::Capsule:
		{
			b3Capsule capsule = { b3TransformPoint( at, { 0.0f, -h.y, 0.0f } ), b3TransformPoint( at, { 0.0f, h.y, 0.0f } ), h.x };
			id = b3CreateCapsuleShape( body, &def, &capsule );
			for ( b3Vec3 end : { capsule.center1, capsule.center2 } )
			{
				reach( b3Add( end, { h.x, h.x, h.x } ) );
				reach( b3Sub( end, { h.x, h.x, h.x } ) );
			}
			break;
		}
		case ShapeKind::Hull:
		{
			if ( points == nullptr )
			{
				break;
			}
			b3HullData* hull = b3CreateHull( points->data(), int( points->size() ), B3_MAX_HULL_VERTICES );
			if ( hull != nullptr )
			{
				id = b3CreateTransformedHullShape( body, &def, hull, at, { 1.0f, 1.0f, 1.0f } );
				b3DestroyHull( hull );
			}
			for ( const b3Vec3& point : *points )
			{
				reach( b3TransformPoint( at, point ) );
			}
			break;
		}
		case ShapeKind::Mesh:
		case ShapeKind::HeightField:
			break; // level pieces only (static): never a body's part
	}
	return id;
}

b3ShapeId Simulation::CreateShapeSet( b3BodyId body, const std::vector<LevelBox>& set, uint64_t category, const ShapeMaterial& material,
									   b3Vec3& bounds )
{
	b3ShapeId first = {};
	bounds = { 0.0f, 0.0f, 0.0f };
	for ( const LevelBox& piece : set )
	{
		const b3Transform at = { piece.center, MakeRotation( piece.yaw, piece.pitch, piece.roll ) };
		const std::vector<b3Vec3>* points = piece.kind == ShapeKind::Hull && piece.hull < m_map.hulls.size() ? &m_map.hulls[piece.hull] : nullptr;
		b3ShapeId id = CreatePiece( body, category, material, piece.kind, piece.halfExtents, at, points, bounds );
		first = first.index1 == 0 ? id : first;
	}
	return first;
}

b3ShapeId Simulation::CreateSurfaceShape( b3BodyId body, const LevelBox& piece )
{
	const LevelLayout& layout = m_map;
	if ( m_levelArena == nullptr )
	{
		// As much as the level's surfaces could take while they are built (their trees, the work).
		size_t bytes = size_t( 16 ) << 20;
		for ( const LevelMesh& mesh : layout.meshes )
		{
			bytes += mesh.vertices.size() * 64 + mesh.indices.size() * 64;
		}
		for ( const LevelHeightField& field : layout.heightFields )
		{
			bytes += field.heights.size() * 16;
		}
		m_levelArena = std::make_unique<PhysicsArena>( bytes );
	}
	const b3ShapeDef def = MakeShapeDef( CatStatic, {} );
	if ( piece.kind == ShapeKind::Mesh && piece.hull < layout.meshes.size() )
	{
		const LevelMesh& mesh = layout.meshes[piece.hull];
		b3MeshData* data = nullptr;
		{
			PhysicsArena::Scope level( *m_levelArena );
			b3MeshDef meshDef = {};
			meshDef.vertices = const_cast<b3Vec3*>( mesh.vertices.data() );
			meshDef.indices = reinterpret_cast<int32_t*>( const_cast<uint32_t*>( mesh.indices.data() ) );
			meshDef.vertexCount = int( mesh.vertices.size() );
			meshDef.triangleCount = int( mesh.indices.size() / 3 );
			meshDef.identifyEdges = true;
			data = b3CreateMesh( &meshDef, nullptr, 0 );
		}
		return data != nullptr ? b3CreateMeshShape( body, &def, data, { 1.0f, 1.0f, 1.0f } ) : b3ShapeId{};
	}
	if ( piece.kind == ShapeKind::HeightField && piece.hull < layout.heightFields.size() )
	{
		const LevelHeightField& field = layout.heightFields[piece.hull];
		b3HeightFieldData* data = nullptr;
		{
			PhysicsArena::Scope level( *m_levelArena );
			std::vector<uint8_t> materials( size_t( field.countX - 1 ) * ( field.countZ - 1 ), 0 );
			for ( size_t cell = 0; cell < field.holes.size() && cell < materials.size(); ++cell )
			{
				materials[cell] = field.holes[cell] != 0 ? uint8_t( B3_HEIGHT_FIELD_HOLE ) : uint8_t( 0 );
			}
			float lowest = field.heights.empty() ? 0.0f : field.heights[0], highest = lowest;
			for ( float height : field.heights )
			{
				lowest = std::min( lowest, height );
				highest = std::max( highest, height );
			}
			b3HeightFieldDef fieldDef = {};
			fieldDef.heights = const_cast<float*>( field.heights.data() );
			fieldDef.materialIndices = materials.data();
			fieldDef.scale = field.scale;
			fieldDef.countX = int( field.countX );
			fieldDef.countZ = int( field.countZ );
			fieldDef.globalMinimumHeight = lowest;
			fieldDef.globalMaximumHeight = highest > lowest ? highest : lowest + 1.0f;
			data = b3CreateHeightField( &fieldDef );
		}
		return data != nullptr ? b3CreateHeightFieldShape( body, &def, data ) : b3ShapeId{};
	}
	return {};
}

void Simulation::WeighBody( b3BodyId body, float mass )
{
	if ( mass <= 0.0f || b3Body_GetMass( body ) <= 0.0f )
	{
		return;
	}
	const float scale = mass / b3Body_GetMass( body );
	b3ShapeId shapes[kMapSetPieceLimit];
	const int count = b3Body_GetShapes( body, shapes, int( kMapSetPieceLimit ) );
	for ( int i = 0; i < count; ++i )
	{
		b3Shape_SetDensity( shapes[i], b3Shape_GetDensity( shapes[i] ) * scale, false );
	}
	b3Body_ApplyMassFromShapes( body );
}

b3ShapeId Simulation::CreateShape( b3BodyId body, const Shape& shape, uint64_t category, const ShapeMaterial& material )
{
	b3ShapeDef def = MakeShapeDef( category, material );

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
		case ShapeKind::Hull:
			break; // its points are not in the Shape: CreateHullShape
		case ShapeKind::Mesh:
		case ShapeKind::HeightField:
			break; // level pieces only: CreateSurfaceShape
	}
	return {};
}

void Simulation::BuildLevel()
{
	const LevelLayout& layout = m_map;

	for ( const LevelBox& box : layout.statics )
	{
		Transform t{ box.center, MakeRotation( box.yaw, box.pitch, box.roll ) };
		Shape shape{ box.kind, {}, box.halfExtents };

		b3BodyDef bodyDef = b3DefaultBodyDef();
		bodyDef.type = b3_staticBody;
		bodyDef.position = t.position;
		bodyDef.rotation = t.rotation;
		b3BodyId body = b3CreateBody( m_physicsWorld, &bodyDef );
		// (A map's hulls were built once when it was read, so they build here.)
		b3ShapeId shapeId = box.kind == ShapeKind::Hull && box.hull < layout.hulls.size() ? CreateHullShape( body, layout.hulls[box.hull], CatStatic )
							: box.kind == ShapeKind::Mesh || box.kind == ShapeKind::HeightField ? CreateSurfaceShape( body, box )
																								: CreateShape( body, shape, CatStatic );

		flecs::entity e = CreateEntity();
		e.set<Transform>( t );
		e.set<Shape>( shape );
		e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
		e.add<StaticGeometry>();
	}

	for ( const LevelProp& prop : layout.props )
	{
		// Its body as the map says: a weight it was given is a density for its size.
		ShapeMaterial material{ prop.density, prop.friction, prop.restitution, prop.rolling, prop.drag };
		if ( prop.mass > 0.0f )
		{
			const b3Vec3 h = prop.halfExtents;
			float volume = prop.kind == ShapeKind::Sphere ? 4.18879f * h.x * h.x * h.x : 8.0f * h.x * h.y * h.z;
			material.density = prop.mass / std::max( volume, 1e-6f );
		}
		CreateProp( prop.kind, prop.position, b3Quat{ { 0.0f, 0.0f, 0.0f }, 1.0f }, prop.halfExtents, { 0.0f, 0.0f, 0.0f }, 0, 0, material );
	}

	// Template instances come last, in the order the map lists them.
	for ( const LevelInstance& instance : layout.instances )
	{
		const b3Quat rotation = MakeRotation( instance.yaw, instance.pitch, instance.roll );
		if ( instance.templateIndex < layout.templates.size() && layout.templates[instance.templateIndex].glue > 0.0f &&
			 layout.templates[instance.templateIndex].pieces.empty() == false )
		{
			CreateGluedWall( instance.templateIndex, instance.position, rotation );
			continue;
		}
		CreateFromTemplate( instance.templateIndex, instance.position, rotation, { 0.0f, 0.0f, 0.0f }, 0 );
	}
}

flecs::entity Simulation::CreateProp( ShapeKind kind, b3Vec3 position, b3Quat rotation, b3Vec3 halfExtents, b3Vec3 velocity,
									  uint32_t owner, uint32_t lifetimeTicks, const ShapeMaterial& material )
{
	flecs::entity e = CreateEntity();
	Shape shape{ kind, {}, halfExtents };

	b3BodyDef bodyDef = b3DefaultBodyDef();
	bodyDef.type = b3_dynamicBody;
	bodyDef.position = position;
	bodyDef.rotation = rotation;
	bodyDef.linearVelocity = velocity;
	bodyDef.linearDamping = material.drag;
	bodyDef.angularDamping = material.drag;
	b3BodyId body = b3CreateBody( m_physicsWorld, &bodyDef );
	b3ShapeId shapeId = CreateShape( body, shape, CatProp, material );

	uint32_t despawn = lifetimeTicks > 0 ? m_globals.tick + lifetimeTicks : 0;
	e.set<Transform>( { position, rotation } );
	e.set<Velocity>( { velocity, { 0.0f, 0.0f, 0.0f } } );
	e.set<Shape>( shape );
	e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
	e.set<Prop>( { owner, m_globals.tick, despawn } );
	return e;
}

flecs::entity Simulation::CreateFromTemplate( uint32_t templateIndex, b3Vec3 position, b3Quat rotation, b3Vec3 extraVelocity,
											  uint32_t owner, bool standing )
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
	bodyDef.type = standing ? b3_staticBody : b3BodyType( bodyType );
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

	// Slots of its own (a chest), set below once it is made.
	constexpr uint32_t kContainer = Fnv32( "Container" );
	const bool isContainer = TemplateHas( t, kContainer );
	const uint8_t containerSlots = uint8_t( std::clamp( TemplateInt( t, kContainer, Fnv32( "slots" ), 9 ), 1, kMaxSlots ) );
	constexpr uint32_t kMaterial = Fnv32( "Material" );
	ShapeMaterial material;
	material.density = TemplateFloat( t, kMaterial, Fnv32( "density" ), material.density );
	material.friction = TemplateFloat( t, kMaterial, Fnv32( "friction" ), material.friction );
	material.restitution = TemplateFloat( t, kMaterial, Fnv32( "restitution" ), material.restitution );
	material.rolling = TemplateFloat( t, kMaterial, Fnv32( "rolling" ), material.rolling );

	bool isStatic = bodyDef.type == b3_staticBody;
	const uint64_t category = isStatic									 ? CatStatic
							  : TemplateHas( t, Fnv32( "Debris" ) )	 ? CatDebris
							  : TemplateHas( t, Fnv32( "Pushable" ) ) ? CatPushable
																	 : CatProp;
	b3BodyId body = b3CreateBody( m_physicsWorld, &bodyDef );
	b3ShapeId shapeId;
	if ( t.shapes < m_map.shapeSets.size() )
	{
		// Its own shapes. What the Shape component would have drawn is the box that holds them.
		b3Vec3 bounds;
		shapeId = CreateShapeSet( body, m_map.shapeSets[t.shapes], category, material, bounds );
		shape.kind = ShapeKind::Box;
		shape.halfExtents = bounds;
	}
	else
	{
		shapeId = CreateShape( body, shape, category, material );
	}
	// A weight of its own: the shapes' densities scaled so the body weighs it.
	if ( isStatic == false )
	{
		WeighBody( body, TemplateFloat( t, kMaterial, Fnv32( "mass" ), 0.0f ) );
	}

	flecs::entity e = CreateEntity();
	e.set<Transform>( { position, rotation } );
	e.set<Velocity>( { linear, angular } );
	e.set<Shape>( shape );
	e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
	e.set<TemplateRef>( { templateIndex } );
	if ( isContainer )
	{
		e.set<Container>( { containerSlots, {} } );
	}

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

void Simulation::CreateGluedWall( uint32_t templateIndex, b3Vec3 position, b3Quat rotation )
{
	const EntityTemplate& wt = m_map.templates[templateIndex];
	const size_t n = wt.pieces.size();
	std::vector<flecs::entity> pieces( n );
	std::vector<b3Transform> at( n );
	uint32_t wallId = 0;
	for ( size_t i = 0; i < n; ++i )
	{
		const TemplatePiece& piece = wt.pieces[i];
		at[i] = { b3Add( position, b3RotateVector( rotation, piece.position ) ),
				  b3NormalizeQuat( b3MulQuat( rotation, MakeRotation( piece.yaw, piece.pitch, piece.roll ) ) ) };
		pieces[i] = CreateFromTemplate( piece.templateIndex, at[i].p, at[i].q, { 0.0f, 0.0f, 0.0f }, 0, piece.held );
		if ( pieces[i].is_valid() && wallId == 0 )
		{
			wallId = pieces[i].get<NetId>().value; // (the wall is known by its first piece)
		}
	}
	for ( size_t i = 0; i < n; ++i )
	{
		if ( pieces[i].is_valid() )
		{
			pieces[i].set<WallPiece>( { wallId, templateIndex, uint32_t( i ) } );
		}
	}
	// Welds where they touch (not between two held pieces: both are static).
	for ( const PieceLink& link : wt.links )
	{
		if ( link.a >= n || link.b >= n || pieces[link.a].is_valid() == false || pieces[link.b].is_valid() == false ||
			 ( wt.pieces[link.a].held && wt.pieces[link.b].held ) )
		{
			continue;
		}
		// (Box3D wants a static body first.)
		const uint16_t a = wt.pieces[link.b].held ? link.b : link.a;
		const uint16_t b = a == link.a ? link.b : link.a;
		b3WeldJointDef def = b3DefaultWeldJointDef();
		def.base.bodyIdA = BodyOf( pieces[a].get<PhysicsBody>() );
		def.base.bodyIdB = BodyOf( pieces[b].get<PhysicsBody>() );
		def.base.localFrameA = b3Transform_identity;
		def.base.localFrameB = b3InvMulTransforms( at[b], at[a] );
		def.base.forceThreshold = wt.glue;
		def.base.torqueThreshold = wt.glue; // (newton metres: as if pulled a metre from the weld)
		b3CreateWeldJoint( m_physicsWorld, &def );
	}
}

void Simulation::BreakGlue()
{
	b3JointEvents events = b3World_GetJointEvents( m_physicsWorld );
	if ( events.count <= 0 )
	{
		return;
	}
	// (Box3D says them in an order of its own: by the joints' ids, the same everywhere.)
	std::vector<b3JointId> overloaded;
	for ( int i = 0; i < events.count; ++i )
	{
		overloaded.push_back( events.jointEvents[i].jointId );
	}
	std::sort( overloaded.begin(), overloaded.end(), []( b3JointId a, b3JointId b ) { return a.index1 < b.index1; } );
	for ( b3JointId joint : overloaded )
	{
		if ( b3Joint_IsValid( joint ) && b3Joint_GetType( joint ) == b3_weldJoint )
		{
			b3DestroyJoint( joint, true );
		}
	}
	LoosenPieces();
}

void Simulation::LoosenPieces()
{
	std::vector<flecs::entity> loose;
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		const PhysicsBody* pb = e.has<WallPiece>() ? e.try_get<PhysicsBody>() : nullptr;
		if ( pb != nullptr && b3Body_GetType( BodyOf( *pb ) ) == b3_dynamicBody && b3Body_GetJointCount( BodyOf( *pb ) ) == 0 )
		{
			loose.push_back( e );
		}
	}
	for ( flecs::entity e : loose )
	{
		e.remove<WallPiece>();
	}
}

void Simulation::BreakWall( flecs::entity target, b3Vec3 blast, float radius, float speed, uint32_t rubbleTicks )
{
	if ( m_followsServer )
	{
		return; // what breaks is the server's to say (its state brings the pieces)
	}
	// The wall: whole (a template with pieces), or a piece of it left standing by a blast before.
	uint32_t wallId = 0;
	uint32_t wallTemplate = kNoTemplate;
	bool whole = false;
	if ( const WallPiece* piece = target.try_get<WallPiece>() )
	{
		wallId = piece->wall;
		wallTemplate = piece->wallTemplate;
	}
	else if ( const TemplateRef* ref = target.try_get<TemplateRef>() )
	{
		wallId = target.get<NetId>().value;
		wallTemplate = ref->index;
		whole = true;
	}
	if ( wallTemplate >= m_map.templates.size() || m_map.templates[wallTemplate].pieces.empty() )
	{
		return; // nothing that breaks
	}
	const EntityTemplate& wt = m_map.templates[wallTemplate];
	const size_t n = wt.pieces.size();

	// The pieces that stand, by their index; for a whole wall, none yet.
	std::vector<flecs::entity> stand( n );
	if ( whole == false )
	{
		for ( const EntityRef& r : m_entities )
		{
			flecs::entity e( m_world, r.entity );
			const WallPiece* piece = e.try_get<WallPiece>();
			if ( piece != nullptr && piece->wall == wallId && piece->piece < n )
			{
				stand[piece->piece] = e;
			}
		}
	}
	// A piece that goes: a body of its own, moving as it is sent (removed after a while, if asked).
	auto fly = [&]( size_t i, b3Vec3 position, b3Quat rotation, b3Vec3 velocity ) {
		flecs::entity e = CreateFromTemplate( wt.pieces[i].templateIndex, position, rotation, velocity, 0 );
		if ( e.is_valid() && rubbleTicks > 0 )
		{
			e.set<Prop>( { 0, m_globals.tick, m_globals.tick + rubbleTicks } );
		}
	};
	// Away from the blast, a little up, slower the further it was.
	auto thrown = [&]( b3Vec3 position, float& distance ) {
		b3Vec3 away = b3Sub( position, blast );
		distance = b3Length( away );
		away = distance > 1e-4f ? b3MulSV( 1.0f / distance, away ) : b3Vec3{ 0.0f, 1.0f, 0.0f };
		away = b3Normalize( b3Add( away, { 0.0f, 0.3f, 0.0f } ) );
		return b3MulSV( speed / ( 1.0f + distance ), away );
	};

	if ( whole )
	{
		// It comes apart: the pieces near the blast fly, the rest stand where they were.
		const Transform at = target.get<Transform>();
		DestroyEntity( target );
		for ( size_t i = 0; i < n; ++i )
		{
			const TemplatePiece& piece = wt.pieces[i];
			const b3Vec3 position = b3Add( at.position, b3RotateVector( at.rotation, piece.position ) );
			const b3Quat rotation = b3NormalizeQuat( b3MulQuat( at.rotation, MakeRotation( piece.yaw, piece.pitch, piece.roll ) ) );
			float distance = 0.0f;
			const b3Vec3 velocity = thrown( position, distance );
			if ( radius <= 0.0f || distance <= radius )
			{
				fly( i, position, rotation, velocity );
				continue;
			}
			flecs::entity e = CreateFromTemplate( piece.templateIndex, position, rotation, { 0.0f, 0.0f, 0.0f }, 0, true );
			if ( e.is_valid() )
			{
				e.set<WallPiece>( { wallId, wallTemplate, uint32_t( i ) } );
				stand[i] = e;
			}
		}
	}
	else
	{
		// What still stands near the blast goes too.
		for ( size_t i = 0; i < n; ++i )
		{
			if ( stand[i].is_valid() == false )
			{
				continue;
			}
			const Transform at = stand[i].get<Transform>();
			float distance = 0.0f;
			const b3Vec3 velocity = thrown( at.position, distance );
			if ( radius > 0.0f && distance > radius )
			{
				continue;
			}
			const b3BodyId body = BodyOf( stand[i].get<PhysicsBody>() );
			if ( b3Body_GetType( body ) == b3_dynamicBody )
			{
				// A glued piece: its welds let go, and it flies as it is.
				std::vector<b3JointId> welds( size_t( b3Body_GetJointCount( body ) ) );
				welds.resize( size_t( b3Body_GetJoints( body, welds.data(), int( welds.size() ) ) ) );
				for ( b3JointId weld : welds )
				{
					b3DestroyJoint( weld, true );
				}
				b3Body_SetLinearVelocity( body, velocity );
				b3Body_SetAwake( body, true );
				stand[i].remove<WallPiece>();
				if ( rubbleTicks > 0 )
				{
					stand[i].set<Prop>( { 0, m_globals.tick, m_globals.tick + rubbleTicks } );
				}
				stand[i] = flecs::entity();
				continue;
			}
			DestroyEntity( stand[i] );
			stand[i] = flecs::entity();
			fly( i, at.position, at.rotation, velocity );
		}
	}
	if ( wt.glue > 0.0f )
	{
		// What holds a glued wall up is its welds: the physics says what falls. A piece left with none
		// is loose.
		LoosenPieces();
		return;
	}

	// What holds it up: from the held pieces still standing, along the links between standing
	// pieces. A piece nothing joins to one falls (it starts still; gravity takes it).
	std::vector<std::vector<uint32_t>> touching( n );
	for ( const PieceLink& link : wt.links )
	{
		if ( link.a < n && link.b < n )
		{
			touching[link.a].push_back( link.b );
			touching[link.b].push_back( link.a );
		}
	}
	std::vector<uint8_t> held( n, 0 );
	std::vector<uint32_t> reach;
	for ( size_t i = 0; i < n; ++i )
	{
		if ( stand[i].is_valid() && wt.pieces[i].held )
		{
			held[i] = 1;
			reach.push_back( uint32_t( i ) );
		}
	}
	while ( reach.empty() == false )
	{
		const uint32_t i = reach.back();
		reach.pop_back();
		for ( uint32_t j : touching[i] )
		{
			if ( held[j] == 0 && stand[j].is_valid() )
			{
				held[j] = 1;
				reach.push_back( j );
			}
		}
	}
	for ( size_t i = 0; i < n; ++i )
	{
		if ( stand[i].is_valid() && held[i] == 0 )
		{
			const Transform at = stand[i].get<Transform>();
			DestroyEntity( stand[i] );
			stand[i] = flecs::entity();
			fly( i, at.position, at.rotation, { 0.0f, 0.0f, 0.0f } );
		}
	}
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
	bodyDef.isBullet = look.bullet != 0;
	bodyDef.linearDamping = look.drag;
	bodyDef.angularDamping = look.drag;
	b3BodyId body = b3CreateBody( m_physicsWorld, &bodyDef );
	ShapeMaterial material;
	material.density = look.mass / std::max( volume, 1e-6f );
	material.friction = look.friction;
	material.restitution = look.restitution;
	material.rolling = look.rolling;
	b3ShapeId shapeId;
	if ( look.parts.empty() == false )
	{
		// Its parts, each where it is in the item's frame (the body sits there: no centre, no turn),
		// and its weight shared among them.
		b3Vec3 bounds = { 0.0f, 0.0f, 0.0f };
		shapeId = {};
		for ( const ItemPart& part : look.parts )
		{
			std::vector<b3Vec3> points;
			for ( const Float3& point : part.points )
			{
				points.push_back( { point.x, point.y, point.z } );
			}
			const b3Transform at = { { part.center.x, part.center.y, part.center.z },
									 b3NormalizeQuat( { { part.turn[0], part.turn[1], part.turn[2] }, part.turn[3] } ) };
			b3ShapeId id =
				CreatePiece( body, CatPushable, material, ShapeKind( part.kind ), { part.half.x, part.half.y, part.half.z }, at, &points, bounds );
			shapeId = shapeId.index1 == 0 ? id : shapeId;
		}
		WeighBody( body, look.mass );
		shape.kind = ShapeKind::Box;
		shape.halfExtents = bounds;
	}
	else
	{
		shapeId = CreateShape( body, shape, CatPushable, material ); // (an item lying about is kicked aside)
	}

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
	// (One a motion threw is nobody's throw once it is taken up, and does not expire in the hand.)
	item.remove<Prop>();
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

void Simulation::RegisterIndexes()
{
	// Bodies: a shape made or gone changes which shape is whose.
	m_world.observer<const PhysicsBody>( "ShapeLookupStale" ).event( flecs::OnSet ).event( flecs::OnRemove ).each(
		[this]( flecs::entity, const PhysicsBody& ) { m_shapeLookupStale = true; } );
	m_world.observer<const RagdollBodies>( "RagdollLookupStale" ).event( flecs::OnSet ).event( flecs::OnRemove ).each(
		[this]( flecs::entity, const RagdollBodies& ) { m_shapeLookupStale = true; } );
	// Items: only a change of holder matters (a slot or a socket is read fresh at each use).
	m_world.observer<const HeldItem>( "HolderChanged" ).event( flecs::OnSet ).each( [this]( flecs::entity e, const HeldItem& item ) {
		auto known = m_holderOf.find( e.id() );
		if ( known == m_holderOf.end() || known->second != item.holder )
		{
			m_itemsByHolderStale = true;
		}
	} );
	m_world.observer<const HeldItem>( "HolderGone" ).event( flecs::OnRemove ).each(
		[this]( flecs::entity, const HeldItem& ) { m_itemsByHolderStale = true; } );
}

const std::vector<Simulation::EntityRef>& Simulation::ItemsOf( uint32_t holder ) const
{
	if ( m_itemsByHolderStale )
	{
		m_itemsByHolder.clear();
		auto& holderOf = const_cast<std::unordered_map<flecs::entity_t, uint32_t>&>( m_holderOf );
		holderOf.clear();
		for ( const EntityRef& r : m_entities )
		{
			if ( const HeldItem* item = flecs::entity( m_world, r.entity ).try_get<HeldItem>() )
			{
				holderOf[r.entity] = item->holder;
				if ( item->holder != 0 )
				{
					m_itemsByHolder[item->holder].push_back( r );
				}
			}
		}
		m_itemsByHolderStale = false;
	}
	static const std::vector<EntityRef> none;
	auto found = m_itemsByHolder.find( holder );
	return found != m_itemsByHolder.end() ? found->second : none;
}

uint32_t Simulation::SlotItemOf( uint32_t holder, uint8_t slot ) const
{
	if ( holder == 0 || slot == kNoSlot )
	{
		return 0;
	}
	uint32_t found = 0;
	for ( const EntityRef& r : ItemsOf( holder ) )
	{
		const HeldItem* item = flecs::entity( m_world, r.entity ).try_get<HeldItem>();
		if ( item != nullptr && item->holder == holder && item->slot == slot )
		{
			found = r.netId;
			break;
		}
	}
	if ( s_checkIndexes )
	{
		uint32_t scanned = 0;
		for ( const EntityRef& r : m_entities )
		{
			const HeldItem* item = flecs::entity( m_world, r.entity ).try_get<HeldItem>();
			if ( item != nullptr && item->holder == holder && item->slot == slot )
			{
				scanned = r.netId;
				break;
			}
		}
		if ( scanned != found )
		{
			std::fprintf( stderr, "SlotItemOf( %u, %u ): the index says %u, every entity %u\n", holder, unsigned( slot ), found, scanned );
			std::abort();
		}
	}
	return found;
}

void Simulation::SettleSlots( uint32_t holder, uint8_t selected )
{
	// The left hand: for the item of the lowest slot that goes there (ItemShape::hand), unless what
	// is selected takes it: its other hand on a marker (a rifle's barrel), or it is in that hand
	// itself. One whose other hand is only as the animation has it (a pistol, a bat) gives it up,
	// and is held with one hand while it is there.
	uint8_t leftSlot = kNoSlot;
	bool leftFree = true;
	// (A copy: setting an item below may make the index stale and rebuild it.)
	const std::vector<EntityRef> items = ItemsOf( holder );
	for ( const EntityRef& r : items )
	{
		const HeldItem* item = flecs::entity( m_world, r.entity ).try_get<HeldItem>();
		if ( item == nullptr || item->holder != holder || item->slot == kNoSlot )
		{
			continue;
		}
		const ItemShape& shape = ItemShapeOf( item->kind );
		if ( item->slot == selected )
		{
			leftFree = ( shape.grip == 0 || shape.grip == 3 ) && shape.hand != kItemHandLeft;
		}
		else if ( shape.hand == kItemHandLeft && item->slot < leftSlot )
		{
			leftSlot = item->slot;
		}
	}
	for ( const EntityRef& r : items )
	{
		flecs::entity e( m_world, r.entity );
		const HeldItem* item = e.try_get<HeldItem>();
		if ( item == nullptr || item->holder != holder || item->slot == kNoSlot )
		{
			continue;
		}
		const ItemShape& shape = ItemShapeOf( item->kind );
		bool left = shape.hand == kItemHandLeft;
		HeldItem want = *item;
		want.stowed = item->slot == selected || ( leftFree && item->slot == leftSlot ) ? 0 : 1;
		want.socket = want.stowed != 0 ? shape.holster : left ? uint8_t( kSocketLeftHand ) : m_config.slotHand;
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
	QueueIntoWorld( item, grip, detmath::YawRotation( yaw ), b3Add( b3MulSV( 1.0f, ahead ), b3Vec3{ 0.0f, 1.5f, 0.0f } ) );
}

bool Simulation::MergeStacks( flecs::entity from, flecs::entity onto, uint32_t count )
{
	const HeldItem* a = from.try_get<HeldItem>();
	const HeldItem* b = onto.try_get<HeldItem>();
	const ItemShape rule = a != nullptr ? ItemShapeOf( a->kind ) : ItemShape{};
	if ( a == nullptr || b == nullptr || from == onto || a->kind != b->kind || rule.stack <= 1 || StackOf( *b ) >= rule.stack )
	{
		return false;
	}
	uint32_t moved = std::min( { count, StackOf( *a ), uint32_t( rule.stack ) - StackOf( *b ) } );
	HeldItem grown = *b;
	grown.count = uint8_t( StackOf( *b ) + moved );
	onto.set<HeldItem>( grown );
	if ( moved >= StackOf( *a ) )
	{
		DestroyEntity( from );
	}
	else
	{
		HeldItem rest = *a;
		rest.count = uint8_t( StackOf( *a ) - moved );
		from.set<HeldItem>( rest );
	}
	return true;
}

bool Simulation::PutInContainer( flecs::entity box, flecs::entity item )
{
	const uint32_t holder = box.get<NetId>().value;
	const uint8_t count = box.get<Container>().slots;
	for ( uint8_t s = 0; s < count; ++s )
	{
		flecs::entity onto = FindEntity( SlotItemOf( holder, s ) );
		if ( onto.is_valid() && MergeStacks( item, onto, StackOf( item.get<HeldItem>() ) ) && item.is_alive() == false )
		{
			return true;
		}
	}
	for ( uint8_t s = 0; s < count; ++s )
	{
		if ( SlotItemOf( holder, s ) == 0 )
		{
			HeldItem h = item.get<HeldItem>();
			h.holder = holder;
			h.slot = s;
			h.stowed = 1;
			h.socket = kNoSocket;
			item.set<HeldItem>( h );
			return true;
		}
	}
	return false;
}

bool Simulation::GiveSlot( flecs::entity player, flecs::entity item, bool select )
{
	uint32_t holder = player.get<NetId>().value;
	Slots slots = player.has<Slots>() ? player.get<Slots>() : Slots{ m_config.slots, kNoSlot, 0, 0 };
	HeldItem held = item.get<HeldItem>();
	const ItemShape rule = ItemShapeOf( held.kind );
	// Onto a stack of its kind with room, the lowest slot's first: as many as fit. All of it: the
	// item it was is gone. What does not fit goes on as an item of its own.
	for ( uint8_t s = 0; rule.stack > 1 && s < slots.count; ++s )
	{
		flecs::entity onto = FindEntity( SlotItemOf( holder, s ) );
		const HeldItem* stack = onto.is_valid() && onto != item ? onto.try_get<HeldItem>() : nullptr;
		if ( stack == nullptr || stack->kind != held.kind || StackOf( *stack ) >= rule.stack )
		{
			continue;
		}
		uint32_t moved = std::min( StackOf( held ), uint32_t( rule.stack ) - StackOf( *stack ) );
		HeldItem grown = *stack;
		grown.count = uint8_t( StackOf( *stack ) + moved );
		onto.set<HeldItem>( grown );
		if ( moved >= StackOf( held ) )
		{
			DestroyEntity( item );
			if ( select )
			{
				slots.selected = s;
				player.set<Slots>( slots );
				SettleSlots( holder, slots.selected );
			}
			return true;
		}
		held.count = uint8_t( StackOf( held ) - moved );
		item.set<HeldItem>( held );
	}
	// The kind's slot, if it says one and it is free; else the first free one. What a slot holds
	// stays there: a slot is a place, and two of a kind are two items, wherever they are.
	int slot = -1;
	if ( rule.slot >= 1 && rule.slot <= slots.count && SlotItemOf( holder, uint8_t( rule.slot - 1 ) ) == 0 )
	{
		slot = rule.slot - 1;
	}
	for ( int s = 0; s < int( slots.count ) && slot < 0; ++s )
	{
		slot = SlotItemOf( holder, uint8_t( s ) ) == 0 ? s : -1;
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

void Simulation::ItemUses( uint32_t player, const PlayerInput& in, uint8_t prevButtons, ActionBits prevActions, std::vector<ItemUse>& out ) const
{
	out.clear();
	flecs::entity e = FindEntity( player );
	const Slots* slots = e.is_valid() ? e.try_get<Slots>() : nullptr;
	const Character* c = e.is_valid() ? e.try_get<Character>() : nullptr;
	if ( slots == nullptr || c == nullptr || c->dead != 0 || c->frozen != 0 )
	{
		return;
	}
	auto add = [&]( uint32_t item, uint8_t use, bool down, bool press ) {
		bool used = press;
		bool inUse = use == kItemUsePress ? press : down;
		if ( item == 0 || ( used == false && inUse == false ) )
		{
			return;
		}
		for ( ItemUse& had : out )
		{
			if ( had.item == item )
			{
				had.used |= used;
				had.inUse |= inUse;
				return;
			}
		}
		out.push_back( { item, used, inUse } );
	};
	// The use button: what is in the hand.
	if ( uint32_t hand = SlotItemOf( player, slots->selected ); hand != 0 )
	{
		bool down = ( in.buttons & BtnUse ) != 0;
		add( hand, ItemShapeOf( FindEntity( hand ).get<HeldItem>().kind ).use, down, down && ( prevButtons & BtnUse ) == 0 );
	}
	// Its slot's key, for a kind that says so (ItemShape::slotKeyUses): the slot's item, where it is.
	for ( uint8_t index = 0; index < slots->count && index < 16; ++index )
	{
		const uint16_t bit = uint16_t( 1u << index );
		const uint32_t item = ( in.slotKeys & bit ) != 0 ? SlotItemOf( player, index ) : 0;
		const uint16_t kind = item != 0 ? FindEntity( item ).get<HeldItem>().kind : uint16_t( 0 );
		if ( item != 0 && kind < m_itemShapes.size() && m_itemShapes[kind].slotKeyUses != 0 )
		{
			add( item, m_itemShapes[kind].use, true, ( slots->prevKeys & bit ) == 0 );
		}
	}
	// A kind's own key: the first copy carried (the selected one, else the lowest slot's), where it is.
	for ( size_t kind = 0; kind < m_itemShapes.size(); ++kind )
	{
		const uint8_t bit = m_itemShapes[kind].useAction;
		if ( bit >= kMaxActions || ( ( in.actions >> bit ) & 1 ) == 0 )
		{
			continue;
		}
		uint32_t copy = 0;
		uint8_t at = kNoSlot;
		for ( const EntityRef& r : m_entities )
		{
			const HeldItem* item = flecs::entity( m_world, r.entity ).try_get<HeldItem>();
			if ( item == nullptr || item->holder != player || item->kind != kind || item->slot == kNoSlot || item->slot >= slots->count )
			{
				continue;
			}
			if ( copy == 0 || item->slot == slots->selected || ( at != slots->selected && item->slot < at ) )
			{
				copy = r.netId;
				at = item->slot;
			}
		}
		add( copy, m_itemShapes[kind].use, true, ( ( prevActions >> bit ) & 1 ) == 0 );
	}
}

void Simulation::StepSlots( const InputFrame& frame )
{
	m_usingScratch.clear();
	m_usingItems.clear();
	if ( m_config.slots == 0 )
	{
		return;
	}
	// (Every driver: the players, and the bodies mods drive, whose items are theirs to use too.)
	for ( const DriverRef& driver : m_drivers )
	{
		const uint32_t netId = driver.netId;
		flecs::entity e = FindEntity( netId );
		const PlayerInput& in = driver.input;
		const Character& c = e.get<Character>();
		const bool had = e.has<Slots>();
		Slots slots = had ? e.get<Slots>() : Slots{ m_config.slots, kNoSlot, in.intentSeq, 0 };
		if ( slots.resync != 0 )
		{
			// Just taken over: what the input says was meant for the body the slot drove before.
			slots.resync = 0;
			slots.seq = in.intentSeq;
			slots.prevKeys = in.slotKeys;
			slots.prevActions = in.actions;
		}
		const bool act = had && c.dead == 0;
		// Using an item is recorded where every simulation can see it (the player's own at once): the
		// kind's event, at the player, with the item.
		auto used = [&]( uint32_t item ) {
			const ItemShape rule = ItemShapeOf( FindEntity( item ).get<HeldItem>().kind );
			if ( rule.usedEvent != 0xFFFF )
			{
				ModEventRecord record;
				record.type = rule.usedEvent;
				record.netIdA = netId;
				record.netIdB = item;
				record.tick = m_globals.tick;
				record.point = e.get<Transform>().position;
				RecordModEvent( record );
			}
		};
		// What it uses this tick (the item in the hand by the use button, one by its kind's own key),
		// as its slots are before this tick's keys and intents: recorded, and for conditions what it
		// is using ("using.<kind>").
		if ( act )
		{
			ItemUses( netId, in, c.prevButtons, slots.prevActions, m_useScratch );
			for ( const ItemUse& use : m_useScratch )
			{
				if ( use.inUse )
				{
					m_usingScratch.push_back( { netId, FindEntity( use.item ).get<HeldItem>().kind } );
					m_usingItems.push_back( use.item );
				}
				if ( use.used )
				{
					used( use.item );
				}
			}
		}
		slots.prevActions = in.actions;
		// A slot's key selects it, whatever it holds (again: empty hands); unless what it holds is
		// used by it (ItemShape::slotKeyUses, above): then nothing changes hands.
		auto slotKey = [&]( uint8_t index ) {
			const uint32_t item = index == kNoSlot ? 0 : SlotItemOf( netId, index );
			if ( item != 0 && ItemShapeOf( FindEntity( item ).get<HeldItem>().kind ).slotKeyUses != 0 )
			{
				return;
			}
			slots.selected = slots.selected == index ? kNoSlot : index;
		};
		const uint16_t keys = in.slotKeys;
		const uint16_t pressedKeys = uint16_t( keys & ~slots.prevKeys );
		slots.prevKeys = keys;
		for ( uint8_t index = 0; act && index < slots.count && index < 16; ++index )
		{
			if ( pressedKeys & uint16_t( 1u << index ) )
			{
				slotKey( index );
			}
		}
		// The container open to it closes when it is gone, or the player is too far from it, or dead.
		if ( slots.open != 0 )
		{
			flecs::entity box = FindEntity( slots.open );
			bool keep = box.is_valid() && box.has<Container>() && box.has<Transform>() && c.dead == 0 &&
						b3Distance( box.get<Transform>().position, e.get<Transform>().position ) <= kContainerKeep;
			slots.open = keep ? slots.open : 0;
		}
		if ( had && in.intentSeq == slots.seq )
		{
			e.set<Slots>( slots );
			SettleSlots( netId, slots.selected );
			continue;
		}
		// Once per intent: an input repeated while a packet is late, or guessed for someone else,
		// has the same count and does nothing again.
		slots.seq = in.intentSeq;
		// A slot an intent names: one of the player's, or (kContainerSlotBit) one of the open
		// container's. Its holder and its place there; holder 0: no such slot.
		struct SlotRef
		{
			uint32_t holder = 0;
			uint8_t index = 0;
		};
		auto address = [&]( uint8_t at ) {
			SlotRef ref;
			if ( at != kNoSlot && ( at & kContainerSlotBit ) != 0 )
			{
				flecs::entity box = slots.open != 0 ? FindEntity( slots.open ) : flecs::entity();
				uint8_t index = uint8_t( at & ~kContainerSlotBit );
				if ( box.is_valid() && box.has<Container>() && index < box.get<Container>().slots )
				{
					ref = { slots.open, index };
				}
			}
			else if ( at < slots.count )
			{
				ref = { netId, at };
			}
			return ref;
		};
		// An item put in a slot: the player's (its hands are settled below), or a container's, out of
		// sight there.
		auto place = [&]( flecs::entity item, SlotRef to ) {
			HeldItem h = item.get<HeldItem>();
			h.holder = to.holder;
			h.slot = to.index;
			if ( to.holder != netId )
			{
				h.stowed = 1;
				h.socket = kNoSocket;
			}
			item.set<HeldItem>( h );
		};
		const SlotRef refA = address( in.intentA );
		const SlotRef refB = address( in.intentB );
		const bool two = refA.holder != 0 && refB.holder != 0 && ( refA.holder != refB.holder || refA.index != refB.index );
		if ( act && in.intent == uint8_t( SlotIntent::Select ) && ( in.intentA < slots.count || in.intentA == kNoSlot ) )
		{
			// The same as its key (a screen's click, a bot), or no slot at all: empty hands.
			slotKey( in.intentA );
		}
		else if ( act && in.intent == uint8_t( SlotIntent::Move ) && two )
		{
			flecs::entity a = FindEntity( SlotItemOf( refA.holder, refA.index ) );
			flecs::entity b = FindEntity( SlotItemOf( refB.holder, refB.index ) );
			if ( a.is_valid() && b.is_valid() && MergeStacks( a, b, StackOf( a.get<HeldItem>() ) ) )
			{
				// Onto a stack of its kind: as many as fit (the rest stays where it was).
			}
			else
			{
				if ( a.is_valid() )
				{
					place( a, refB );
				}
				if ( b.is_valid() )
				{
					place( b, refA );
				}
			}
		}
		else if ( act && in.intent == uint8_t( SlotIntent::Split ) && two )
		{
			flecs::entity a = FindEntity( SlotItemOf( refA.holder, refA.index ) );
			flecs::entity b = FindEntity( SlotItemOf( refB.holder, refB.index ) );
			const HeldItem* from = a.is_valid() ? a.try_get<HeldItem>() : nullptr;
			uint32_t half = from != nullptr ? ( StackOf( *from ) + 1 ) / 2 : 0;
			if ( from != nullptr && b.is_valid() )
			{
				MergeStacks( a, b, half ); // (nothing when b is another kind, or full)
			}
			else if ( from != nullptr && StackOf( *from ) > 1 )
			{
				// Into the empty slot: a stack of its own, with the stack's fields.
				HeldItem rest = *from;
				rest.count = uint8_t( StackOf( *from ) - half );
				a.set<HeldItem>( rest );
				flecs::entity part = CreateEntity();
				HeldItem split = *from;
				split.count = uint8_t( half );
				part.set<HeldItem>( split );
				StartItemFields( part, split.kind );
				if ( HasBoard( a ) )
				{
					SetBoard( part, GetBoard( a ) );
				}
				part.set<Transform>( e.get<Transform>() );
				place( part, refB );
			}
			else if ( from != nullptr )
			{
				// One alone: it moves.
				place( a, refB );
			}
		}
		else if ( act && in.intent == uint8_t( SlotIntent::Drop ) )
		{
			SlotRef which = in.intentA == kNoSlot ? address( slots.selected ) : refA;
			if ( flecs::entity item = FindEntity( SlotItemOf( which.holder, which.index ) ); which.holder != 0 && item.is_valid() )
			{
				ThrowOut( item, e.get<Transform>(), in.cameraYaw );
			}
		}
		else if ( act && in.intent == uint8_t( SlotIntent::Open ) )
		{
			// What the player looks at, within reach: a container is open to it.
			float yaw = detmath::YawToRadians( in.cameraYaw );
			b3CosSin pitch = detmath::CosSin( float( in.cameraPitch ) * ( detmath::kTwoPi / 65536.0f ) );
			b3Vec3 forward = detmath::YawForward( yaw );
			b3Vec3 look = { forward.x * pitch.cosine, pitch.sine, forward.z * pitch.cosine };
			b3Vec3 eye = ViewPivot( e, e.get<Transform>().position );
			RayHit hit;
			flecs::entity box;
			if ( CastRay( eye, b3MulSV( kContainerReach, look ), netId, hit ) )
			{
				box = FindEntity( hit.netId );
			}
			slots.open = box.is_valid() && box.has<Container>() ? hit.netId : 0;
		}
		else if ( in.intent == uint8_t( SlotIntent::Close ) )
		{
			slots.open = 0;
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
	if ( slot >= kMaxPlayers )
	{
		return layout.spawnCenter; // (a body no player drives: an NPC)
	}
	float col = float( slot % 8 );
	float row = float( slot / 8 );
	return { layout.spawnCenter.x - 5.25f + 1.5f * col, layout.spawnCenter.y, layout.spawnCenter.z + 1.5f * row };
}

flecs::entity Simulation::CreatePlayer( PlayerSlot slot )
{
	flecs::entity e = CreateEntity();
	uint32_t netId = e.get<NetId>().value;

	Transform t{ SpawnPoint( slot ), detmath::YawRotation( 0.0f ) };
	Shape shape{ ShapeKind::Capsule, {}, { Body( 0 ).radius, Body( 0 ).halfHeight, 0.0f } };

	b3BodyDef bodyDef = b3DefaultBodyDef();
	bodyDef.type = b3_kinematicBody;
	bodyDef.position = t.position;
	bodyDef.rotation = t.rotation;
	// A character's body never sleeps (M246): asleep, Box3D takes it out of its awake bodies, and
	// that reorders the physics bodies around it (whose contacts are then made in another order),
	// so whether a player stands still would reach the physics world.
	bodyDef.enableSleep = false;
	b3BodyId body = b3CreateBody( m_physicsWorld, &bodyDef );
	b3ShapeId shapeId = CreateShape( body, shape, CatPlayer );

	Character c;
	c.slot = slot;

	e.set<Transform>( t );
	e.set<Velocity>( {} );
	e.set<Shape>( shape );
	e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
	e.set<Character>( c );
	SetAnimState( e, AnimState{} );

	m_globals.playerNetIds[slot] = netId;
	return e;
}

b3BodyId Simulation::CreateIdlePhysics( const Shape& shape, const Transform& t, b3Vec3 velocity, float mass, b3ShapeId& shapeId )
{
	b3BodyDef def = b3DefaultBodyDef();
	def.type = b3_dynamicBody;
	def.position = t.position;
	def.rotation = t.rotation;
	def.linearVelocity = velocity;
	// Upright, facing where it faced: it slides and falls, it does not tip over.
	def.motionLocks = { false, false, false, true, true, true };
	b3BodyId body = b3CreateBody( m_physicsWorld, &def );
	// Its capsule reaches down to its feet: driven, the mover holds the capsule above them on a ray,
	// so standing on its own it would sink by the difference. The body's origin stays where a driven
	// one's is (the capsule's centre), so it is drawn the same way.
	const float r = shape.halfExtents.x, h = shape.halfExtents.y;
	const float feet = mover::FeetBelowCenter( r, h );
	b3Capsule capsule = { { 0.0f, std::min( -feet + r, h ), 0.0f }, { 0.0f, h, 0.0f }, r };
	// It weighs what it did when it was driven (its mass, a movement parameter).
	const float length = capsule.center2.y - capsule.center1.y;
	const float volume = detmath::kPi * r * r * ( length + ( 4.0f / 3.0f ) * r );
	ShapeMaterial material;
	material.density = volume > 0.0f ? mass / volume : material.density;
	b3ShapeDef shapeDef = MakeShapeDef( CatProp, material );
	shapeId = b3CreateCapsuleShape( body, &shapeDef, &capsule );
	return body;
}

flecs::entity Simulation::CreateIdleBody( uint16_t character, b3Vec3 feet, float yaw, b3Vec3 velocity )
{
	character = character < m_characters.size() ? character : uint16_t( 0 );
	const CharacterBody& kind = Body( character );
	flecs::entity e = CreateEntity();
	Shape shape{ ShapeKind::Capsule, {}, { kind.radius, kind.halfHeight, 0.0f } };
	Transform t{ b3Add( feet, b3Vec3{ 0.0f, mover::FeetBelowCenter( kind.radius, kind.halfHeight ), 0.0f } ), detmath::YawRotation( yaw ) };
	MoveParams move = m_config.move;
	for ( const auto& [param, value] : kind.move )
	{
		if ( param < kMoveParams && character != 0 ) // (the server's own are in the config already, as MoveOf)
		{
			move.values[param] = value;
		}
	}
	b3ShapeId shapeId;
	b3BodyId body = CreateIdlePhysics( shape, t, velocity, move[MoveParam::Mass], shapeId );
	e.set<Transform>( t );
	e.set<Velocity>( { velocity, { 0.0f, 0.0f, 0.0f } } );
	e.set<Shape>( shape );
	e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
	e.set<IdleBody>( { character, 0 } );
	return e;
}

void Simulation::MakeIdle( flecs::entity e )
{
	if ( e.is_valid() == false || e.has<Character>() == false )
	{
		return;
	}
	if ( e.has<DrivenProp>() )
	{
		ReleaseProp( e ); // a prop again
		return;
	}
	const uint32_t netId = e.get<NetId>().value;
	// What it held is put away, not dropped: still its own.
	if ( e.has<Slots>() )
	{
		Slots slots = e.get<Slots>();
		slots.selected = kNoSlot;
		e.set<Slots>( slots );
		SettleSlots( netId, kNoSlot );
	}
	else
	{
		for ( const EntityRef& r : m_entities )
		{
			flecs::entity item( m_world, r.entity );
			if ( const HeldItem* held = item.try_get<HeldItem>(); held != nullptr && held->holder == netId && held->stowed == 0 )
			{
				HeldItem put = *held;
				put.stowed = 1;
				put.socket = kNoSocket;
				item.set<HeldItem>( put );
			}
		}
	}
	const Character c = e.get<Character>();
	const uint16_t character = CharacterOf( e );
	const float mass = MoveOf( e )[MoveParam::Mass];
	Transform t = e.get<Transform>();
	t.rotation = detmath::YawRotation( c.facingYaw );
	const Shape shape = e.get<Shape>();

	b3DestroyBody( BodyOf( e.get<PhysicsBody>() ) );
	b3ShapeId shapeId;
	b3BodyId body = CreateIdlePhysics( shape, t, c.velocity, mass, shapeId );
	e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
	e.set<Transform>( t );
	e.set<Velocity>( { c.velocity, { 0.0f, 0.0f, 0.0f } } );
	// The controller goes: its state, its animation, whatever its motions had going.
	e.remove<Character>();
	e.remove<CharacterKind>();
	e.remove<MotionHold>();
	e.remove<MoveEffects>();
	e.remove<MoveValues>();
	ecs_remove_id( m_world.c_ptr(), e.id(), m_animId );
	if ( m_motionId != 0 )
	{
		ecs_remove_id( m_world.c_ptr(), e.id(), m_motionId );
	}
	e.set<IdleBody>( { character, 0 } );
}

void Simulation::MakeDriven( flecs::entity e, PlayerSlot slot )
{
	const uint16_t character = e.get<IdleBody>().character;
	const Transform at = e.get<Transform>();
	const b3Vec3 velocity = e.has<Velocity>() ? e.get<Velocity>().linear : b3Vec3{ 0.0f, 0.0f, 0.0f };
	// Its yaw, from its upright rotation (a turn about +Y).
	const float yaw = detmath::WrapAngle( 2.0f * detmath::Atan2( at.rotation.v.y, at.rotation.s ) );
	Transform t{ at.position, detmath::YawRotation( yaw ) };
	const Shape shape = e.get<Shape>();

	b3DestroyBody( BodyOf( e.get<PhysicsBody>() ) );
	b3BodyDef def = b3DefaultBodyDef();
	def.type = b3_kinematicBody;
	def.position = t.position;
	def.rotation = t.rotation;
	def.enableSleep = false; // (a character's body never sleeps: CreatePlayer)
	b3BodyId body = b3CreateBody( m_physicsWorld, &def );
	b3ShapeId shapeId = CreateShape( body, shape, CatPlayer );

	Character c;
	c.slot = slot;
	c.facingYaw = yaw;
	c.velocity = velocity;
	e.remove<IdleBody>();
	e.set<Transform>( t );
	e.set<Velocity>( {} );
	e.set<PhysicsBody>( MakePhysicsBody( body, shapeId ) );
	e.set<Character>( c );
	if ( character != 0 )
	{
		e.set<CharacterKind>( { character, 0 } );
	}
	SetAnimState( e, AnimState{} );
	if ( e.has<Slots>() )
	{
		Slots slots = e.get<Slots>();
		slots.resync = 1;
		e.set<Slots>( slots );
	}
}

void Simulation::Possess( PlayerSlot slot, flecs::entity body )
{
	if ( slot >= kMaxPlayers )
	{
		return;
	}
	uint32_t& driven = m_globals.playerNetIds[slot];
	if ( driven == 0 || Possessable( body ) == false )
	{
		return; // (a slot nobody is in, or a body that is not free to drive)
	}
	if ( flecs::entity old = FindEntity( driven ); old.is_valid() )
	{
		if ( old.get<Character>().dead != 0 && old.has<DrivenProp>() == false )
		{
			// Dead, it is its ragdoll or nothing: it goes, with what it carried.
			for ( uint32_t item : CarriedItemsOf( driven ) )
			{
				DestroyEntity( FindEntity( item ) );
			}
			DestroyEntity( old );
		}
		else
		{
			MakeIdle( old );
		}
	}
	if ( body.has<IdleBody>() )
	{
		MakeDriven( body, slot );
	}
	else
	{
		DriveProp( body, slot );
	}
	driven = body.get<NetId>().value;
}

bool Simulation::Possessable( flecs::entity e ) const
{
	if ( e.is_valid() == false || e.has<Character>() )
	{
		return false;
	}
	if ( e.has<IdleBody>() )
	{
		return true;
	}
	// A loose body: not an item (held or lying), a ragdoll, a container, a wall's piece standing, or
	// anything static.
	const PhysicsBody* pb = e.try_get<PhysicsBody>();
	if ( pb == nullptr || e.has<StaticGeometry>() || e.has<HeldItem>() || e.has<Ragdoll>() || e.has<RagdollBodies>() || e.has<Container>() ||
		 e.has<WallPiece>() )
	{
		return false;
	}
	return b3Body_GetType( BodyOf( *pb ) ) == b3_dynamicBody;
}

b3Vec3 Simulation::ViewPivot( flecs::entity e, b3Vec3 position ) const
{
	const DrivenProp* prop = e.is_valid() ? e.try_get<DrivenProp>() : nullptr;
	return { position.x, position.y + kViewPivotHeight - ( prop != nullptr ? prop->drop : 0.0f ), position.z };
}

bool Simulation::SeenInFirstPerson( flecs::entity e ) const
{
	return e.has<DrivenProp>() == false && Body( CharacterOf( e ) ).firstPerson;
}

void Simulation::DriveProp( flecs::entity e, PlayerSlot slot )
{
	b3BodyId body = BodyOf( e.get<PhysicsBody>() );
	const b3Vec3 origin = b3Body_GetPosition( body );
	const b3Vec3 velocity = b3Body_GetLinearVelocity( body );
	// Upright, keeping its heading.
	const b3Vec3 forward = b3RotateVector( b3Body_GetRotation( body ), b3Vec3{ 0.0f, 0.0f, 1.0f } );
	const float yaw = forward.x * forward.x + forward.z * forward.z > 1e-6f ? detmath::Atan2( forward.x, forward.z ) : 0.0f;
	// Its bounds in its own frame: at the identity rotation, from its origin.
	b3Body_SetTransform( body, origin, b3Quat_identity );
	const b3AABB box = b3Body_ComputeAABB( body );
	const b3Quat upright = detmath::YawRotation( yaw );
	DrivenProp prop;
	prop.low[0] = box.lowerBound.x - origin.x;
	prop.low[1] = box.lowerBound.y - origin.y;
	prop.low[2] = box.lowerBound.z - origin.z;
	prop.high[0] = box.upperBound.x - origin.x;
	prop.high[1] = box.upperBound.y - origin.y;
	prop.high[2] = box.upperBound.z - origin.z;
	// The walking capsule, fitted: its feet at the prop's bottom (the mover holds the capsule three
	// radii above them), its top at the prop's top, no wider than the prop.
	const float height = std::max( prop.high[1] - prop.low[1], 0.05f );
	const float across = 0.5f * std::max( prop.high[0] - prop.low[0], prop.high[2] - prop.low[2] );
	prop.radius = std::clamp( std::min( across, ( height + 0.02f ) / 5.0f ), 0.05f, 0.6f );
	prop.halfHeight = std::max( 0.0f, ( height + 0.02f - 4.0f * prop.radius ) / 2.0f );
	const float feet = mover::FeetBelowCenter( prop.radius, prop.halfHeight );
	const b3Vec3 centre = { origin.x, origin.y + prop.low[1] + feet, origin.z };
	prop.drop = centre.y - origin.y;

	b3Body_SetTransform( body, origin, upright );
	b3Body_SetType( body, b3_kinematicBody );
	b3Body_EnableSleep( body, false ); // (a character's body never sleeps: CreatePlayer)
	b3Body_SetLinearVelocity( body, { 0.0f, 0.0f, 0.0f } );
	b3Body_SetAngularVelocity( body, { 0.0f, 0.0f, 0.0f } );
	b3ShapeId shapes[64];
	const int count = b3Body_GetShapes( body, shapes, 64 );
	for ( int i = 0; i < count; ++i )
	{
		b3Filter filter = b3Shape_GetFilter( shapes[i] );
		prop.category = i == 0 ? uint32_t( filter.categoryBits ) : prop.category;
		filter.categoryBits = CatDriven;
		filter.maskBits = CollisionMask( CatDriven );
		b3Shape_SetFilter( shapes[i], filter, true );
	}

	Character c;
	c.slot = slot;
	c.facingYaw = yaw;
	c.velocity = velocity;
	e.set<Transform>( { centre, upright } );
	e.set<Velocity>( {} );
	e.set<Character>( c );
	e.set<DrivenProp>( prop );
	SetAnimState( e, AnimState{} );
	if ( const Prop* p = e.try_get<Prop>(); p != nullptr && p->despawnTick != 0 )
	{
		Prop kept = *p;
		kept.despawnTick = 0; // (a player's body does not run out)
		e.set<Prop>( kept );
	}
}

void Simulation::ReleaseProp( flecs::entity e )
{
	const DrivenProp prop = e.get<DrivenProp>();
	const Character c = e.get<Character>();
	const Transform t = e.get<Transform>();
	b3BodyId body = BodyOf( e.get<PhysicsBody>() );
	const b3Vec3 origin = { t.position.x, t.position.y - prop.drop, t.position.z };
	const b3Quat upright = detmath::YawRotation( c.facingYaw );
	b3Body_SetTransform( body, origin, upright );
	b3Body_SetType( body, b3_dynamicBody );
	b3Body_EnableSleep( body, true );
	b3Body_Enable( body ); // (its driver may have died in it: a dead body is out of the world)
	b3ShapeId shapes[64];
	const int count = b3Body_GetShapes( body, shapes, 64 );
	for ( int i = 0; i < count; ++i )
	{
		b3Filter filter = b3Shape_GetFilter( shapes[i] );
		filter.categoryBits = prop.category != 0 ? prop.category : uint64_t( CatProp );
		filter.maskBits = CollisionMask( filter.categoryBits );
		b3Shape_SetFilter( shapes[i], filter, true );
	}
	b3Body_SetLinearVelocity( body, c.velocity );
	b3Body_SetAwake( body, true );
	e.set<Transform>( { origin, upright } );
	e.set<Velocity>( { c.velocity, { 0.0f, 0.0f, 0.0f } } );
	e.remove<Character>();
	e.remove<DrivenProp>();
	e.remove<MotionHold>();
	e.remove<MoveEffects>();
	e.remove<MoveValues>();
	ecs_remove_id( m_world.c_ptr(), e.id(), m_animId );
	if ( m_motionId != 0 )
	{
		ecs_remove_id( m_world.c_ptr(), e.id(), m_motionId );
	}
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
	const AnimState old = GetAnimState( e );
	AnimState anim;
	anim.aiming = old.aiming;
	// The stances stay (the mod still has the weapon out).
	anim.stances = old.stances;
	SetAnimState( e, anim );
	e.set<Transform>( t );
	e.set<Velocity>( {} );
	if ( const MotionHold* hold = e.try_get<MotionHold>(); hold != nullptr && hold->on != 0 )
	{
		MotionHold off = *hold;
		off.on = 0;
		e.set<MotionHold>( off );
	}

	b3BodyId body = BodyOf( e.get<PhysicsBody>() );
	const float drop = e.has<DrivenProp>() ? e.get<DrivenProp>().drop : 0.0f;
	b3Body_SetTransform( body, b3Vec3{ t.position.x, t.position.y - drop, t.position.z }, t.rotation );
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
		else if ( ev.type == PlayerEventType::Leave && netId != 0 && FindEntity( netId ).has<DrivenProp>() )
		{
			ReleaseProp( FindEntity( netId ) ); // (a map's crate stays the map's)
			netId = 0;
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
	// Everyone who moves this tick, and uses what it holds: the players by slot, then the bodies
	// mods drive (NPCs) by NetId.
	m_drivers.clear();
	for ( int slot = 0; slot < kMaxPlayers; ++slot )
	{
		// (A stand-in is the server's to move: the state model.)
		if ( const uint32_t netId = m_globals.playerNetIds[slot]; netId != 0 && ( m_followsServer == false || IsStandIn( netId ) == false ) )
		{
			m_drivers.push_back( { netId, frame.inputs[slot], slot } );
		}
	}
	for ( const EntityRef& r : m_entities )
	{
		// (NetIds in order: the list is sorted.)
		if ( const ModDriven* npc = flecs::entity( m_world, r.entity ).try_get<ModDriven>(); npc != nullptr && StandsIn( flecs::entity( m_world, r.entity ) ) == false )
		{
			m_drivers.push_back( { r.netId, npc->input, -1 } );
		}
	}
	// What the players ask of their slots, first: everything below asks what is in the hand.
	{
		auto start = std::chrono::steady_clock::now();
		StepSlots( frame );
		m_profile.slotsNs += uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now() - start ).count() );
	}
	// What everyone holds, for state machines that ask ("attack and melee.bat").
	m_heldScratch.clear();
	m_carriedScratch.clear();
	if ( AnyGraph() || m_motions )
	{
		for ( const EntityRef& r : m_entities )
		{
			flecs::entity e( m_world, r.entity );
			if ( const HeldItem* item = e.try_get<HeldItem>(); item != nullptr && item->holder != 0 )
			{
				m_carriedScratch.push_back( { item->holder, item->kind } );
				if ( item->stowed == 0 )
				{
					m_heldScratch.push_back( { item->holder, item->kind } );
				}
			}
		}
	}
	// What a player holds, carries and uses, for what reads its items (motions, its state machine).
	struct Kinds
	{
		uint16_t held[16];
		uint32_t heldCount = 0;
		uint16_t carried[32];
		uint32_t carriedCount = 0;
		uint16_t usingKinds[16];
		uint32_t usingCount = 0;
	};
	auto kindsOf = [this]( uint32_t netId, Kinds& k ) {
		for ( const auto& [holder, kind] : m_heldScratch )
		{
			if ( holder == netId && k.heldCount < 16 )
			{
				k.held[k.heldCount++] = kind;
			}
		}
		for ( const auto& [holder, kind] : m_carriedScratch )
		{
			if ( holder == netId && k.carriedCount < 32 )
			{
				k.carried[k.carriedCount++] = kind;
			}
		}
		for ( const auto& [holder, kind] : m_usingScratch )
		{
			if ( holder == netId && k.usingCount < 16 )
			{
				k.usingKinds[k.usingCount++] = kind;
			}
		}
	};

	// Two passes, so nothing depends on which slot a player has. First every player's motions, each
	// on the world as the tick found it (no one has moved yet), with what they do to other players
	// kept and applied once all have run, after the targets' own; then every player moves. Within a
	// pass, slot order: deterministic, and independent of entity creation history.
	std::vector<uint8_t> moving( m_drivers.size(), 0 );
	m_turnCharacters.resize( m_drivers.size() );
	m_playerPushes.clear();
	m_deferPlayerPushes = true;
	for ( size_t d = 0; d < m_drivers.size(); ++d )
	{
		const int slot = m_drivers[d].slot;
		const uint32_t netId = m_drivers[d].netId;
		flecs::entity e = FindEntity( netId );
		const PlayerInput& in = m_drivers[d].input;
		Character c = e.get<Character>();
		if ( c.dead )
		{
			// Held buttons are still tracked, so a jump held through the respawn is not a press.
			c.prevButtons = in.buttons;
			e.set<Character>( c );
			if ( m_motions )
			{
				MotionState motion = GetMotionState( e );
				motion.prevActions = in.actions;
				SetMotionState( e, motion );
				if ( const MotionHold* hold = e.try_get<MotionHold>(); hold != nullptr && hold->on != 0 )
				{
					MotionHold off = *hold;
					off.on = 0;
					e.set<MotionHold>( off );
				}
			}
			continue;
		}
		moving[d] = 1;
		const Transform t = e.get<Transform>();
		const uint8_t pressed = uint8_t( in.buttons & ~c.prevButtons );
		Kinds kinds;
		kindsOf( netId, kinds );
		const uint16_t* held = kinds.held;
		const uint32_t heldCount = kinds.heldCount;
		const uint16_t* carried = kinds.carried;
		const uint32_t carriedCount = kinds.carriedCount;
		const uint16_t* usingKinds = kinds.usingKinds;
		const uint32_t usingCount = kinds.usingCount;

		// The mods' motions (a dash, a double jump), before the mover: on the world as the last tick
		// and this tick's commands left it, by this tick's input. A frozen player does none.
		if ( m_motions )
		{
			MotionState motion = GetMotionState( e );
			FlyHold( e, motion );
			{
				const AnimState before = GetAnimState( e );
				Blackboard board = GetBoard( e );
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
				values.board = &boardBefore.values;
				values.input = &in;
				// A frozen player presses nothing.
				values.pressedActions = c.frozen ? ActionBits( 0 ) : ActionBits( in.actions & ~motion.prevActions );
				values.pressedButtons = c.frozen ? uint8_t( 0 ) : pressed;
				// What a probe of its holds on to, if one is out, and whether that is still there.
				int heldMotion = -1;
				bool holdAlive = false;
				b3Vec3 where = {};
				bool missed = false;
				if ( const MotionHold* out = e.try_get<MotionHold>(); out != nullptr && out->on != 0 )
				{
					heldMotion = out->motion;
					// A line that found nothing is gone when it has flown its range.
					missed = out->missed != 0;
					holdAlive = HoldPoint( *out, where ) && ( missed == false || m_globals.tick < out->holdTick );
				}
				// ("linked": it holds on to something, or flies to: not a line that found nothing.)
				values.builtins[AnimExpr::Linked] = heldMotion >= 0 && missed == false ? 1.0f : 0.0f;
				// (From where the line leaves the player: the point its camera orbits.)
				values.builtins[AnimExpr::LinkDistance] =
					holdAlive && missed == false ? b3Distance( where, ViewPivot( e, t.position ) ) : 0.0f;
				values.globalBoard = &m_globalBoard;
				values.events = m_globals.modEvents;
				values.eventCount = m_globals.modEventCount;
				values.tick = m_globals.tick;
				values.hearLate = true; // (a motion's conditions: an animation's marker of last tick)
				values.netId = netId;
				values.heldKinds = held;
				values.heldCount = heldCount;
				values.carriedKinds = carried;
				values.carriedCount = carriedCount;
				values.usingKinds = usingKinds;
				values.usingCount = usingCount;
				// (As the tick found them: what was on when the last tick ended.)
				values.motionsOn = m_globals.tick > 0 ? MotionsOnAt( motion, m_globals.tick - 1 ) : 0;

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
				m_refusalScratch.clear();
				motionIn.refusals = &m_refusalScratch;
				RunMotions( *m_motions, motionIn, motion, c, board, boardChanged, m_motionEvents, m_motionActive );
				if ( m_refusalScratch.empty() == false && slot >= 0 )
				{
					m_motionNotes[size_t( slot )] = { m_refusalScratch.back().motion, m_refusalScratch.back().why, m_globals.tick };
				}
				if ( boardChanged )
				{
					SetBoard( e, board );
				}
				// A hold whose motion ended (its condition, a new throw) is let go.
				if ( const MotionHold* out = e.try_get<MotionHold>(); out != nullptr && out->on != 0 )
				{
					const MotionSlot slot = motion.slots[out->motion];
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
					record.late = 1; // (the motions have run: they hear it next tick)
					RecordModEvent( record );
				}
				// The effects read the state the motions just decided (the parameters that hold).
				SetMotionState( e, motion );
				ApplyMotionEffects( e, c, t, in, motion, boardBefore, m_motionActive, values );
			}
		}
		// (Stored once every player's motions have run: until then the others read it as it was.)
		m_turnCharacters[d] = c;
	}
	m_deferPlayerPushes = false;
	for ( size_t d = 0; d < m_drivers.size(); ++d )
	{
		if ( moving[d] )
		{
			FindEntity( m_drivers[d].netId ).set<Character>( m_turnCharacters[d] );
		}
	}
	// What their motions did to others: after the targets' own, in the order they were done.
	ApplyPlayerPushes();

	for ( size_t d = 0; d < m_drivers.size(); ++d )
	{
		if ( moving[d] == 0 )
		{
			continue;
		}
		const uint32_t netId = m_drivers[d].netId;
		flecs::entity e = FindEntity( netId );
		const PlayerInput& in = m_drivers[d].input;
		Character c = e.get<Character>();
		Transform t = e.get<Transform>();
		PhysicsBody pb = e.get<PhysicsBody>();
		const uint8_t pressed = uint8_t( in.buttons & ~c.prevButtons );
		Kinds kinds;
		kindsOf( netId, kinds );
		const uint16_t* held = kinds.held;
		const uint32_t heldCount = kinds.heldCount;
		const uint16_t* carried = kinds.carried;
		const uint32_t carriedCount = kinds.carriedCount;
		const uint16_t* usingKinds = kinds.usingKinds;
		const uint32_t usingCount = kinds.usingCount;

		// Frozen: the mover still runs (gravity, the ground, being pushed), with no intent.
		PlayerInput still;
		still.cameraYaw = in.cameraYaw;
		const CharacterBody& bodyKind = Body( CharacterOf( e ) );
		const DrivenProp* driven = e.try_get<DrivenProp>();
		mover::Body moving{ m_physicsWorld, BodyOf( pb ), ShapeOf( pb ), bodyKind.radius, bodyKind.halfHeight };
		if ( driven != nullptr )
		{
			moving.radius = driven->radius;
			moving.halfHeight = driven->halfHeight;
			moving.offset = { 0.0f, -driven->drop, 0.0f };
		}
		const b3Vec3 moveStart = t.position;
		const float yawStart = c.facingYaw;
		mover::Move( moving, MoveOf( e ), m_config.TimeStep(), m_globals.tick, c.frozen ? still : in, c.frozen ? uint8_t( 0 ) : pressed, c, t );
		if ( driven != nullptr )
		{
			HoldBackProp( e, *driven, moveStart, yawStart, t, c );
		}
		c.prevButtons = in.buttons;

		AnimState anim = GetAnimState( e );
		UpdateAnimState( anim, c, in, m_globals.tick, m_config.TimeStep() );
		if ( const AnimGraph* graph = Body( CharacterOf( e ) ).graph.get() )
		{
			// The character's own state machine, on what this tick's movement and commands left.
			Blackboard board = GetBoard( e );
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
			graphIn.board = &board.values;
			graphIn.globalBoard = &m_globalBoard;
			graphIn.events = m_globals.modEvents;
			graphIn.eventCount = m_globals.modEventCount;
			graphIn.tick = m_globals.tick;
			graphIn.netId = netId;
			graphIn.heldKinds = held;
			graphIn.heldCount = heldCount;
			graphIn.carriedKinds = carried;
			graphIn.carriedCount = carriedCount;
			graphIn.usingKinds = usingKinds;
			graphIn.usingCount = usingCount;
			if ( m_motions && HasMotionState( e ) )
			{
				graphIn.motionsOn = MotionsOnAt( GetMotionState( e ), m_globals.tick );
			}
			m_markerScratch.clear();
			UpdateAnimGraph( anim, *graph, m_animPacks, graphIn, m_config.TimeStep(), m_markerScratch );
			for ( int event : m_markerScratch )
			{
				// A marker the playing clip crossed: the mod event of its name, from this player.
				// (After its motions: they hear it next tick, a throw released on its marker.)
				ModEventRecord record;
				record.type = uint16_t( event );
				record.netIdA = netId;
				record.tick = m_globals.tick;
				record.point = t.position;
				record.late = 1;
				RecordModEvent( record );
			}
		}
		SetAnimState( e, anim );

		e.set<Character>( c );
		e.set<Transform>( t );
		e.set<Velocity>( { c.velocity, { 0.0f, 0.0f, 0.0f } } );
	}
	CreateLaunched();
}

// What the motions threw this tick, in the order the players moved: an item in the world, like
// one dropped, that knows who threw it and when it goes (a Prop: the caps on props count it, and
// ExpireProps removes it). Every simulation makes the same ones from the same inputs, so a
// player's own throw is there on the tick of the press.
void Simulation::CreateLaunched()
{
	if ( m_followsPhysics )
	{
		m_launched.clear(); // (what is launched comes in the frame's physics events)
		return;
	}
	for ( const Launched& l : m_launched )
	{
		flecs::entity item;
		if ( l.item != 0 )
		{
			// The copy it carried leaves its slot, as it is (its fields too); from a stack of more
			// than one, one is split off it, with the stack's fields, and the rest stays.
			item = FindEntity( l.item );
			const HeldItem* held = item.is_valid() ? item.try_get<HeldItem>() : nullptr;
			if ( held == nullptr || held->holder != l.owner )
			{
				continue;
			}
			if ( StackOf( *held ) > 1 )
			{
				HeldItem rest = *held;
				rest.count = uint8_t( StackOf( *held ) - 1 );
				item.set<HeldItem>( rest );
				flecs::entity stack = item;
				item = CreateEntity();
				item.set<HeldItem>( { 0, l.kind, 0, 0 } );
				StartItemFields( item, l.kind );
				if ( HasBoard( stack ) )
				{
					SetBoard( item, GetBoard( stack ) );
				}
			}
		}
		else
		{
			item = CreateEntity();
			item.set<HeldItem>( { 0, l.kind, 0, 0 } );
			StartItemFields( item, l.kind );
		}
		QueueIntoWorld( item, l.position, l.rotation, l.velocity, l.owner, l.lifetimeTicks );
	}
	m_launched.clear();
}

uint32_t Simulation::CarriedCopyOf( uint32_t holder, uint16_t kind ) const
{
	uint32_t best = 0;
	int bestRank = 1 << 30;
	for ( const EntityRef& r : m_entities )
	{
		const HeldItem* item = flecs::entity( m_world, r.entity ).try_get<HeldItem>();
		if ( item == nullptr || item->holder != holder || item->kind != kind )
		{
			continue;
		}
		// (A stack may be thrown from as many times a tick as it has.)
		uint32_t taken = uint32_t( std::count_if( m_launched.begin(), m_launched.end(), [&]( const Launched& l ) { return l.item == r.netId; } ) );
		// In a hand first, then the lowest slot, then the oldest.
		int rank = ( item->stowed == 0 ? 0 : 1 << 20 ) + int( item->slot ) * 4096;
		if ( taken < StackOf( *item ) && rank < bestRank )
		{
			best = r.netId;
			bestRank = rank;
		}
	}
	return best;
}

uint32_t Simulation::LaunchedBy( uint32_t netId ) const
{
	flecs::entity e = FindEntity( netId );
	const Prop* prop = e.is_valid() && e.has<HeldItem>() ? e.try_get<Prop>() : nullptr;
	return prop != nullptr ? prop->owner : 0;
}

void Simulation::ExpireProps()
{
	uint32_t tick = m_globals.tick;
	m_scratch.clear();
	std::vector<std::pair<EntityRef, int>> goingOff;
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		const Prop* p = e.try_get<Prop>();
		if ( p != nullptr && p->despawnTick != 0 && tick >= p->despawnTick )
		{
			// An item's time is up: said, where it was, if its kind has something to say (a fuse).
			const HeldItem* item = e.try_get<HeldItem>();
			uint16_t said = item != nullptr ? ItemShapeOf( item->kind ).expiredEvent : uint16_t( 0xFFFF );
			if ( said != 0xFFFF )
			{
				ModEventRecord record;
				record.type = said;
				record.netIdA = r.netId;
				record.netIdB = p->owner;
				record.tick = tick;
				const Transform* at = e.try_get<Transform>();
				record.point = at != nullptr ? at->position : b3Vec3{};
				RecordModEvent( record );
				goingOff.push_back( { r, int( said ) } );
			}
			m_scratch.push_back( r );
			continue;
		}
		const Ragdoll* rd = e.try_get<Ragdoll>();
		if ( rd != nullptr && rd->despawnTick != 0 && tick >= rd->despawnTick )
		{
			m_scratch.push_back( r );
		}
	}
	// What their sheets say they do then (a grenade's blast), while they are still here: after the
	// walk over the entities, as it may break walls (new pieces) and move things.
	const std::vector<EntityRef> going = m_scratch;
	for ( const auto& [r, event] : goingOff )
	{
		RunItemMotions( flecs::entity( m_world, r.entity ), event, true );
	}
	for ( const EntityRef& r : going )
	{
		DestroyEntity( flecs::entity( m_world, r.entity ) );
	}
}

void Simulation::EnforcePropCaps()
{
	// Spawned props in NetId order == spawn order, so the front of each list is the oldest. Counted
	// by the owner's NetId: the body that spawned them, whoever drives it now (a body a player left,
	// one a mod drives, has no slot to count them by).
	std::vector<std::pair<uint32_t, uint32_t>> perOwner; // owner NetId, how many
	auto countOf = [&]( uint32_t owner ) -> uint32_t& {
		for ( auto& [id, count] : perOwner )
		{
			if ( id == owner )
			{
				return count;
			}
		}
		perOwner.push_back( { owner, 0 } );
		return perOwner.back().second;
	};
	uint32_t total = 0;
	m_scratch.clear();

	// First pass: count.
	for ( const EntityRef& r : m_entities )
	{
		const flecs::entity prop( m_world, r.entity );
		const Prop* p = prop.try_get<Prop>();
		if ( p == nullptr || p->owner == 0 || prop.has<Character>() )
		{
			continue; // (a prop someone drives is a body, not a prop to count)
		}
		total += 1;
		if ( FindEntity( p->owner ).is_valid() )
		{
			countOf( p->owner ) += 1;
		}
	}

	// Second pass: oldest first, drop while over a cap.
	for ( const EntityRef& r : m_entities )
	{
		const flecs::entity prop( m_world, r.entity );
		const Prop* p = prop.try_get<Prop>();
		if ( p == nullptr || p->owner == 0 || prop.has<Character>() )
		{
			continue; // (a prop someone drives is a body, not a prop to count)
		}

		uint32_t* ownerCount = FindEntity( p->owner ).is_valid() ? &countOf( p->owner ) : nullptr;
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
	m_impactScratch.clear(); // (what BreakByItems reads: this step's hits, none)
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

	// An item's own hits first, whatever else collides, and all of them (as many as the ring holds):
	// a mod decides what a thrown grenade's hit means (ctx.Hits), so none may be crowded out by
	// crates falling about. The rest, the hardest, as many as a tick keeps.
	auto isItem = [this]( uint32_t netId ) {
		flecs::entity e = netId != 0 ? FindEntity( netId ) : flecs::entity();
		return e.is_valid() && e.has<HeldItem>();
	};
	auto firstOther = std::stable_partition( m_impactScratch.begin(), m_impactScratch.end(), [&]( const ImpactRecord& r ) {
		return isItem( r.netIdA ) || isItem( r.netIdB );
	} );
	size_t items = size_t( firstOther - m_impactScratch.begin() );
	size_t count = std::min( m_impactScratch.size(), std::min( std::max( items, size_t( kImpactsPerTick ) ), size_t( kImpactHistory ) ) );
	for ( size_t i = 0; i < count; ++i )
	{
		m_globals.impacts[m_globals.impactCount % kImpactHistory] = m_impactScratch[i];
		m_globals.impactCount += 1;
	}
}

void Simulation::RememberBreakers()
{
	m_breakersBefore.clear();
	if ( m_anyBreaks == false )
	{
		return;
	}
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		const HeldItem* item = e.try_get<HeldItem>();
		const PhysicsBody* pb = item != nullptr && item->holder == 0 ? e.try_get<PhysicsBody>() : nullptr;
		if ( pb != nullptr && ItemShapeOf( item->kind ).breakSpeed > 0.0f )
		{
			m_breakersBefore.push_back( { r.netId, b3Body_GetLinearVelocity( BodyOf( *pb ) ) } );
		}
	}
}

void Simulation::BreakByItems()
{
	if ( m_breakersBefore.empty() || m_impactScratch.empty() )
	{
		return;
	}
	// What this step's hits mean, decided first (breaking changes what the later ones name): an item
	// that breaks walls, each once, against a wall that breaks. In the hits' order (the hardest
	// first, then by the entities): the same in every simulation.
	struct Smash
	{
		uint32_t item = 0;
		uint32_t wall = 0;
		b3Vec3 point = {};
		b3Vec3 before = {};
		float speed = 0.0f;
	};
	std::vector<Smash> smashes;
	auto breakable = [this]( flecs::entity e ) {
		if ( e.has<WallPiece>() )
		{
			return true;
		}
		const TemplateRef* ref = e.try_get<TemplateRef>();
		return ref != nullptr && ref->index < m_map.templates.size() && m_map.templates[ref->index].pieces.empty() == false;
	};
	for ( const ImpactRecord& hit : m_impactScratch )
	{
		for ( int side = 0; side < 2; ++side )
		{
			const uint32_t itemId = side == 0 ? hit.netIdA : hit.netIdB;
			const uint32_t wallId = side == 0 ? hit.netIdB : hit.netIdA;
			auto was = std::find_if( m_breakersBefore.begin(), m_breakersBefore.end(), [&]( const auto& b ) { return b.first == itemId; } );
			flecs::entity wall = wallId != 0 ? FindEntity( wallId ) : flecs::entity();
			if ( was == m_breakersBefore.end() || wall.is_valid() == false || breakable( wall ) == false ||
				 std::any_of( smashes.begin(), smashes.end(), [&]( const Smash& s ) { return s.item == itemId; } ) )
			{
				continue;
			}
			const ItemShape shape = ItemShapeOf( FindEntity( itemId ).get<HeldItem>().kind );
			const float speed = std::max( b3Length( was->second ), hit.speed );
			if ( speed >= shape.breakSpeed )
			{
				smashes.push_back( { itemId, wallId, hit.point, was->second, speed } );
			}
		}
	}
	for ( const Smash& smash : smashes )
	{
		flecs::entity item = FindEntity( smash.item );
		flecs::entity wall = FindEntity( smash.wall );
		if ( item.is_valid() == false || wall.is_valid() == false )
		{
			continue; // (another item's hole took it this step)
		}
		const ItemShape shape = ItemShapeOf( item.get<HeldItem>().kind );
		const float moving = b3Length( smash.before );
		const b3Vec3 way = moving > 0.01f ? b3MulSV( 1.0f / moving, smash.before ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
		// A hole as wide as it was fast; the pieces flung its way (from half a metre behind the hit).
		const float radius = std::clamp( shape.breakHole * smash.speed / 30.0f, 0.25f, 4.0f );
		BreakWall( wall, b3Sub( smash.point, b3MulSV( 0.5f, way ) ), radius, 0.5f * smash.speed, 0 );
		// It has bounced off in this step: on through the hole instead, with a share of what it had.
		if ( shape.breakThrough > 0.0f )
		{
			b3Body_SetLinearVelocity( BodyOf( item.get<PhysicsBody>() ), b3MulSV( shape.breakThrough, smash.before ) );
		}
		if ( shape.breakEvent != 0xFFFF )
		{
			ModEventRecord record;
			record.type = shape.breakEvent;
			record.netIdA = smash.item;
			record.netIdB = smash.wall;
			record.tick = m_globals.tick;
			record.value = int32_t( radius * 100.0f );
			record.point = smash.point;
			record.vector = way;
			RecordModEvent( record );
		}
	}
}

void Simulation::UpdateFootsteps()
{
	const float dt = m_config.TimeStep();
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		Character* c = e.try_get_mut<Character>();
		if ( c == nullptr || StandsIn( e ) )
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
		if ( t == nullptr || t->position.y >= killY || e.has<StaticGeometry>() || StandsIn( e ) )
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

Simulation::PhysicsCounts Simulation::CountPhysics() const
{
	PhysicsArena::Scope scope( *m_arena );
	b3Counters c = b3World_GetCounters( m_physicsWorld );
	PhysicsCounts out;
	out.bodies = c.bodyCount;
	out.awakeBodies = b3World_GetAwakeBodyCount( m_physicsWorld );
	out.shapes = c.shapeCount;
	out.contacts = c.contactCount;
	out.awakeContacts = c.awakeContactCount;
	out.joints = c.jointCount;
	out.islands = c.islandCount;
	return out;
}

void Simulation::Step( const InputFrame& given )
{
	if ( given.tick != m_globals.tick )
	{
		std::fprintf( stderr, "Simulation::Step: frame for tick %u, expected %u\n", given.tick, m_globals.tick );
		std::abort();
	}
	// A body whose character has no first-person camera is seen from behind only: a first-person
	// view in its input is the third-person one, in every simulation.
	const InputFrame* chosen = &given;
	InputFrame narrowed;
	if ( m_someSeenFromBehind )
	{
		for ( int slot = 0; slot < kMaxPlayers; ++slot )
		{
			const uint32_t netId = m_globals.playerNetIds[slot];
			if ( netId == 0 || ViewMode( given.inputs[slot].view ) != ViewMode::FirstPerson ||
				 SeenInFirstPerson( FindEntity( netId ) ) )
			{
				continue;
			}
			if ( chosen == &given )
			{
				narrowed = given;
				chosen = &narrowed;
			}
			narrowed.inputs[slot].view = uint8_t( ViewMode::ThirdPerson );
		}
	}
	const InputFrame& frame = *chosen;

	PhysicsArena::Scope scope( *m_arena );
	if ( m_tasks->threaded )
	{
		PhysicsThreads::Wake();
	}

	// (The profile: the time between two of these goes to the part named.)
	auto last = std::chrono::steady_clock::now();
	auto spent = [&last]( uint64_t& part ) {
		auto now = std::chrono::steady_clock::now();
		part += uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( now - last ).count() );
		last = now;
	};

	ApplyEvents( frame );
	ApplyCommands( frame );
	spent( m_profile.commandsNs );
	MoveCharacters( frame );
	m_physicsMade.physics.clear();
	ApplyPhysicsEvents( frame, 0 );
	// (Following a server, the world's rules are its own: simulation.h, ApplyState.)
	if ( m_followsServer == false )
	{
		ExpireProps();
		EnforcePropCaps();
	}
	spent( m_profile.charactersNs );

	if ( m_followsServer == false )
	{
		RememberBreakers();
	}
	PlacePushers( frame );
	b3World_Step( m_physicsWorld, m_config.TimeStep(), int( m_config.subSteps ) );
	spent( m_profile.physicsNs );
	{
		b3Profile parts = b3World_GetProfile( m_physicsWorld );
		m_profile.pairsMs += parts.pairs;
		m_profile.collideMs += parts.collide;
		m_profile.solveMs += parts.solve;
		m_profile.constraintsMs += parts.constraints;
		m_profile.transformsMs += parts.transforms;
		m_profile.refitMs += parts.refit;
		m_profile.sleepMs += parts.sleepIslands;
		m_profile.sensorsMs += parts.sensors;
	}

	CollectImpacts();
	if ( m_followsServer == false )
	{
		BreakGlue();
		BreakByItems();
	}
	RunItemEvents();
	ApplyPhysicsEvents( frame, 1 );
	SyncFromPhysics();
	FollowHolders();
	UpdateFootsteps();
	HandleOutOfBounds();
	spent( m_profile.afterNs );
	m_profile.steps += 1;

	m_globals.tick += 1;
}

void Simulation::SerializeEcs( std::vector<uint8_t>& out ) const
{
	out.clear();
	AppendValue( out, kSnapMagic );
	AppendValue( out, m_globals );
	AppendBytes( out, m_globalBoard.data(), m_globalBoard.size() * 4 );
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
	m_globalBoard.assign( reinterpret_cast<const int32_t*>( rd.Take( size_t( m_config.globalFields ) * 4 ) ), m_config.globalFields );
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
	IndexesStale();
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
	// A mesh's or height field's data the world will point at: kept by the simulation, 8-aligned.
	auto keep = []( void* context, size_t size ) -> void* {
		auto& kept = *static_cast<std::vector<std::unique_ptr<uint64_t[]>>*>( context );
		kept.push_back( std::make_unique<uint64_t[]>( ( size + 7 ) / 8 ) );
		return kept.back().get();
	};
	if ( cbx_DeserializeWorld( m_physicsWorld, in.data() + rd.cursor, in.size() - rd.cursor, keep, &m_keptGeometry ) == false )
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

std::vector<uint32_t> Simulation::CarriedItemsOf( uint32_t holder ) const
{
	std::vector<uint32_t> out;
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		if ( const HeldItem* item = e.try_get<HeldItem>(); item != nullptr && holder != 0 && item->holder == holder )
		{
			out.push_back( r.netId );
		}
	}
	return out;
}

void Simulation::StartItemFields( flecs::entity item, uint16_t kind )
{
	const ItemShape& shape = ItemShapeOf( kind );
	if ( shape.fieldDefaults.empty() || m_config.fields == 0 )
	{
		return;
	}
	Blackboard board = GetBoard( item );
	board.values.resize( m_config.fields );
	for ( const ItemShape::FieldDefault& field : shape.fieldDefaults )
	{
		if ( field.slot < m_config.fields )
		{
			board.values[field.slot] = field.raw;
		}
	}
	SetBoard( item, board );
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
			if ( command.target == 0 )
			{
				if ( command.index < m_config.globalFields )
				{
					m_globalBoard[command.index] = command.value;
				}
				return;
			}
			if ( command.index >= m_config.fields )
			{
				return;
			}
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() == false )
			{
				return;
			}
			Blackboard board = GetBoard( e );
			board.values[command.index] = command.value;
			SetBoard( e, board );
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

		case CommandType::SpawnBody:
		{
			const float yaw = detmath::YawToRadians( uint16_t( command.value & 0xFFFF ) );
			flecs::entity body = CreateIdleBody( command.index, ToVec( command.a ), yaw, { 0.0f, 0.0f, 0.0f } );
			if ( command.target & kSlotTargetBit )
			{
				const uint32_t slot = command.target & ~kSlotTargetBit;
				if ( slot < uint32_t( kMaxPlayers ) )
				{
					Possess( PlayerSlot( slot ), body );
				}
			}
			return;
		}

		case CommandType::Possess:
		{
			if ( ( command.target & kSlotTargetBit ) == 0 )
			{
				return;
			}
			const uint32_t slot = command.target & ~kSlotTargetBit;
			if ( slot < uint32_t( kMaxPlayers ) )
			{
				Possess( PlayerSlot( slot ), FindEntity( command.other ) );
			}
			return;
		}

		case CommandType::Drive:
		{
			flecs::entity e = FindEntity( command.target );
			if ( e.is_valid() == false )
			{
				return;
			}
			if ( command.mode == 0 )
			{
				if ( e.has<ModDriven>() )
				{
					e.remove<ModDriven>();
					if ( e.get<Character>().dead != 0 && e.has<DrivenProp>() == false )
					{
						// Dead, it is its ragdoll or nothing (as a player's left dead): it goes, with
						// what it carried. A prop is given back.
						const uint32_t self = e.get<NetId>().value;
						for ( uint32_t item : CarriedItemsOf( self ) )
						{
							DestroyEntity( FindEntity( item ) );
						}
						DestroyEntity( e );
					}
					else
					{
						MakeIdle( e );
					}
				}
				return;
			}
			const PlayerInput in = UnpackInput( command );
			if ( e.has<ModDriven>() )
			{
				e.set<ModDriven>( { in } );
				return;
			}
			if ( Possessable( e ) == false )
			{
				return; // (a player's body, an item, a ragdoll, ...)
			}
			if ( e.has<IdleBody>() )
			{
				MakeDriven( e, kNoSlot );
			}
			else
			{
				DriveProp( e, kNoSlot );
			}
			e.set<ModDriven>( { in } );
			return;
		}

		case CommandType::SetCharacter:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() == false || e.has<Character>() == false || e.has<DrivenProp>() || command.index >= m_characters.size() )
			{
				return;
			}
			const uint16_t was = CharacterOf( e );
			const uint16_t now = uint16_t( command.index );
			if ( was == now )
			{
				return;
			}
			if ( now == 0 )
			{
				e.remove<CharacterKind>();
			}
			else
			{
				e.set<CharacterKind>( { now, 0 } );
			}
			// Its capsule: the new character's, with the feet where they were.
			const CharacterBody& from = Body( was );
			const CharacterBody& to = Body( now );
			Transform t = e.get<Transform>();
			t.position.y += mover::FeetBelowCenter( to.radius, to.halfHeight ) - mover::FeetBelowCenter( from.radius, from.halfHeight );
			PhysicsBody pb = e.get<PhysicsBody>();
			const bool enabled = b3Body_IsEnabled( BodyOf( pb ) );
			b3DestroyShape( ShapeOf( pb ), false );
			Shape shape{ ShapeKind::Capsule, {}, { to.radius, to.halfHeight, 0.0f } };
			b3ShapeId shapeId = CreateShape( BodyOf( pb ), shape, CatPlayer );
			b3Body_SetTransform( BodyOf( pb ), t.position, t.rotation );
			if ( enabled == false )
			{
				b3Body_Disable( BodyOf( pb ) );
			}
			e.set<Shape>( shape );
			e.set<PhysicsBody>( MakePhysicsBody( BodyOf( pb ), shapeId ) );
			e.set<Transform>( t );
			// Its state machine's layers start over: the new character's are not the old one's.
			AnimState anim = GetAnimState( e );
			for ( size_t l = 0; l < anim.graph.size(); ++l )
			{
				anim.graph[l] = AnimGraphLayerState{};
			}
			SetAnimState( e, anim );
			IndexesStale();
			return;
		}

		case CommandType::Break:
		{
			flecs::entity wall = FindEntity( ResolveTarget( command.target ) );
			if ( wall.is_valid() )
			{
				BreakWall( wall, ToVec( command.a ), std::max( command.b.y, 0.0f ), std::max( command.b.x, 0.0f ), command.other );
			}
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
				StartItemFields( item, command.index );
				PutItemInWorld( item, ToVec( command.a ), CommandRotation( command.c ), ToVec( command.b ) );
				return;
			}
			flecs::entity player = FindEntity( holder );
			if ( holder != 0 && player.is_valid() && player.has<Container>() )
			{
				// Into a container (a chest a mod fills): onto a stack of its kind with room, else the
				// first free slot; a full one takes nothing.
				flecs::entity item = CreateEntity();
				item.set<HeldItem>( { 0, command.index, kNoSocket, 1 } );
				StartItemFields( item, command.index );
				item.set<Transform>( player.get<Transform>() );
				if ( PutInContainer( player, item ) == false )
				{
					DestroyEntity( item );
				}
				return;
			}
			if ( holder == 0 || player.is_valid() == false || player.has<Character>() == false )
			{
				return;
			}
			if ( m_config.slots > 0 )
			{
				// Into a slot (its kind's own, or the first free one); with none, onto the floor.
				flecs::entity item = CreateEntity();
				item.set<HeldItem>( { 0, command.index, kNoSocket, 1 } );
				StartItemFields( item, command.index );
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
				StartItemFields( item, command.index );
				item.set<Transform>( player.get<Transform>() );
				return;
			}
			if ( uint32_t old = HeldItemOf( holder, command.mode ) )
			{
				// Dropped, not destroyed: it may be one someone picked up.
				const Transform& at = player.get<Transform>();
				QueueIntoWorld( FindEntity( old ), b3Add( at.position, b3Vec3{ 0.0f, 0.4f, 0.0f } ), at.rotation, b3Vec3{ 0.0f, 0.0f, 0.0f } );
			}
			flecs::entity item = CreateEntity();
			item.set<HeldItem>( { holder, command.index, command.mode, 0 } );
			StartItemFields( item, command.index );
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
				// Into a slot (a free one, or onto a stack of its kind with room), and out into the hand;
				// with no room for it, it stays where it lies.
				const ItemShape rule = ItemShapeOf( held->kind );
				bool room = false;
				for ( uint8_t s = 0; s < m_config.slots && room == false; ++s )
				{
					flecs::entity there = FindEntity( SlotItemOf( holder, s ) );
					const HeldItem* stack = there.is_valid() ? there.try_get<HeldItem>() : nullptr;
					room = stack == nullptr || ( rule.stack > 1 && stack->kind == held->kind && StackOf( *stack ) < rule.stack );
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
			if ( e.is_valid() == false || HasAnimState( e ) == false || command.index >= m_config.layers || command.value < 0 ||
				 command.value > 255 )
			{
				return;
			}
			AnimState a = GetAnimState( e );
			AnimGraphLayerState& layer = a.graph[command.index];
			uint8_t source = uint8_t( command.value );
			const std::vector<uint8_t>& fits = Body( CharacterOf( e ) ).packs;
			if ( source > 0 && fits.empty() == false && ( size_t( source - 1 ) >= fits.size() || fits[size_t( source - 1 )] == 0 ) )
			{
				source = 0; // a pack its skeleton cannot play: its own layer plays on
			}
			if ( layer.source != source )
			{
				layer.source = source;
				layer.started = 0; // starts over in the new layer's start state
			}
			SetAnimState( e, a );
			return;
		}

		case CommandType::Stance:
		{
			flecs::entity e = FindEntity( ResolveTarget( command.target ) );
			if ( e.is_valid() == false || HasAnimState( e ) == false || command.index >= m_config.layers ||
				 command.value < 0 || command.value > kMaxStances )
			{
				return;
			}
			AnimState a = GetAnimState( e );
			a.stances[command.index] = uint8_t( command.value );
			SetAnimState( e, a );
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
			if ( e.is_valid() && HasAnimState( e ) )
			{
				AnimState a = GetAnimState( e );
				a.aiming = command.mode != 0 ? 1 : 0;
				SetAnimState( e, a );
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
	b3Vec3 impulse = mode == ImpulseLinear ? vector : b3MulSV( b3Body_GetMass( body ), vector );
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
		if ( command.mode == ImpulseThrow )
		{
			// The whole body is thrown: every part the same, each through its own centre.
			for ( int i = 0; i < kRagdollParts; ++i )
			{
				b3BodyId part = rb->body[i];
				part.world0 = uint16_t( m_physicsWorld.index1 - 1 );
				PushBody( part, b3Body_GetWorldCenter( part ), vector, ImpulseVelocity );
			}
			return;
		}
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

	if ( command.mode == 1 && Body( CharacterOf( e ) ).ragdoll && e.has<DrivenProp>() == false )
	{
		// (Made from the player as it is now, when the physics events are applied: M246.)
		PhysicsEvent ragdoll = RagdollEvent( e, command.other );
		ragdoll.hitAt = command.a;
		ragdoll.hitVector = command.b;
		ragdoll.mode = uint8_t( ImpulseVelocity );
		ragdoll.count = uint8_t( std::clamp( command.value, 0, 255 ) );
		m_physicsQueue.push_back( std::move( ragdoll ) );
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

PhysicsEvent Simulation::RagdollEvent( flecs::entity player, uint32_t lifetimeTicks ) const
{
	const Transform t = player.get<Transform>();
	const Character c = player.get<Character>();
	const CharacterBody& kind = Body( CharacterOf( player ) );
	const b3Vec3 feet = b3Sub( t.position, b3Vec3{ 0.0f, mover::FeetBelowCenter( kind.radius, kind.halfHeight ), 0.0f } );
	PhysicsEvent event;
	event.type = PhysicsEventType::Ragdoll;
	event.netId = player.get<NetId>().value;
	event.slot = c.slot;
	event.at = { feet.x, feet.y, feet.z };
	event.yaw = c.facingYaw;
	event.vector = { c.velocity.x, c.velocity.y, c.velocity.z };
	event.mass = MoveOf( player )[MoveParam::Mass];
	event.ticks = lifetimeTicks;
	return event;
}

flecs::entity Simulation::CreateRagdoll( const PhysicsEvent& event )
{
	using namespace ragdoll;

	const b3Quat facing = detmath::YawRotation( event.yaw );
	const b3Vec3 feet = { event.at.x, event.at.y, event.at.z };
	const b3Vec3 velocity = { event.vector.x, event.vector.y, event.vector.z };

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
			case ShapeKind::Hull:
			case ShapeKind::Mesh:
			case ShapeKind::HeightField:
				break; // (no ragdoll part is one)
		}
	}
	const float density = event.mass / volume;

	RagdollBodies bodies;
	b3BodyId live[PartCount];
	for ( int i = 0; i < PartCount; ++i )
	{
		const PartDef& part = kParts[i];
		b3BodyDef def = b3DefaultBodyDef();
		def.type = b3_dynamicBody;
		def.position = b3Add( feet, b3RotateVector( facing, part.center ) );
		def.rotation = facing;
		def.linearVelocity = velocity;
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
		pose.linear[i] = velocity;
	}

	flecs::entity e = event.made != 0 ? EntityWithId( event.made ) : CreateEntity();
	Ragdoll r;
	r.owner = event.netId;
	r.slot = event.slot;
	r.spawnTick = m_globals.tick;
	r.despawnTick = event.ticks > 0 ? m_globals.tick + event.ticks : 0;
	r.yaw = event.yaw;
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
	if ( m_shapeLookupStale == false && s_checkIndexes == false )
	{
		return;
	}
	auto kept = m_shapeLookupStale ? decltype( m_shapeLookup )() : m_shapeLookup;
	bool wasStale = m_shapeLookupStale;
	m_shapeLookupStale = false;
	m_shapeLookup.clear();
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		if ( const PhysicsBody* pb = e.try_get<PhysicsBody>() )
		{
			// A body of several shapes is one entity whichever of them is hit.
			b3BodyId body = BodyOf( *pb );
			const int count = b3Body_IsValid( body ) ? b3Body_GetShapeCount( body ) : 0;
			if ( count <= 1 )
			{
				m_shapeLookup.push_back( { uint32_t( pb->shape.index1 ), r.netId } );
			}
			else
			{
				b3ShapeId shapes[kMapSetPieceLimit];
				const int got = b3Body_GetShapes( body, shapes, int( kMapSetPieceLimit ) );
				for ( int i = 0; i < got; ++i )
				{
					m_shapeLookup.push_back( { uint32_t( shapes[i].index1 ), r.netId } );
				}
			}
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
	if ( s_checkIndexes && wasStale == false && kept != m_shapeLookup )
	{
		std::fprintf( stderr, "BuildShapeLookup: kept as it was, but the world has changed\n" );
		std::abort();
	}
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
	// Its character's own (a dragon walks slower), where it is not the server's own: the server's
	// has its own in SimConfig::move already.
	for ( const auto& [param, value] : Body( CharacterOf( e ) ).move )
	{
		if ( param < kMoveParams && CharacterOf( e ) != 0 )
		{
			params.values[param] = value;
		}
	}
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
		if ( HasMotionState( e ) )
		{
			ApplyMotionParams( *m_motions, GetMotionState( e ), m_globals.tick, m_config.tickRate, params, e.try_get<MoveValues>() );
		}
	}
	// ... but what another's motion gave it for a while comes after: a slow holds through a dash.
	if ( const MoveEffects* given = e.try_get<MoveEffects>() )
	{
		// The newest last, so it wins (by when it was given, then by place: the same on every machine).
		int order[kMoveEffects];
		int count = 0;
		for ( int i = 0; i < kMoveEffects; ++i )
		{
			const MoveEffect& m = given->list[i];
			if ( m.untilTick != 0 && m.sinceTick <= m_globals.tick && m_globals.tick < m.untilTick && m.param < kMoveParams )
			{
				order[count++] = i;
			}
		}
		std::sort( order, order + count, [&]( int a, int b ) {
			return given->list[a].sinceTick != given->list[b].sinceTick ? given->list[a].sinceTick < given->list[b].sinceTick : a < b;
		} );
		for ( int k = 0; k < count; ++k )
		{
			params.values[given->list[order[k]].param] = given->list[order[k]].value;
		}
	}
	return params;
}

// --- Motions: holds and effects ---------------------------------------------------------------------

namespace
{

// A rope never reels in shorter than this, and nothing nearer than this is worth a throw of one. A
// probe with no rope (a punch, a grab) holds anything in front of the player.
constexpr float kHoldMinLength = 1.0f;
constexpr float kHoldMinReach = 0.05f;

float MinHoldLength( const Motion& m )
{
	bool rope = false;
	for ( const MotionStep& step : m.steps )
	{
		rope = rope || std::any_of( step.effects.begin(), step.effects.end(), []( const MotionEffect& e ) { return e.kind == MotionEffect::Kind::Link; } );
	}
	return rope ? kHoldMinLength : kHoldMinReach;
}
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
	// Along the camera's look (a hook aimed at the crosshair), or level along the body's facing
	// (a punch's: where the body faces is what it hits, whatever the camera does).
	const Character* body = m.probeFacing ? e.try_get<Character>() : nullptr;
	float yaw = body != nullptr ? body->facingYaw : detmath::YawToRadians( in.cameraYaw );
	b3CosSin pitch = body != nullptr ? detmath::CosSin( 0.0f ) : detmath::CosSin( float( in.cameraPitch ) * ( detmath::kTwoPi / 65536.0f ) );
	b3Vec3 forward = detmath::YawForward( yaw );
	b3Vec3 look = { forward.x * pitch.cosine, pitch.sine, forward.z * pitch.cosine };

	// What is under the crosshair: along the camera's line from where it passes the player (the
	// point a third-person camera orbits, moved to a shoulder if the camera is). Then the line itself
	// goes from the player to that point, so something in between stops it.
	b3Vec3 from = ViewPivot( e, t.position );
	b3Vec3 view = from;
	if ( ViewMode( in.view ) == ViewMode::ShoulderRight || ViewMode( in.view ) == ViewMode::ShoulderLeft )
	{
		view = b3MulAdd( view, ViewMode( in.view ) == ViewMode::ShoulderRight ? kShoulderOffset : -kShoulderOffset, detmath::YawRight( yaw ) );
	}
	RayHit seen;
	if ( m.probeTravel > 0.0f )
	{
		// A line that flies is a projectile: aimed now, at what is under the crosshair (or as far as
		// it reaches), and traced tick by tick on its way (FlyHold): what it meets first, it holds.
		b3Vec3 aim = CastRay( view, b3MulSV( m.probeRange, look ), self, seen ) ? seen.point : b3MulAdd( view, m.probeRange, look );
		b3Vec3 toward = b3Sub( aim, from );
		float far = b3Length( toward );
		if ( far < MinHoldLength( m ) )
		{
			return false;
		}
		MotionHold fly;
		fly.origin = from;
		fly.point = b3MulAdd( from, m.probeRange / far, toward );
		fly.startTick = m_globals.tick;
		fly.holdTick = m_globals.tick + std::max( 1u, uint32_t( m.probeRange / m.probeTravel * float( m_config.tickRate ) + 0.5f ) );
		fly.motion = uint16_t( index );
		fly.on = 1;
		fly.flying = 1;
		e.set<MotionHold>( fly );
		holdTick = fly.holdTick; // (until it meets something: then from that tick)
		return true;
	}
	// One that does not fly holds at once, on what is there, or is not thrown.
	if ( CastRay( view, b3MulSV( m.probeRange, look ), self, seen ) == false && CastProbe( m, from, b3MulSV( m.probeRange, look ), self, seen ) == false )
	{
		return false;
	}
	RayHit hit = seen;
	b3Vec3 to = b3Sub( seen.point, from );
	RayHit nearer;
	if ( CastProbe( m, from, b3MulSV( 1.02f, to ), self, nearer ) )
	{
		hit = nearer;
	}
	float distance = b3Distance( hit.point, from );
	if ( distance < MinHoldLength( m ) || ( HoldKind( hit ) & m.probeHolds ) == 0 )
	{
		return false;
	}

	MotionHold hold;
	HoldOn( hold, hit );
	hold.length = 0.0f; // a link measures its rope when the probe takes hold
	hold.startTick = m_globals.tick;
	hold.holdTick = m_globals.tick;
	hold.motion = uint16_t( index );
	hold.on = 1;
	e.set<MotionHold>( hold );
	holdTick = hold.holdTick;
	SayHold( e, m, hold.anchor, hit.point );
	return true;
}

void Simulation::SayHold( flecs::entity e, const Motion& m, uint32_t anchor, b3Vec3 point )
{
	// Its hold_emits, at the point it holds, with what it holds (0: the world).
	if ( m.holdEvent >= 0 )
	{
		ModEventRecord record;
		record.type = uint16_t( m.holdEvent );
		record.netIdA = e.get<NetId>().value;
		record.netIdB = anchor;
		record.tick = m_globals.tick;
		record.point = point;
		RecordModEvent( record );
	}
}

uint8_t Simulation::HoldKind( const RayHit& hit ) const
{
	flecs::entity a = FindEntity( hit.netId );
	if ( a.is_valid() && a.has<Character>() )
	{
		return kProbeHoldsPlayers;
	}
	if ( a.is_valid() && a.has<RagdollBodies>() )
	{
		return kProbeHoldsBodies;
	}
	if ( a.is_valid() && a.has<PhysicsBody>() && a.has<StaticGeometry>() == false &&
		 b3Body_GetType( BodyOf( a.get<PhysicsBody>() ) ) == b3_dynamicBody )
	{
		return kProbeHoldsBodies;
	}
	return kProbeHoldsLevel;
}

void Simulation::HoldOn( MotionHold& hold, const RayHit& hit ) const
{
	hold.anchor = 0;
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
}

void Simulation::FlyHold( flecs::entity e, MotionState& motion )
{
	const MotionHold* found = e.try_get<MotionHold>();
	if ( found == nullptr || found->on == 0 || found->flying == 0 || m_globals.tick <= found->startTick ||
		 found->holdTick <= found->startTick || size_t( found->motion ) >= motion.slots.size() )
	{
		return;
	}
	MotionHold hold = *found;
	// Where its tip was a tick ago, and where it is now: that stretch is traced.
	auto tip = [&]( uint32_t tick ) {
		float along = std::min( float( tick - hold.startTick ) / float( hold.holdTick - hold.startTick ), 1.0f );
		return b3Add( hold.origin, b3MulSV( along, b3Sub( hold.point, hold.origin ) ) );
	};
	const uint32_t now = m_globals.tick;
	b3Vec3 from = tip( now - 1 );
	b3Vec3 to = tip( now );
	RayHit hit;
	if ( CastProbe( m_motions->list[hold.motion], from, b3Sub( to, from ), e.get<NetId>().value, hit ) )
	{
		if ( ( HoldKind( hit ) & m_motions->list[hold.motion].probeHolds ) == 0 )
		{
			// Something it may not hold on to is in its way: it stops there, holding nothing, and is
			// gone (its line ends there, now).
			hold.point = hit.point;
			hold.holdTick = now;
			hold.flying = 0;
			hold.missed = 1;
			e.set<MotionHold>( hold );
			return;
		}
		HoldOn( hold, hit );
		hold.flying = 0;
		// It takes effect from now, for as long as it would have from where its range ends.
		MotionSlot& slot = motion.slots[hold.motion];
		if ( slot.untilTick != kForever && slot.untilTick >= slot.sinceTick )
		{
			slot.untilTick = now + ( slot.untilTick - slot.sinceTick );
		}
		slot.sinceTick = now;
		hold.holdTick = now;
		e.set<MotionHold>( hold );
		SayHold( e, m_motions->list[hold.motion], hold.anchor, hit.point );
		return;
	}
	if ( now >= hold.holdTick )
	{
		// Out of its range, and nothing met: it holds nothing, and its motion ends.
		hold.flying = 0;
		hold.missed = 1;
		e.set<MotionHold>( hold );
	}
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
	auto entity = [&]( flecs::entity who, const b3Vec3* at ) {
		end = EndOf( self, t, who );
		if ( at != nullptr && ( end.kind == MotionEnd::Kind::Player || end.kind == MotionEnd::Kind::Body ) )
		{
			end.point = *at;
		}
	};
	switch ( target.kind )
	{
		case MotionTarget::Kind::Self:
			end = EndOf( self, t, self );
			break;
		case MotionTarget::Kind::Found:
			break; // (a find's: RunFind)
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

Simulation::MotionEnd Simulation::EndOf( flecs::entity self, const Transform& t, flecs::entity who ) const
{
	MotionEnd end;
	if ( who.is_valid() == false )
	{
		return end;
	}
	auto player = [&]( MotionEnd::Kind kind ) {
		end.kind = kind;
		end.entity = who;
		end.point = kind == MotionEnd::Kind::Self ? ViewPivot( self, t.position ) : ViewPivot( who, who.get<Transform>().position );
		end.mass = MoveOf( who )[MoveParam::Mass];
	};
	if ( who == self )
	{
		player( MotionEnd::Kind::Self );
	}
	else if ( const Character* other = who.try_get<Character>() )
	{
		if ( other->dead == 0 )
		{
			player( MotionEnd::Kind::Player );
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
			end.point = b3Body_GetWorldCenter( body );
			end.mass = b3Body_GetMass( body );
		}
	}
	return end;
}

void Simulation::ImpulseEnd( const MotionEffect& effect, float strength, const MotionEnd& target, Character& c, b3Vec3 direction )
{
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
	// Momentum (N s) moves a heavy target less: its change of velocity is the momentum over its mass.
	const float change = effect.momentum ? ( target.mass > 0.0f ? strength / target.mass : 0.0f ) : strength;
	if ( target.kind == MotionEnd::Kind::Player && m_deferPlayerPushes )
	{
		// Another player, while the motions run: what it replaces is taken from its velocity when the
		// push lands (after its own motions), not from what it had when this was decided.
		m_playerPushes.push_back( { target.entity.get<NetId>().value, effect.replace, b3MulSV( change, direction ) } );
		return;
	}
	PushEnd( target, c, b3Add( b3Sub( keep, had ), b3MulSV( change, direction ) ) );
}

void Simulation::GiveMove( flecs::entity who, uint8_t param, float value, float seconds )
{
	MoveEffects given = who.has<MoveEffects>() ? who.get<MoveEffects>() : MoveEffects{};
	const uint32_t now = m_globals.tick;
	// A place that is over, else the one given longest ago (the first such: the same everywhere).
	int place = 0;
	for ( int i = 0; i < kMoveEffects; ++i )
	{
		const MoveEffect& m = given.list[i];
		if ( m.untilTick == 0 || m.untilTick <= now )
		{
			place = i;
			break;
		}
		if ( m.sinceTick < given.list[place].sinceTick )
		{
			place = i;
		}
	}
	MoveEffect& m = given.list[place];
	m = MoveEffect{};
	m.sinceTick = now;
	// Its seconds from the next tick, when the mover first reads it (one tick at least).
	m.untilTick = now + 1 + std::max<uint32_t>( 1, uint32_t( seconds * float( m_config.tickRate ) + 0.5f ) );
	m.param = param;
	m.value = value;
	who.set<MoveEffects>( given );
}

void Simulation::RunBreaks( const Motion& m, const MotionStep& step, const MotionSlot& slot, b3Vec3 at, const AnimGraphInputs& values )
{
	for ( const MotionStep::Break& b : step.breaks )
	{
		if ( MotionGuardHolds( slot, b.guard ) == false )
		{
			continue;
		}
		const float radius = MotionValueOf( m, b.radius, values, 0.0f, 20.0f );
		const float speed = MotionValueOf( m, b.speed, values, 0.0f, 200.0f );
		const float seconds = MotionValueOf( m, b.seconds, values, 0.0f, 3600.0f );
		if ( radius <= 0.0f )
		{
			continue;
		}
		const uint32_t rubble = uint32_t( seconds * float( m_config.tickRate ) );
		for ( uint32_t wall : BreakablesNear( at, radius ) )
		{
			// (When the physics events are applied: M246.)
			PhysicsEvent hit;
			hit.type = PhysicsEventType::Break;
			hit.netId = wall;
			hit.at = { at.x, at.y, at.z };
			hit.vector = { radius, speed, 0.0f };
			hit.ticks = rubble;
			m_physicsQueue.push_back( std::move( hit ) );
		}
	}
}

void Simulation::RunItemMotions( flecs::entity item, int event, bool expiring )
{
	const HeldItem* held = item.is_valid() ? item.try_get<HeldItem>() : nullptr;
	if ( held == nullptr || held->holder != 0 || !m_motions || size_t( held->kind ) >= m_itemMotions.size() || event < 0 )
	{
		return;
	}
	const uint32_t itemId = item.get<NetId>().value;
	const Prop* prop = item.try_get<Prop>();
	const uint32_t thrower = prop != nullptr ? prop->owner : 0;
	const Transform t = item.get<Transform>();
	for ( uint16_t index : m_itemMotions[size_t( held->kind )] )
	{
		const Motion& m = m_motions->list[index];
		if ( m.trigger != event || item.is_valid() == false )
		{
			continue;
		}
		// What its expressions read: the item's fields, the game's, its curves.
		Blackboard board = HasBoard( item ) ? GetBoard( item ) : Blackboard{};
		AnimGraphInputs values;
		values.board = &board.values;
		values.globalBoard = &m_globalBoard;
		values.events = m_globals.modEvents;
		values.eventCount = m_globals.modEventCount;
		values.tick = m_globals.tick;
		values.hearLate = true;
		values.netId = itemId;
		values.curves = &m.curves;
		const PhysicsBody* pb = item.try_get<PhysicsBody>();
		values.builtins[AnimExpr::Speed] = pb != nullptr ? b3Length( b3Body_GetLinearVelocity( BodyOf( *pb ) ) ) : 0.0f;
		const MotionStep& step = m.steps.front();
		MotionSlot slot;
		slot.guards = MotionGuardBits( step, values );
		// Its fields, then what it says (as its thrower's: whose blast it is), then what it does.
		bool changed = false;
		for ( const MotionStep::Change& change : step.changes )
		{
			if ( MotionGuardHolds( slot, change.guard ) && HasBoard( item ) )
			{
				int32_t& stored = board.values[change.slot];
				float value = change.type == BoardType::Float ? BoardToFloat( stored ) : float( stored );
				const float amount = MotionAmount( m, change.value, change.expr, values, -1.0e9f, 1.0e9f );
				value = change.op == Motion::ChangeOp::Set ? amount : change.op == Motion::ChangeOp::Add ? value + amount : value - amount;
				stored = change.type == BoardType::Float ? BoardFromFloat( value ) : int32_t( value );
				changed = true;
			}
		}
		if ( changed )
		{
			SetBoard( item, board );
		}
		for ( const MotionStep::Emit& emit : step.emits )
		{
			if ( emit.event >= 0 && MotionGuardHolds( slot, emit.guard ) )
			{
				ModEventRecord record;
				record.type = uint16_t( emit.event );
				record.netIdA = thrower;
				record.netIdB = itemId;
				record.tick = m_globals.tick;
				record.point = t.position;
				record.late = 1;
				RecordModEvent( record );
			}
		}
		RunBreaks( m, step, slot, t.position, values );
		// Itself: pushed; what it finds (as its own player has none: no look, no facing).
		Character none;
		PlayerInput still;
		const MotionEnd self = EndOf( flecs::entity(), t, item );
		for ( const MotionEffect& effect : step.effects )
		{
			if ( MotionGuardHolds( slot, effect.guard ) && self.kind == MotionEnd::Kind::Body )
			{
				b3Vec3 direction = MotionTurn( effect, MotionDirection( effect.frame, effect.direction, none, still ) );
				ImpulseEnd( effect, MotionStrength( m, effect, values, effect.momentum ? kMotionMomentumLimit : kMotionImpulseLimit ), self, none, direction );
			}
		}
		for ( const MotionFind& find : step.finds )
		{
			if ( MotionGuardHolds( slot, find.guard ) )
			{
				RunFind( item, none, t, still, m, find, self, MotionEnd{}, values, thrower );
			}
		}
		bool remove = false;
		for ( int8_t guard : step.removes )
		{
			remove = remove || MotionGuardHolds( slot, guard );
		}
		if ( remove && expiring == false )
		{
			DestroyEntity( item );
			return;
		}
	}
}

void Simulation::RunItemEvents()
{
	if ( m_itemMotions.empty() )
	{
		return;
	}
	// This tick's events said at a live item that has motions (newest first in the ring: gathered,
	// then run oldest first, the order they were said).
	std::vector<std::pair<uint32_t, int>> said;
	const uint32_t kept = std::min( m_globals.modEventCount, kModEventHistory );
	for ( uint32_t i = 0; i < kept; ++i )
	{
		const ModEventRecord& e = m_globals.modEvents[( m_globals.modEventCount - 1 - i ) % kModEventHistory];
		if ( e.tick != m_globals.tick )
		{
			break;
		}
		said.push_back( { e.netIdA, int( e.type ) } );
	}
	for ( size_t i = said.size(); i-- > 0; )
	{
		flecs::entity item = said[i].first != 0 ? FindEntity( said[i].first ) : flecs::entity();
		const HeldItem* held = item.is_valid() && StandsIn( item ) == false ? item.try_get<HeldItem>() : nullptr;
		if ( held != nullptr && size_t( held->kind ) < m_itemMotions.size() && m_itemMotions[size_t( held->kind )].empty() == false )
		{
			RunItemMotions( item, said[i].second, false );
		}
	}
}

std::vector<uint32_t> Simulation::BreakablesNear( b3Vec3 point, float radius ) const
{
	// Per wall (the NetId it had whole): the nearest piece, and what to break it by.
	struct Near
	{
		uint32_t wall = 0;
		float distance = 0.0f;
		uint32_t target = 0;
	};
	std::vector<Near> walls;
	auto consider = [&]( uint32_t wall, float distance, uint32_t target ) {
		if ( distance > radius )
		{
			return;
		}
		for ( Near& n : walls )
		{
			if ( n.wall == wall )
			{
				if ( distance < n.distance )
				{
					n.distance = distance;
					n.target = target;
				}
				return;
			}
		}
		walls.push_back( { wall, distance, target } );
	};
	for ( const EntityRef& r : m_entities )
	{
		flecs::entity e( m_world, r.entity );
		const Transform& at = e.get<Transform>();
		if ( const WallPiece* piece = e.try_get<WallPiece>() )
		{
			consider( piece->wall, b3Length( b3Sub( at.position, point ) ), r.netId );
			continue;
		}
		const TemplateRef* ref = e.try_get<TemplateRef>();
		if ( ref == nullptr || ref->index >= m_map.templates.size() || m_map.templates[ref->index].pieces.empty() )
		{
			continue;
		}
		for ( const TemplatePiece& piece : m_map.templates[ref->index].pieces )
		{
			const b3Vec3 where = b3Add( at.position, b3RotateVector( at.rotation, piece.position ) );
			consider( r.netId, b3Length( b3Sub( where, point ) ), r.netId );
		}
	}
	std::vector<std::pair<float, uint32_t>> found;
	for ( const Near& n : walls )
	{
		found.push_back( { n.distance, n.target } );
	}
	std::stable_sort( found.begin(), found.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
	std::vector<uint32_t> out;
	for ( const auto& [distance, netId] : found )
	{
		out.push_back( netId );
	}
	return out;
}

void Simulation::RunFind( flecs::entity e, Character& c, const Transform& t, const PlayerInput& in, const Motion& m, const MotionFind& find,
						  const MotionEnd& self, const MotionEnd& hit, const AnimGraphInputs& motionValues, uint32_t sayer )
{
	const uint32_t selfId = e.get<NetId>().value;
	sayer = sayer != 0 ? sayer : selfId;
	// The shape, turned as the player faces (or looks), at its feet.
	b3Quat turn = b3Quat_identity;
	if ( find.frame == MotionFrame::Facing )
	{
		turn = detmath::YawRotation( c.facingYaw );
	}
	else if ( find.frame == MotionFrame::Look )
	{
		float pitch = float( in.cameraPitch ) * ( detmath::kTwoPi / 65536.0f );
		turn = b3MulQuat( detmath::YawRotation( detmath::YawToRadians( in.cameraYaw ) ),
						  b3MakeQuatFromAxisAngle( b3Vec3{ 1.0f, 0.0f, 0.0f }, -pitch ) );
	}
	std::vector<uint32_t>& found = m_findScratch;
	found.clear();
	{
		PhysicsArena::Scope scope( *m_arena );
		BuildShapeLookup();
		b3Vec3 points[128];
		for ( const MotionFind::Piece& piece : find.pieces )
		{
			int count = int( std::min<size_t>( piece.points.size(), 128 ) );
			for ( int i = 0; i < count; ++i )
			{
				points[i] = b3RotateVector( turn, piece.points[size_t( i )] );
			}
			const b3ShapeProxy proxy = { points, count, piece.radius };
			b3QueryFilter filter = { ~uint64_t( 0 ), ~uint64_t( CatPusher ), 0, nullptr };
			struct Found
			{
				const Simulation* sim;
				std::vector<uint32_t>* out;
			} ctx{ this, &found };
			auto each = []( b3ShapeId shape, void* context ) -> bool {
				auto* f = static_cast<Found*>( context );
				uint32_t netId = f->sim->NetIdOfShape( shape );
				if ( netId != 0 )
				{
					f->out->push_back( netId );
				}
				return true; // all of them
			};
			b3World_OverlapShape( m_physicsWorld, t.position, &proxy, filter, each, &ctx );
		}
	}
	// What it may find, nearest first (then by id: the same order on every machine), at most so many.
	std::sort( found.begin(), found.end() );
	found.erase( std::unique( found.begin(), found.end() ), found.end() );
	struct Target
	{
		float distance;
		uint32_t netId;
		MotionEnd end;
	};
	std::vector<Target> targets;
	for ( uint32_t netId : found )
	{
		if ( netId == selfId && find.self == false )
		{
			continue;
		}
		MotionEnd end = EndOf( e, t, FindEntity( netId ) );
		bool player = end.kind == MotionEnd::Kind::Player || end.kind == MotionEnd::Kind::Self;
		if ( end.kind == MotionEnd::Kind::None || ( player && ( find.who & kProbeHoldsPlayers ) == 0 ) ||
			 ( end.kind == MotionEnd::Kind::Body && ( find.who & kProbeHoldsBodies ) == 0 ) )
		{
			continue;
		}
		targets.push_back( { b3Distance( end.point, t.position ), netId, end } );
	}
	std::sort( targets.begin(), targets.end(),
			   []( const Target& a, const Target& b ) { return a.distance != b.distance ? a.distance < b.distance : a.netId < b.netId; } );
	if ( targets.size() > size_t( find.most ) )
	{
		targets.resize( size_t( find.most ) );
	}

	for ( const Target& target : targets )
	{
		// What its strengths read about it: how far it is, and how far out along the ground.
		AnimGraphInputs values = motionValues;
		values.builtins[AnimExpr::Distance] = target.distance;
		b3Vec3 out = b3Sub( target.end.point, t.position );
		float along = std::sqrt( out.x * out.x + out.z * out.z );
		float reach = find.extent > 0.0f ? along / find.extent : 0.0f;
		values.builtins[AnimExpr::Reach] = reach < 1.0f ? reach : 1.0f;
		for ( const MotionEffect& effect : find.effects )
		{
			const MotionEnd& on = effect.target.kind == MotionTarget::Kind::Self ? self
								  : effect.target.kind == MotionTarget::Kind::Hit ? hit
																				  : target.end;
			if ( on.kind == MotionEnd::Kind::None || on.kind == MotionEnd::Kind::World )
			{
				continue;
			}
			b3Vec3 direction = {};
			float length = 0.0f;
			if ( effect.frame == MotionFrame::Away )
			{
				// Along the ground, from the player out to it; right on top of it, where it faces.
				b3Vec3 out = b3Sub( target.end.point, t.position );
				direction = b3GetLengthAndNormalize( &length, b3Vec3{ out.x, 0.0f, out.z } );
				direction = length < 0.001f ? detmath::YawForward( c.facingYaw ) : direction;
			}
			else if ( effect.frame == MotionFrame::To )
			{
				direction = b3GetLengthAndNormalize( &length, b3Sub( target.end.point, self.point ) );
				if ( length < 0.001f )
				{
					continue;
				}
			}
			else
			{
				direction = MotionDirection( effect.frame, effect.direction, c, in );
			}
			direction = MotionTurn( effect, direction );
			ImpulseEnd( effect, MotionStrength( m, effect, values, effect.momentum ? kMotionMomentumLimit : kMotionImpulseLimit ), on, c, direction );
		}
		// A player it found moves as it is given, for its seconds.
		if ( target.end.kind == MotionEnd::Kind::Player || target.end.kind == MotionEnd::Kind::Self )
		{
			for ( const MotionFind::Move& move : find.moves )
			{
				GiveMove( target.end.entity, move.param, MotionAmount( m, move.value, move.expr, values, MoveParamInfoOf( move.param ).min, MoveParamInfoOf( move.param ).max ), move.seconds );
			}
		}
		// The owner's fields, once for each thing found.
		if ( find.changes.empty() == false )
		{
			Blackboard board = GetBoard( e );
			for ( MotionFind::Change change : find.changes )
			{
				change.value = MotionAmount( m, change.value, change.expr, values, -1.0e9f, 1.0e9f );
				ApplyFindChange( change, board );
			}
			SetBoard( e, board );
		}
		for ( int event : find.emits )
		{
			if ( event < 0 )
			{
				continue;
			}
			ModEventRecord record;
			record.type = uint16_t( event );
			record.netIdA = sayer;
			record.netIdB = target.netId;
			record.tick = m_globals.tick;
			record.point = target.end.point;
			record.late = 1; // (the motions have run: they hear it next tick)
			RecordModEvent( record );
		}
	}
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
			if ( m_deferPlayerPushes )
			{
				// While the motions run: kept for when every player's have (ApplyPlayerPushes).
				m_playerPushes.push_back( { end.entity.get<NetId>().value, MotionEffect::Replace::None, change } );
				break;
			}
			Character other = end.entity.get<Character>();
			pushed( other, change );
			end.entity.set<Character>( other );
			break;
		}
		case MotionEnd::Kind::Body:
		{
			// (When the physics events are applied: M246.)
			const b3Vec3 impulse = b3MulSV( end.mass, change );
			PhysicsEvent push;
			push.type = PhysicsEventType::Push;
			push.netId = end.entity.get<NetId>().value;
			push.at = { end.point.x, end.point.y, end.point.z };
			push.vector = { impulse.x, impulse.y, impulse.z };
			m_physicsQueue.push_back( std::move( push ) );
			break;
		}
		default:
			break;
	}
}

std::string Simulation::DescribeMotions( PlayerSlot slot ) const
{
	const uint32_t netId = slot < kMaxPlayers ? m_globals.playerNetIds[slot] : 0;
	const flecs::entity e = netId != 0 ? FindEntity( netId ) : flecs::entity();
	if ( !m_motions || e.is_valid() == false || HasMotionState( e ) == false )
	{
		return std::string();
	}
	const MotionState state = GetMotionState( e );
	const uint32_t tick = m_globals.tick;
	const float dt = m_config.TimeStep();
	const MotionHold* hold = e.try_get<MotionHold>();
	std::string out;
	char line[512];
	for ( size_t i = 0; i < m_motions->list.size(); ++i )
	{
		const Motion& m = m_motions->list[i];
		const MotionSlot s = state.slots[i];
		if ( m.ownerItem || s.lastTick == 0 || tick >= s.untilTick || size_t( s.step ) >= m.steps.size() )
		{
			continue;
		}
		std::string what = m.name;
		if ( m.steps.size() > 1 )
		{
			std::snprintf( line, sizeof( line ), "  step %u of %zu", unsigned( s.step ) + 1u, m.steps.size() );
			what += line;
			const int next = m.steps[s.step].next;
			if ( next >= 0 )
			{
				const MotionStep& waits = m.steps[size_t( next )];
				what += "  waits for ";
				what += waits.waitText.empty() ? std::string() : waits.waitText;
				if ( waits.after > 0.0f )
				{
					std::snprintf( line, sizeof( line ), "%s%.1f s", waits.waitText.empty() ? "" : " after ", double( waits.after ) );
					what += line;
				}
				if ( waits.within > 0.0f )
				{
					std::snprintf( line, sizeof( line ), " (gives up after %.1f s)", double( waits.within ) );
					what += line;
				}
			}
		}
		std::snprintf( line, sizeof( line ), "  for %.1f s", double( float( tick - std::min( tick, MotionStepStart( s ) ) ) * dt ) );
		what += line;
		if ( s.untilTick != kForever )
		{
			std::snprintf( line, sizeof( line ), ", %.1f s left", double( float( s.untilTick - tick ) * dt ) );
			what += line;
		}
		if ( m.probe && hold != nullptr && hold->on != 0 && hold->motion == i )
		{
			if ( hold->flying != 0 && tick < hold->holdTick )
			{
				what += "  its cast flies";
			}
			else if ( hold->missed != 0 )
			{
				what += "  its cast found nothing";
			}
			else
			{
				std::snprintf( line, sizeof( line ), "  holds %s", hold->anchor != 0 ? ( "#" + std::to_string( hold->anchor ) ).c_str() : "the level" );
				what += line;
			}
		}
		out += what + "\n";
	}
	// The last start refused, for a while.
	const MotionNote& note = m_motionNotes[slot];
	if ( note.why != MotionRefusal::Why::None && size_t( note.motion ) < m_motions->list.size() && tick >= note.tick &&
		 float( tick - note.tick ) * dt <= 3.0f )
	{
		const Motion& m = m_motions->list[note.motion];
		std::string why = MotionRefusalText( note.why );
		if ( note.why == MotionRefusal::Why::Condition && m.conditionText.empty() == false )
		{
			why += ": " + m.conditionText;
		}
		else if ( note.why == MotionRefusal::Why::Cooldown )
		{
			const MotionSlot s = state.slots[note.motion];
			const float left = m.cooldown - float( note.tick + 1 - s.lastTick ) * dt;
			std::snprintf( line, sizeof( line ), " (%.1f s left then)", double( std::max( left, 0.0f ) ) );
			why += line;
		}
		std::snprintf( line, sizeof( line ), "%s did not start %.1f s ago: %s\n", m.name.c_str(), double( float( tick - note.tick ) * dt ), why.c_str() );
		out += line;
	}
	return out;
}

void Simulation::KeepMoveValues( flecs::entity e, uint16_t motion, const MotionStep& step, const MotionSlot& slot, const AnimGraphInputs& values )
{
	const MoveValues* had = e.try_get<MoveValues>();
	bool any = false;
	for ( const MotionStep::Param& p : step.params )
	{
		any = any || p.expr >= 0;
	}
	if ( any == false && had == nullptr )
	{
		return; // (a player whose motions read no move from an expression never has the component)
	}
	MoveValues kept = had != nullptr ? *had : MoveValues{};
	// This motion's places are taken again: what its new step reads.
	for ( MoveValue& place : kept.list )
	{
		place = place.motion == uint16_t( motion + 1 ) ? MoveValue{} : place;
	}
	const Motion& m = m_motions->list[motion];
	for ( size_t k = 0; k < step.params.size(); ++k )
	{
		const MotionStep::Param& p = step.params[k];
		if ( p.expr < 0 || MotionGuardHolds( slot, p.guard ) == false )
		{
			continue;
		}
		const MoveParamInfo& info = MoveParamInfoOf( p.param );
		for ( MoveValue& place : kept.list )
		{
			if ( place.motion == 0 )
			{
				place.motion = uint16_t( motion + 1 );
				place.index = uint8_t( k );
				place.value = MotionAmount( m, p.value, p.expr, values, info.min, info.max );
				break;
			}
		}
	}
	e.set<MoveValues>( kept );
}

bool Simulation::CharactersOf( const ModSchema& schema, std::vector<CharacterBody>& out, std::string& error )
{
	out.clear();
	for ( const CharacterInfo& info : schema.characters )
	{
		CharacterBody body;
		body.radius = info.radius;
		body.halfHeight = info.halfHeight;
		body.move = info.move;
		body.firstPerson = info.firstPerson;
		body.ragdoll = info.ragdoll;
		body.packs = info.packs;
		if ( info.graph.empty() == false )
		{
			std::string why, warnings;
			body.graph = CompileAnimGraph( info.graph, schema, why, warnings );
			if ( body.graph == nullptr )
			{
				error = "character " + ( info.name.empty() ? std::string( "built-in" ) : info.name ) + ": " + why;
				return false;
			}
		}
		out.push_back( std::move( body ) );
	}
	if ( out.empty() )
	{
		out.resize( 1 );
	}
	return true;
}

void Simulation::SetCharacters( std::vector<CharacterBody> characters )
{
	if ( characters.empty() )
	{
		characters.resize( 1 );
	}
	m_characters = std::move( characters );
	m_someSeenFromBehind = std::any_of( m_characters.begin(), m_characters.end(), []( const CharacterBody& c ) { return c.firstPerson == false; } );
}

uint16_t Simulation::CharacterOf( flecs::entity e ) const
{
	if ( const IdleBody* idle = e.is_valid() ? e.try_get<IdleBody>() : nullptr )
	{
		return idle->character < m_characters.size() ? idle->character : uint16_t( 0 );
	}
	const CharacterKind* kind = e.is_valid() ? e.try_get<CharacterKind>() : nullptr;
	return kind != nullptr && kind->index < m_characters.size() ? kind->index : uint16_t( 0 );
}

bool Simulation::AnyGraph() const
{
	for ( const CharacterBody& c : m_characters )
	{
		if ( c.graph )
		{
			return true;
		}
	}
	return false;
}

void Simulation::ApplyPlayerPushes()
{
	for ( const PlayerPush& push : m_playerPushes )
	{
		flecs::entity who = FindEntity( push.netId );
		if ( who.is_valid() == false || who.has<Character>() == false )
		{
			continue;
		}
		Character other = who.get<Character>();
		const b3Vec3 had = other.velocity;
		b3Vec3 keep = had;
		switch ( push.replace )
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
		const b3Vec3 by = b3Add( b3Sub( keep, had ), push.change );
		other.velocity = b3Add( had, by );
		if ( by.y > 0.0f && other.velocity.y > 0.0f )
		{
			other.grounded = 0; // lifted off, as a jump is (PushEnd)
		}
		who.set<Character>( other );
	}
	m_playerPushes.clear();
}

void Simulation::ApplyMotionEffects( flecs::entity e, Character& c, const Transform& t, const PlayerInput& in, const MotionState& state,
									 const Blackboard& board, const std::vector<MotionActive>& active, const AnimGraphInputs& playerValues )
{
	const float dt = m_config.TimeStep();
	for ( const MotionActive& on : active )
	{
		const Motion& m = m_motions->list[on.index];
		const MotionSlot slot = state.slots[on.index];
		// What the motion's probe holds on to: its effects wait for it to take hold.
		const MotionHold* found = e.try_get<MotionHold>();
		MotionHold hold = found != nullptr ? *found : MotionHold{};
		const bool holds = hold.on != 0 && hold.motion == on.index && hold.missed == 0;
		if ( m.probe && ( holds == false || m_globals.tick < hold.holdTick ) )
		{
			continue;
		}
		bool holdChanged = false;
		// What its strengths read: the player's values, its curves, and the time since its step started.
		AnimGraphInputs values = playerValues;
		values.curves = &m.curves;
		values.builtins[AnimExpr::Time] = float( m_globals.tick - std::min( m_globals.tick, MotionStepStart( slot ) ) ) * dt;
		const MotionEnd self = ResolveEnd( e, t, MotionTarget{}, nullptr, board );
		const MotionTarget hitTarget{ MotionTarget::Kind::Hit, 0, true };
		const MotionEnd hit = m.probe ? ResolveEnd( e, t, hitTarget, &hold, board ) : MotionEnd{};

		if ( size_t( slot.step ) >= m.steps.size() )
		{
			continue;
		}
		if ( on.started )
		{
			KeepMoveValues( e, on.index, m.steps[slot.step], slot, values );
		}
		// Its current step's: the once ones when the step starts, the lasting ones while it is the step
		// (and its repeat's, on the ticks it happens).
		auto runEffects = [&]( const std::vector<MotionEffect>& effects, bool startsNow ) {
		for ( const MotionEffect& effect : effects )
		{
			if ( MotionGuardHolds( slot, effect.guard ) == false )
			{
				continue;
			}
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
			direction = MotionTurn( effect, direction );

			if ( effect.kind == MotionEffect::Kind::Follow )
			{
				// Carried along its path: this tick's stretch of it, turned as the player faces, as
				// its velocity (the mover still meets walls on the way).
				if ( target.kind != MotionEnd::Kind::Self || effect.path < 0 || size_t( effect.path ) >= m.paths.size() )
				{
					continue;
				}
				const uint32_t ticksOn = m_globals.tick - std::min( m_globals.tick, MotionStepStart( slot ) );
				const float total = std::max( effect.seconds, dt );
				const float u0 = std::min( 1.0f, float( ticksOn ) * dt / total );
				const float u1 = std::min( 1.0f, float( ticksOn + 1 ) * dt / total );
				if ( u0 >= 1.0f )
				{
					continue;
				}
				const Motion::Path& path = m.paths[size_t( effect.path )];
				b3Vec3 stretch = b3RotateVector( detmath::YawRotation( c.facingYaw ), b3Sub( MotionPathAt( path, u1 ), MotionPathAt( path, u0 ) ) );
				PushEnd( target, c, b3Sub( b3MulSV( 1.0f / dt, stretch ), c.velocity ) );
				continue;
			}
			if ( effect.kind == MotionEffect::Kind::Launch )
			{
				if ( startsNow == false || effect.known == false )
				{
					continue;
				}
				// From where the player's look starts, a little ahead (clear of its own capsule), turned
				// as the camera is; with the player's own velocity, so a throw on the run goes further.
				float yaw = detmath::YawToRadians( in.cameraYaw );
				b3Vec3 from = ViewPivot( e, t.position );
				const float speed = MotionStrength( m, effect, values, kMotionImpulseLimit );
				const float lift = MotionAmount( m, effect.lift, effect.liftExpr, values, -kMotionImpulseLimit, kMotionImpulseLimit );
				const float ahead = MotionAmount( m, effect.ahead, effect.aheadExpr, values, 0.0f, 5.0f );
				const float seconds = MotionAmount( m, effect.seconds, effect.secondsExpr, values, 0.0f, 600.0f );
				b3Vec3 velocity = b3Add( b3Add( b3MulSV( speed, direction ), b3Vec3{ 0.0f, lift, 0.0f } ), c.velocity );
				uint32_t lifetime = seconds > 0.0f ? std::max<uint32_t>( 1, uint32_t( seconds * float( m_config.tickRate ) + 0.5f ) ) : 0u;
				uint32_t copy = 0;
				if ( effect.carried )
				{
					copy = CarriedCopyOf( e.get<NetId>().value, effect.itemKind );
					if ( copy == 0 )
					{
						continue; // none left to throw
					}
				}
				// From a marker: its place around the player, turned with its heading; else ahead of
				// where its look starts.
				b3Vec3 start = effect.fromMarker ? b3Add( t.position, b3RotateVector( detmath::YawRotation( yaw ), effect.from ) )
												 : b3Add( from, b3MulSV( ahead, direction ) );
				m_launched.push_back( { effect.itemKind, start, detmath::YawRotation( yaw ), velocity, e.get<NetId>().value, lifetime, copy } );
				continue;
			}
			if ( effect.kind == MotionEffect::Kind::Impulse )
			{
				if ( startsNow == false || target.kind == MotionEnd::Kind::World )
				{
					continue;
				}
				ImpulseEnd( effect, MotionStrength( m, effect, values, effect.momentum ? kMotionMomentumLimit : kMotionImpulseLimit ), target, c, direction );
			}
			else if ( effect.kind == MotionEffect::Kind::Force )
			{
				if ( target.kind == MotionEnd::Kind::World || target.mass <= 0.0f )
				{
					continue;
				}
				const float ramp = MotionRamp( effect, m_globals.tick - MotionStepStart( slot ), m_config.tickRate );
				const float strength = MotionStrength( m, effect, values, MotionForceLimit( effect ) );
				const float along = b3Dot( EndVelocity( target, c ), direction );
				// Its speed (the top, or the speed it brings the target to), read when it acts.
				const float top = MotionAmount( m, effect.speed, effect.speedExpr, values, -kMotionImpulseLimit, kMotionImpulseLimit );
				float change = 0.0f;
				switch ( effect.push )
				{
					case MotionEffect::Push::Acceleration:
						change = strength * ramp * dt;
						break;
					case MotionEffect::Push::Force:
						change = strength * ramp / target.mass * dt;
						break;
					case MotionEffect::Push::Velocity:
					{
						// Toward the speed, no faster than its rate lets it.
						float rate = std::fabs( strength ) * ramp * dt;
						change = std::clamp( top - along, -rate, rate );
						// With a reaction on something that can move, the speed is the two's toward
						// each other, and each takes its share of the change by the other's weight:
						// the light one does the moving, and the two never close faster than the
						// speed. (Its own speed alone, mirrored, would hand a light body a heavy
						// one's whole momentum again and again.)
						const MotionEnd& other = target.kind == MotionEnd::Kind::Self ? hit : self;
						if ( effect.react && other.mass > 0.0f && other.kind != MotionEnd::Kind::None && other.kind != MotionEnd::Kind::World )
						{
							float closing = along - b3Dot( EndVelocity( other, c ), direction );
							change = std::clamp( top - closing, -rate, rate ) * other.mass / ( target.mass + other.mass );
						}
						break;
					}
				}
				if ( effect.push != MotionEffect::Push::Velocity && top > 0.0f )
				{
					// A top speed along the push: past it, it pushes no more.
					float room = change >= 0.0f ? top - along : top + along;
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
				float length = MotionAmount( m, effect.length, effect.lengthExpr, values, 0.0f, 500.0f );
				if ( length <= 0.0f )
				{
					if ( hold.length <= 0.0f )
					{
						hold.length = std::max( distance, kHoldMinLength );
					}
					hold.length = std::max( kHoldMinLength, hold.length - MotionAmount( m, effect.reel, effect.reelExpr, values, 0.0f, 100.0f ) * dt );
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
				// (A rope that does not react moves the player alone: what it holds weighs as the world.)
				float theirs = target.mass > 0.0f && effect.react ? 1.0f / target.mass : 0.0f;
				float impulse = ( need - approach ) / ( mine + theirs );
				PushEnd( self, c, b3MulSV( impulse * mine, toward ) );
				if ( theirs > 0.0f )
				{
					PushEnd( target, c, b3MulSV( -impulse * theirs, toward ) );
				}
			}
		}
		};
		runEffects( m.steps[slot.step].effects, on.started );
		// Its step's walls and finds, when the step starts; its repeat's, each time it happens.
		auto runOnce = [&]( const MotionStep& step ) {
			RunBreaks( m, step, slot, t.position, values );
			for ( const MotionFind& find : step.finds )
			{
				if ( MotionGuardHolds( slot, find.guard ) )
				{
					RunFind( e, c, t, in, m, find, self, hit, values, 0 );
				}
			}
		};
		if ( on.started )
		{
			runOnce( m.steps[slot.step] );
		}
		const MotionStep& current = m.steps[slot.step];
		if ( MotionRepeatsNow( current, MotionStepStart( slot ), m_globals.tick, m_config.tickRate ) )
		{
			runEffects( current.repeats.front().effects, true );
			runOnce( current.repeats.front() );
		}
		if ( holdChanged )
		{
			e.set<MotionHold>( hold );
		}
	}
}

bool Simulation::EntityHold( uint32_t netId, b3Vec3& end, bool& holds, uint16_t& motion, bool* missed ) const
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
	holds = m_globals.tick >= hold->holdTick && hold->missed == 0;
	if ( missed != nullptr )
	{
		*missed = hold->missed != 0;
	}
	end = point;
	if ( holds == false && hold->holdTick > hold->startTick )
	{
		// Flying: its tip, on its way from where it was thrown toward the end of its range.
		float along = std::min( float( m_globals.tick - hold->startTick ) / float( hold->holdTick - hold->startTick ), 1.0f );
		end = b3Add( hold->origin, b3MulSV( along, b3Sub( point, hold->origin ) ) );
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

uint64_t Simulation::EntityMotionsOn( uint32_t netId ) const
{
	flecs::entity e = FindEntity( netId );
	// (The last tick simulated: the state is the one after it.)
	return e.is_valid() && m_motions && HasMotionState( e ) && m_globals.tick > 0 ? MotionsOnAt( GetMotionState( e ), m_globals.tick - 1 ) : 0;
}

AnimStateCopy Simulation::EntityAnimState( uint32_t netId ) const
{
	flecs::entity e = FindEntity( netId );
	return e.is_valid() && HasAnimState( e ) ? AnimStateCopy( GetAnimState( e ) ) : AnimStateCopy();
}

int32_t Simulation::BoardValue( uint32_t netId, int slot ) const
{
	flecs::entity e = FindEntity( netId );
	if ( slot < 0 || slot >= int( m_config.fields ) || e.is_valid() == false || HasBoard( e ) == false )
	{
		return 0;
	}
	return static_cast<const int32_t*>( ecs_get_id( m_world.c_ptr(), e.id(), m_boardId ) )[slot];
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
		const flecs::entity who = ctx->sim->FindEntity( netId );
		if ( who.has<Character>() && who.has<DrivenProp>() == false )
		{
			return -1.0f; // (a player's or an NPC's: hit by its hitboxes)
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

float Simulation::StaticFreeDistance( b3Vec3 origin, b3Vec3 direction, float maxDistance, float radius )
{
	PhysicsArena::Scope scope( *m_arena );
	const b3Vec3 centre = { 0.0f, 0.0f, 0.0f };
	const b3ShapeProxy ball = { &centre, 1, radius };
	b3QueryFilter filter = { ~uint64_t( 0 ), CatStatic, 0, nullptr };
	float closest = 1.0f;
	auto nearest = []( b3ShapeId, b3Pos, b3Vec3, float fraction, uint64_t, int, int, void* context ) -> float {
		float& best = *static_cast<float*>( context );
		best = std::min( best, fraction );
		return fraction; // (only nearer ones from here on)
	};
	b3World_CastShape( m_physicsWorld, origin, &ball, b3MulSV( maxDistance, direction ), filter, nearest, &closest );
	return closest * maxDistance;
}

namespace
{

// The turn from forward (+Z) to `direction` (unit length): about Y to its heading, then about X to its
// pitch, so what is level stays level. Half angles by square roots, the same on every machine.
b3Quat TurnTo( b3Vec3 direction )
{
	auto half = []( float cosine, float sine, b3Vec3 axis ) {
		float c = std::sqrt( std::max( 0.0f, ( 1.0f + cosine ) * 0.5f ) );
		float s = std::sqrt( std::max( 0.0f, ( 1.0f - cosine ) * 0.5f ) ) * ( sine < 0.0f ? -1.0f : 1.0f );
		return b3Quat{ b3MulSV( s, axis ), c };
	};
	float flat = std::sqrt( direction.x * direction.x + direction.z * direction.z );
	b3Quat yaw = flat > 1.0e-6f ? half( direction.z / flat, direction.x / flat, b3Vec3{ 0.0f, 1.0f, 0.0f } ) : b3Quat_identity;
	// (Up is a turn of -pitch about X: +Z goes to +Y.)
	b3Quat pitch = half( flat, -direction.y, b3Vec3{ 1.0f, 0.0f, 0.0f } );
	return b3MulQuat( yaw, pitch );
}

} // namespace

bool Simulation::CastProbe( const Motion& m, b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit )
{
	if ( m.probePoints.empty() )
	{
		return CastRay( origin, translation, ignoreNetId, hit );
	}
	float length = 0.0f;
	b3Vec3 direction = b3GetLengthAndNormalize( &length, translation );
	b3Quat turn = length > 0.0f ? TurnTo( direction ) : b3Quat_identity;
	b3Vec3 points[128];
	int count = int( std::min<size_t>( m.probePoints.size(), 128 ) );
	for ( int i = 0; i < count; ++i )
	{
		points[i] = b3RotateVector( turn, m.probePoints[size_t( i )] );
	}
	const b3ShapeProxy proxy = { points, count, m.probeRadius };
	PhysicsArena::Scope scope( *m_arena );
	BuildShapeLookup();
	hit = RayHit{};
	RayContext ctx{ this, ignoreNetId, &hit, false, false };
	b3QueryFilter filter = { ~uint64_t( 0 ), ~uint64_t( CatPusher ), 0, nullptr };
	b3World_CastShape( m_physicsWorld, origin, &proxy, translation, filter, &Simulation::RayCallback, &ctx );
	return ctx.found;
}

bool Simulation::CastRay( b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit, bool skipPlayers )
{
	PhysicsArena::Scope scope( *m_arena );
	BuildShapeLookup();
	hit = RayHit{};
	RayContext ctx{ this, ignoreNetId, &hit, false, skipPlayers };
	b3QueryFilter filter = { ~uint64_t( 0 ), ~uint64_t( CatPusher ), 0, nullptr };
	b3World_CastRay( m_physicsWorld, origin, translation, filter, &Simulation::RayCallback, &ctx );
	return ctx.found;
}

void Simulation::HoldBackProp( flecs::entity e, const DrivenProp& prop, b3Vec3 start, float startYaw, Transform& t, Character& c )
{
	// The mover has the ground, steps and slopes; the prop's upright bounds come along and keep out
	// of the world, other players and other driven props (a big box does not fit a small doorway, a
	// corner does not swing into a wall). Raised a little off the ground so the floor it stands on
	// is not in the way.
	const uint32_t self = e.get<NetId>().value;
	const float lift = std::min( 0.15f, 0.25f * ( prop.high[1] - prop.low[1] ) );
	// The gap it keeps from what it meets: past Box3D's speculative distance (2 cm), within which a
	// cast starting there would say it touches already, with no direction to slide along.
	constexpr float kSkin = 0.03f;
	b3Vec3 points[8];
	// Its bounds turned to `yaw`, shrunk by `inset` all round but at the floor (an inset that makes
	// them clear says how far in they are).
	auto shapeAt = [&]( float yaw, float inset ) {
		const b3Quat turn = detmath::YawRotation( yaw );
		for ( int i = 0; i < 8; ++i )
		{
			const b3Vec3 corner = { ( i & 1 ) ? prop.high[0] - inset : prop.low[0] + inset, ( i & 2 ) ? prop.high[1] - inset : prop.low[1] + lift,
									( i & 4 ) ? prop.high[2] - inset : prop.low[2] + inset };
			points[i] = b3RotateVector( turn, corner );
		}
		return b3ShapeProxy{ points, 8, 0.0f };
	};
	// (Anything that does not move by itself: the level, a static template's body (a wall's standing
	// pieces), rubble, players, other driven props. A loose body is shoved by the capsule instead.)
	const b3QueryFilter filter = { CatDriven, CatStatic | CatProp | CatPlayer | CatDriven | CatDebris, 0, nullptr };
	PhysicsArena::Scope scope( *m_arena );
	BuildShapeLookup();
	struct Overlap
	{
		const Simulation* sim;
		uint32_t self;
		bool found;
	};
	auto overlaps = [&]( b3Vec3 origin, float yaw, float inset ) {
		const b3ShapeProxy proxy = shapeAt( yaw, inset );
		Overlap o{ this, self, false };
		b3World_OverlapShape( m_physicsWorld, origin, &proxy, filter,
							  []( b3ShapeId shape, void* context ) {
								  auto* o = static_cast<Overlap*>( context );
								  if ( o->sim->NetIdOfShape( shape ) == o->self || b3Body_GetType( b3Shape_GetBody( shape ) ) == b3_dynamicBody )
								  {
									  return true; // (its own shapes; a loose body)
								  }
								  o->found = true;
								  return false;
							  },
							  &o );
		return o.found;
	};
	// How far into something its bounds are, roughly (0: clear): the least inset that clears them.
	const float across0 = 0.5f * std::min( prop.high[0] - prop.low[0], prop.high[2] - prop.low[2] );
	auto depth = [&]( b3Vec3 origin, float yaw ) {
		if ( overlaps( origin, yaw, 0.0f ) == false )
		{
			return 0.0f;
		}
		for ( float inset = 0.01f; inset < across0; inset *= 2.0f )
		{
			if ( overlaps( origin, yaw, inset ) == false )
			{
				return inset;
			}
		}
		return across0;
	};
	auto bodyAt = [&]( b3Vec3 centre ) { return b3Vec3{ centre.x, centre.y - prop.drop, centre.z }; };
	const b3Vec3 from = bodyAt( start );
	const float inside = depth( from, startYaw );

	// The turn: only where it does not put the bounds further into something.
	float yaw = c.facingYaw;
	if ( yaw != startYaw && depth( from, yaw ) > inside )
	{
		yaw = startYaw;
		c.facingYaw = startYaw;
		t.rotation = detmath::YawRotation( startYaw );
	}

	const b3Vec3 across = { t.position.x - start.x, 0.0f, t.position.z - start.z };
	if ( b3Length( across ) > 1e-5f )
	{
		if ( inside > 0.0f )
		{
			// Already in something (taken over against a wall, a corner turned in): any way that is
			// not further in, so it always gets out.
			if ( depth( bodyAt( t.position ), yaw ) > inside )
			{
				t.position.x = start.x;
				t.position.z = start.z;
			}
		}
		else
		{
			// Up to what is in the way, then along it with what is left (once).
			b3Vec3 at = from;
			b3Vec3 left = across;
			for ( int pass = 0; pass < 2; ++pass )
			{
				const float length = b3Length( left );
				if ( length < 1e-5f )
				{
					break;
				}
				const b3ShapeProxy proxy = shapeAt( yaw, 0.0f );
				struct Cast
				{
					const Simulation* sim;
					uint32_t self;
					bool found;
					float fraction;
					b3Vec3 normal;
				} cast{ this, self, false, 1.0f, {} };
				b3World_CastShape( m_physicsWorld, at, &proxy, left, filter,
								   []( b3ShapeId shape, b3Pos, b3Vec3 normal, float fraction, uint64_t, int, int, void* context ) -> float {
									   auto* cast = static_cast<Cast*>( context );
									   if ( cast->sim->NetIdOfShape( shape ) == cast->self || b3Body_GetType( b3Shape_GetBody( shape ) ) == b3_dynamicBody )
									   {
										   return -1.0f; // (its own shapes; a loose body)
									   }
									   if ( fraction < cast->fraction || cast->found == false )
									   {
										   cast->found = true;
										   cast->fraction = fraction;
										   cast->normal = normal;
									   }
									   return fraction;
								   },
								   &cast );
				if ( cast.found == false )
				{
					at = b3Add( at, left );
					break;
				}
				float flat = 0.0f;
				const b3Vec3 normal = b3GetLengthAndNormalize( &flat, b3Vec3{ cast.normal.x, 0.0f, cast.normal.z } );
				if ( flat < 1e-3f )
				{
					// Touching from the start, with no direction to say: the move is taken if it does not
					// put it further in.
					if ( depth( b3Add( at, left ), yaw ) <= depth( at, yaw ) )
					{
						at = b3Add( at, left );
					}
					break;
				}
				// To the contact, then the skin back off it, along its normal (not along the way it came:
				// at a slant that would leave next to no gap).
				at = b3MulAdd( at, cast.fraction, left );
				at = b3MulAdd( at, kSkin, normal );
				b3Vec3 rest = b3MulSV( 1.0f - cast.fraction, left );
				rest = b3MulSub( rest, b3Dot( rest, normal ), normal );
				left = rest;
				// Its speed into what stopped it goes.
				const float into = b3Dot( c.velocity, normal );
				if ( into < 0.0f )
				{
					c.velocity = b3MulSub( c.velocity, into, normal );
				}
			}
			t.position.x = at.x;
			t.position.z = at.z;
		}
	}
	b3Body_SetTargetTransform( BodyOf( e.get<PhysicsBody>() ), { bodyAt( t.position ), t.rotation }, m_config.TimeStep(), true );
}

} // namespace cb
