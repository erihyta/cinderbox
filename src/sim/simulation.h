#pragma once

// The deterministic game simulation. Server and clients run exactly this code.
//
// Determinism contract:
//   Same SimConfig + same sequence of InputFrames => bit-identical state on every platform.
// All state lives in (a) the flecs sim world, (b) SimGlobals and (c) the Box3D arena, and all
// three are captured by Save()/Load().

#include "components.h"
#include "level.h"
#include "physics_arena.h"
#include "types.h"

#include "box3d/id.h"
#include "flecs.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <mutex>
#include <vector>

namespace cb
{

// flecs (OS API init counter) and Box3D (static world table) are not safe to create or destroy
// worlds on several threads at once. Everything that creates flecs or Box3D worlds while
// simulations may run on other threads goes through these.
std::mutex& WorldLifetimeMutex();
flecs::world CreateFlecsWorld();
void ReleaseFlecsWorld( flecs::world& world );

// Surface values Box3D needs when a shape is created. Authored through the Material component.
struct ShapeMaterial
{
	float density = 1.0f;
	float friction = 0.6f;
	float restitution = 0.0f;
};

struct Snapshot
{
	uint32_t tick = 0;
	uint64_t hash = 0;
	std::vector<uint8_t> ecs;	  // canonical ECS + globals image (also what gets hashed)
	std::vector<uint8_t> physics; // arena image followed by the b3World struct
};

// One collision hard enough to be worth showing. Both entities are named so presentation can pick
// an effect by what was hit; a static is an entity too, so its NetId appears here like any other.
struct ImpactRecord
{
	uint32_t netIdA = 0;
	uint32_t netIdB = 0;
	uint32_t tick = 0;
	float speed = 0.0f; // approach speed along the contact normal, m/s
	b3Vec3 point = {};
};

// How many impacts presentation can pick up at once. A renderer that falls far behind drops the
// rest, which is the right trade for an effect.
inline constexpr uint32_t kImpactHistory = 16;
inline constexpr uint32_t kImpactsPerTick = 8;

// Something a server mod announced (an Event command): "pistol fired", "player killed". The type
// indexes the event names the server sends on join; the simulation only keeps the record, so
// presentation can play it and a rollback can un-count it.
struct ModEventRecord
{
	uint16_t type = 0;
	uint16_t reserved = 0;
	uint32_t netIdA = 0;
	uint32_t netIdB = 0;
	uint32_t tick = 0;
	int32_t value = 0;
	b3Vec3 point = {};
	b3Vec3 vector = {};
};

inline constexpr uint32_t kModEventHistory = 32;

// Singleton sim state that is not a component.
// Hashed as raw bytes, so it must stay free of padding.
struct SimGlobals
{
	uint32_t tick = 0;
	uint32_t nextNetId = 1;
	uint64_t rngState = 0;
	uint32_t playerNetIds[kMaxPlayers] = {}; // 0 = slot empty
	// Only ever grows, so presentation can tell how many impacts it missed. The ring holds the
	// most recent ones, newest at (impactCount - 1) % kImpactHistory.
	uint32_t impactCount = 0;
	// Same shape for mod events.
	uint32_t modEventCount = 0;
	ImpactRecord impacts[kImpactHistory] = {};
	ModEventRecord modEvents[kModEventHistory] = {};
	// The global blackboard: values a mod publishes about the whole game (a round timer, a score).
	int32_t board[kBoardSlots] = {};
};

static_assert( sizeof( ImpactRecord ) == 28, "ImpactRecord layout changed: check for padding" );
static_assert( sizeof( ModEventRecord ) == 44, "ModEventRecord layout changed: check for padding" );
static_assert( sizeof( SimGlobals ) == 16 + 4 * kMaxPlayers + 8 + kImpactHistory * sizeof( ImpactRecord ) +
											 kModEventHistory * sizeof( ModEventRecord ) + 4 * kBoardSlots,
			   "SimGlobals has padding: it is hashed as raw bytes" );

// What a ray hit, for server mods (hitscan weapons, line of sight).
struct RayHit
{
	uint32_t netId = 0; // 0: the level has no entity for it (never happens today)
	b3Vec3 point = {};
	b3Vec3 normal = {};
	float fraction = 1.0f;
	// The hitbox zone ("head", "torso") when the server's hit test found a player; null otherwise.
	const char* zone = nullptr;
};

class Simulation
{
public:
	// `map` is the level to build. It must be identical on the server and every client; the server
	// sends the bytes it loaded to each client on join.
	explicit Simulation( const SimConfig& config, const LevelLayout& map = GetLevelLayout() );
	~Simulation();

	Simulation( const Simulation& ) = delete;
	Simulation& operator=( const Simulation& ) = delete;

	// Advance one tick. frame.tick must equal Tick().
	void Step( const InputFrame& frame );

	// The tick that the next Step() will simulate.
	uint32_t Tick() const
	{
		return m_globals.tick;
	}

	// Fast in-process snapshot for rollback. Only valid for this Simulation instance.
	void Save( Snapshot& out );
	void Load( const Snapshot& snapshot );

	// Self-contained state for another process (late join, desync recovery). Slower.
	void SavePortable( std::vector<uint8_t>& out );
	bool LoadPortable( const std::vector<uint8_t>& in );

	// Box3D handles for a PhysicsBody component.
	b3BodyId BodyOf( const PhysicsBody& pb ) const;
	b3ShapeId ShapeOf( const PhysicsBody& pb ) const;
	b3WorldId PhysicsWorld() const
	{
		return m_physicsWorld;
	}

	// Hash of the canonical ECS image. Every physics body's pose and velocity is mirrored into
	// components each tick, so this covers the physics state too.
	uint64_t ComputeHash();

	const SimConfig& Config() const
	{
		return m_config;
	}
	const SimGlobals& Globals() const
	{
		return m_globals;
	}
	flecs::world& World()
	{
		return m_world;
	}

	bool IsPlayerActive( PlayerSlot slot ) const
	{
		return m_globals.playerNetIds[slot] != 0;
	}

	// --- Read-only queries (server mods, tools) ------------------------------------------------
	// None of these change the state, and none are used by Step().

	// 0 if the slot is empty.
	uint32_t PlayerNetId( PlayerSlot slot ) const
	{
		return m_globals.playerNetIds[slot];
	}
	// Null if the slot is empty.
	const Character* PlayerCharacter( PlayerSlot slot ) const;
	const Transform* EntityTransform( uint32_t netId ) const;
	// A player's animation state, or null.
	const AnimState* EntityAnimState( uint32_t netId ) const;
	// The entity's board value, or 0 when it has none.
	int32_t BoardValue( uint32_t netId, int slot ) const;
	int32_t GlobalBoardValue( int slot ) const
	{
		return slot >= 0 && slot < kBoardSlots ? m_globals.board[slot] : 0;
	}
	// The closest thing a ray from `origin` along `translation` hits, skipping entity `ignoreNetId`
	// and disabled bodies (and every player's capsule with `skipPlayers`). Returns false when it hits
	// nothing.
	bool CastRay( b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit, bool skipPlayers = false );
	// The level this simulation was built from (templates, spawn template).
	const LevelLayout& Map() const
	{
		return m_map;
	}
	// Where a player in `slot` appears when joining or respawning.
	b3Vec3 SpawnPoint( PlayerSlot slot ) const;

	// 0 if not found. Lookup only; never iterate this for simulation order.
	flecs::entity FindEntity( uint32_t netId ) const;

	// Entities in canonical (NetId) order.
	struct EntityRef
	{
		uint32_t netId;
		flecs::entity_t entity;
	};
	const std::vector<EntityRef>& Entities() const
	{
		return m_entities;
	}

	size_t PhysicsBytesInUse() const
	{
		return m_arena->UsedBytes();
	}

private:
	struct SnapComponent
	{
		flecs::entity_t id;
		uint32_t size;
	};

	void RegisterComponents();
	template <typename T>
	void RegisterSnapComponent();

	void BuildLevel();
	flecs::entity CreateEntity();
	void DestroyEntity( flecs::entity e );

	flecs::entity CreateProp( ShapeKind kind, b3Vec3 position, b3Quat rotation, b3Vec3 halfExtents, b3Vec3 velocity,
							  uint32_t owner, uint32_t lifetimeTicks );
	// Builds an entity from a map template: shape, body, material and initial velocity all come
	// from the authored values, with the engine's defaults for anything the author left alone.
	flecs::entity CreateFromTemplate( uint32_t templateIndex, b3Vec3 position, b3Quat rotation, b3Vec3 extraVelocity,
									  uint32_t owner );
	flecs::entity CreatePlayer( PlayerSlot slot );
	void PlaceCharacter( flecs::entity e, b3Vec3 position, float yaw );
	b3ShapeId CreateShape( b3BodyId body, const Shape& shape, uint64_t category, const ShapeMaterial& material = {} );
	static PhysicsBody MakePhysicsBody( b3BodyId body, b3ShapeId shape );

	void ApplyEvents( const InputFrame& frame );
	void ApplyCommands( const InputFrame& frame );
	void ApplyCommand( const SimCommand& command );
	uint32_t ResolveTarget( uint32_t target ) const;
	void ApplyImpulse( flecs::entity e, const SimCommand& command );
	void KillPlayer( flecs::entity e, const SimCommand& command );
	void RespawnPlayer( flecs::entity e, const SimCommand& command );
	flecs::entity CreateRagdoll( flecs::entity player, uint32_t lifetimeTicks );
	void EnforceRagdollCap( uint32_t cap );
	void MoveCharacters( const InputFrame& frame );
	void MoveCharacter( Character& c, Transform& t, const PhysicsBody& pb, const PlayerInput& in, uint8_t pressed );
	void ExpireProps();
	void EnforcePropCaps();
	void SyncFromPhysics();
	// Reads Box3D's contact hit events into the impact ring, strongest first.
	void CollectImpacts();
	// Advances each character's stride and counts a step whenever it completes one.
	void UpdateFootsteps();
	void HandleOutOfBounds();

	void SerializeEcs( std::vector<uint8_t>& out ) const;
	void DeserializeEcs( const std::vector<uint8_t>& in );

	SimConfig m_config;
	LevelLayout m_map;
	SimGlobals m_globals;
	flecs::world m_world;
	std::unique_ptr<PhysicsArena> m_arena;
	b3WorldId m_physicsWorld = {};

	std::vector<SnapComponent> m_snapComponents;
	std::vector<EntityRef> m_entities; // sorted by netId

	// Rebuilds m_shapeLookup (shape index -> NetId) for every shape in the world.
	void BuildShapeLookup();
	uint32_t NetIdOfShape( b3ShapeId shape ) const;
	static float RayCallback( b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t material, int triangle,
							  int child, void* context );

	// Per-step scratch, never part of the state.
	std::vector<EntityRef> m_scratch;
	std::vector<uint8_t> m_hashScratch;
	// Shape index -> NetId, rebuilt only when a shape has to be named (impacts, ray casts).
	std::vector<std::pair<uint32_t, uint32_t>> m_shapeLookup;
	std::vector<ImpactRecord> m_impactScratch;
};

} // namespace cb
