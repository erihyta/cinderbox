#pragma once

// The deterministic game simulation. Server and clients run exactly this code.
//
// Determinism contract:
//   Same SimConfig + same sequence of InputFrames => bit-identical state on every platform.
// All state lives in (a) the flecs sim world, (b) SimGlobals and (c) the Box3D arena, and all
// three are captured by Save()/Load().

#include "components.h"
#include "events.h"
#include "level.h"
#include "mod_schema.h"
#include "motions.h"
#include "physics_arena.h"
#include "types.h"
#include "world_lifetime.h"

#include "box3d/id.h"
#include "flecs.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <mutex>
#include <vector>

namespace cb
{

struct AnimGraph;

// Surface values Box3D needs when a shape is created. Authored through the Material component.
struct ShapeMaterial
{
	float density = 40.0f; // kg/m^3: a crate 0.8 m across is 20 kg
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

// Singleton sim state that is not a component.
// A player's animation state, copied out of a simulation (where it is a packed block): used like
// a pointer that may be null.
class AnimStateCopy
{
public:
	AnimStateCopy() = default;
	explicit AnimStateCopy( const AnimState& state )
		: m_has( true )
		, m_state( state )
	{
	}
	const AnimState* operator->() const
	{
		return &m_state;
	}
	const AnimState& operator*() const
	{
		return m_state;
	}
	explicit operator bool() const
	{
		return m_has;
	}
	bool operator==( std::nullptr_t ) const
	{
		return m_has == false;
	}
	const AnimState* get() const
	{
		return m_has ? &m_state : nullptr;
	}

private:
	bool m_has = false;
	AnimState m_state;
};

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
};

static_assert( sizeof( SimGlobals ) == 16 + 4 * kMaxPlayers + 8 + kImpactHistory * sizeof( ImpactRecord ) +
											 kModEventHistory * sizeof( ModEventRecord ),
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
	// How the player in `slot` moves (move_params.h): the server's parameters, with what mods set for
	// that player. The server's alone when the slot is empty.
	MoveParams PlayerMove( PlayerSlot slot ) const;
	// What a probe of the entity's motions holds on to, if one is out: where the line's end is now
	// (flying toward where it will hold, or holding), whether it holds, and the motion that threw
	// it. False when it has none.
	bool EntityHold( uint32_t netId, b3Vec3& end, bool& holds, uint8_t& motion ) const;
	const Transform* EntityTransform( uint32_t netId ) const;
	// A player's animation state, or null.
	AnimStateCopy EntityAnimState( uint32_t netId ) const;
	bool HasAnimState( flecs::entity e ) const;
	AnimState GetAnimState( flecs::entity e ) const;
	// The entity's board value, or 0 when it has none.
	int32_t BoardValue( uint32_t netId, int slot ) const;
	int32_t GlobalBoardValue( int slot ) const
	{
		return slot >= 0 ? m_globalBoard[size_t( slot )] : 0;
	}
	// The global board: values a mod publishes about the whole game (a round timer, a score). As
	// many as the config says (SimConfig::globalFields).
	const BoardValues& GlobalBoard() const
	{
		return m_globalBoard;
	}
	// An entity's board: all 0 when it has none.
	bool HasBoard( flecs::entity e ) const;
	Blackboard GetBoard( flecs::entity e ) const;
	// A player's motions: where each is (all never used when it has none).
	bool HasMotionState( flecs::entity e ) const;
	MotionState GetMotionState( flecs::entity e ) const;
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

	// The item `holder` (a player's NetId) holds in `socket`, or 0.
	// The item `holder` has in use in `socket` (stowed items are in nobody's hand), or 0.
	uint32_t HeldItemOf( uint32_t holder, uint32_t socket ) const;

	// The character's state machine (sim/anim_graph.h), compiled against the server's schema.
	// Like the map it must be the same everywhere, so it travels in the schema; set it before the
	// first Step. Without one, players' animation states carry how they move and nothing plays.
	void SetAnimGraph( std::shared_ptr<const AnimGraph> graph )
	{
		m_animGraph = std::move( graph );
	}
	const AnimGraph* Graph() const
	{
		return m_animGraph.get();
	}
	// The mods' animation packs (compiled from the schema), whose layers players can swap to.
	// The bodies of items lying in the world, by kind (ModSchema::itemShapes); set before the first
	// Step, like the graph.
	void SetItemShapes( std::vector<ItemShape> shapes )
	{
		m_itemShapes = std::move( shapes );
	}
	// Slots: which one a player has selected (kNoSlot: none, or no slots), what is in one.
	uint8_t SelectedSlot( uint32_t player ) const;
	uint32_t SlotItem( uint32_t player, uint8_t slot ) const
	{
		return SlotItemOf( player, slot );
	}
	ItemShape ItemShapeOf( uint16_t kind ) const
	{
		return kind < m_itemShapes.size() ? m_itemShapes[kind] : ItemShape{};
	}
	void SetAnimPacks( std::vector<std::shared_ptr<const AnimGraph>> packs )
	{
		m_animPacks = std::move( packs );
	}
	// The motions the server's mods provide (sim/motions.h), compiled from the schema: run for every
	// player, every tick, before the mover. Null: none.
	// (More of them than the config has slots for are not run at all: the config was not made for
	// this schema.)
	void SetMotions( std::shared_ptr<const Motions> motions );
	const std::vector<std::shared_ptr<const AnimGraph>>& Packs() const
	{
		return m_animPacks;
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
	// Slots (types.h): each player's intent of this tick, before anything asks what it holds.
	void StepSlots( const InputFrame& frame );
	// The item a player has in a slot (0: none).
	uint32_t SlotItemOf( uint32_t holder, uint8_t slot ) const;
	// A new arrival finds its slot (its kind's own, pushing out what is there; else the first free
	// one). False when there is none: the player cannot carry it.
	bool GiveSlot( flecs::entity player, flecs::entity item, bool select );
	// The selected slot's item is in the hand; every other slotted item is stowed in its holster.
	void SettleSlots( uint32_t holder, uint8_t selected );
	// Out of the bag: thrown a little, from the chest, the way the player looks.
	void ThrowOut( flecs::entity item, const Transform& from, uint16_t cameraYaw );
	// The parameters the mover uses for this player: the server's, with what mods set for it.
	MoveParams MoveOf( flecs::entity e ) const;
	// Throws `motion`'s probe for the player `e` along its look: false when it finds nothing.
	bool AttachHold( flecs::entity e, const Transform& t, const PlayerInput& in, const Motion& motion, size_t index, uint32_t& holdTick );
	// Where a hold is, in the world; false when what it held on to is gone.
	bool HoldPoint( const MotionHold& hold, b3Vec3& point ) const;
	// The effects of the motions that are on for the player `e` this tick: impulses, forces and
	// links, on the player (`c`, not yet stored) and on their targets.
	struct MotionEnd;
	MotionEnd ResolveEnd( flecs::entity self, const Transform& t, const MotionTarget& target, const MotionHold* hold,
						  const Blackboard& board ) const;
	b3Vec3 EndVelocity( const MotionEnd& end, const Character& c ) const;
	void PushEnd( const MotionEnd& end, Character& c, b3Vec3 change );
	void ApplyMotionEffects( flecs::entity e, Character& c, const Transform& t, const PlayerInput& in, const MotionState& state,
							 const Blackboard& board, const std::vector<MotionActive>& active );
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
	// State whose size the server's mods decide is kept in components that get their size when
	// the world is made (a block of bytes each; 0 bytes: no entity ever has it).
	flecs::entity_t RegisterSizedComponent( const char* name, uint32_t size );
	void SetBoard( flecs::entity e, const Blackboard& board );
	flecs::entity_t m_boardId = 0;
	void SetMotionState( flecs::entity e, const MotionState& state );
	flecs::entity_t m_motionId = 0;
	void SetAnimState( flecs::entity e, const AnimState& state );
	flecs::entity_t m_animId = 0;
	BoardValues m_globalBoard;
	std::vector<EntityRef> m_entities; // sorted by netId

	// Rebuilds m_shapeLookup (shape index -> NetId) for every shape in the world.
	void BuildShapeLookup();
	uint32_t NetIdOfShape( b3ShapeId shape ) const;
	static float RayCallback( b3ShapeId shapeId, b3Pos point, b3Vec3 normal, float fraction, uint64_t material, int triangle,
							  int child, void* context );

	// Per-step scratch, never part of the state.
	std::vector<EntityRef> m_scratch;
	std::vector<uint8_t> m_hashScratch;
	std::vector<uint8_t> m_sizedScratch;
	// Shape index -> NetId, rebuilt only when a shape has to be named (impacts, ray casts).
	std::vector<std::pair<uint32_t, uint32_t>> m_shapeLookup;
	std::vector<ImpactRecord> m_impactScratch;
	std::vector<int> m_markerScratch;
	std::vector<std::pair<uint32_t, uint16_t>> m_heldScratch; // holder NetId, item kind

	std::shared_ptr<const AnimGraph> m_animGraph;
	std::vector<std::shared_ptr<const AnimGraph>> m_animPacks;
	std::vector<ItemShape> m_itemShapes;
	std::shared_ptr<const Motions> m_motions;
	std::vector<ModEventRecord> m_motionEvents;
	std::vector<MotionActive> m_motionActive;
	// An item leaves the hand and lies in the world at its grip (a body of its kind's shape), or
	// the other way round.
	void PutItemInWorld( flecs::entity item, b3Vec3 grip, b3Quat rotation, b3Vec3 velocity );
	void TakeItemFromWorld( flecs::entity item, uint32_t holder, uint8_t socket );
	void RecordModEvent( const ModEventRecord& record );
	void FollowHolders();
};

} // namespace cb
