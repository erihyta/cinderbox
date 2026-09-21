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
	uint32_t reserved = 0;
	ImpactRecord impacts[kImpactHistory] = {};
};

static_assert( sizeof( ImpactRecord ) == 28, "ImpactRecord layout changed: check for padding" );
static_assert( sizeof( SimGlobals ) == 16 + 4 * kMaxPlayers + 8 + kImpactHistory * sizeof( ImpactRecord ),
			   "SimGlobals has padding: it is hashed as raw bytes" );

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
	b3Vec3 SpawnPoint( PlayerSlot slot ) const;
	b3ShapeId CreateShape( b3BodyId body, const Shape& shape, uint64_t category, const ShapeMaterial& material = {} );
	static PhysicsBody MakePhysicsBody( b3BodyId body, b3ShapeId shape );

	void ApplyEvents( const InputFrame& frame );
	void MoveCharacters( const InputFrame& frame );
	void MoveCharacter( Character& c, Transform& t, const PhysicsBody& pb, const PlayerInput& in, uint8_t pressed );
	void SpawnProps();
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

	// Per-step scratch, never part of the state.
	std::vector<uint32_t> m_spawnRequests;
	std::vector<EntityRef> m_scratch;
	std::vector<uint8_t> m_hashScratch;
	// Shape index -> NetId, rebuilt only on ticks that produced impacts.
	std::vector<std::pair<uint32_t, uint32_t>> m_shapeLookup;
	std::vector<ImpactRecord> m_impactScratch;
};

} // namespace cb
