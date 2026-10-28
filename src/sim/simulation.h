#pragma once

// The deterministic game simulation. Server and clients run exactly this code.
//
// Determinism contract:
//   Same SimConfig + same sequence of InputFrames => bit-identical state on every platform.
// All state lives in (a) the flecs sim world, (b) SimGlobals and (c) the Box3D arena, and all
// three are captured by Save()/Load().

#include "bytes.h"
#include "components.h"
#include "events.h"
#include "level.h"
#include "mod_schema.h"
#include "motions.h"
#include "physics_arena.h"
#include "physics_threads.h"
#include "types.h"
#include "world_lifetime.h"

#include "box3d/id.h"
#include "flecs.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <mutex>
#include <string>
#include <unordered_map>
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
	// Rolling resistance (balls and capsules: a ball on the ground slows by about `rolling` times
	// gravity), and drag: how fast its speed and spin fade in the air and everywhere, per second.
	float rolling = 0.0f;
	float drag = 0.0f;
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
	// NetIds come from two counters (M247): the physics world's entities' from 1, the characters'
	// (players, the items they make) from kCharacterIdBase. So a simulation without the characters
	// (following another's physics) still names every physics body as the server does.
	uint32_t nextNetId = 1;
	uint32_t nextCharacterId = kCharacterIdBase;
	uint32_t reserved = 0;
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

static_assert( sizeof( SimGlobals ) == 24 + 4 * kMaxPlayers + 8 + kImpactHistory * sizeof( ImpactRecord ) +
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

	// Where the steps' time went, in nanoseconds, since the simulation was made (a tool's figures:
	// nothing in the simulation reads them).
	struct StepProfile
	{
		uint64_t steps = 0;
		uint64_t commandsNs = 0;   // events and commands
		uint64_t charactersNs = 0; // the characters' movement, motions and animation
		uint64_t physicsNs = 0;	   // the physics step
		uint64_t afterNs = 0;	   // what is read back from the physics, and the rest
		uint64_t slotsNs = 0;	   // of the characters' part: the slots (selecting, moving, settling)
		// The physics step's own parts, in milliseconds (the physics library's figures).
		double pairsMs = 0.0;	   // finding new pairs of shapes that may touch
		double collideMs = 0.0;	   // working out the contacts
		double solveMs = 0.0;	   // the solver, all of it: the four below are inside it
		double constraintsMs = 0.0; // contacts and joints solved
		double transformsMs = 0.0; // bodies moved, their boxes kept
		double refitMs = 0.0;	   // the tree of boxes kept
		double sleepMs = 0.0;	   // islands put to sleep
		double sensorsMs = 0.0;
	};
	// What the physics world holds now.
	struct PhysicsCounts
	{
		int bodies = 0, awakeBodies = 0, shapes = 0, contacts = 0, awakeContacts = 0, joints = 0, islands = 0;
	};
	PhysicsCounts CountPhysics() const;
	const StepProfile& Profile() const
	{
		return m_profile;
	}

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

	// The physics world's inputs (M246; ROADMAP step 3; physics_inputs.cpp). Characters read the
	// physics world and never write it themselves: what they do to it goes in by its inputs, which
	// each step makes (PhysicsMade: the pushers' places) or, following another simulation's physics,
	// takes from its frame instead (InputFrame::pushers), whatever its own characters do.
	void SetFollowsPhysics( bool follows )
	{
		m_followsPhysics = follows;
	}
	bool FollowsPhysics() const
	{
		return m_followsPhysics;
	}
	// What the last step put into the physics world (its tick, its pushers' places, the physics
	// events it applied, each with what it made): what a follower is given for that tick.
	const InputFrame& PhysicsMade() const
	{
		return m_physicsMade;
	}
	// Hash of the physics world alone: every body that is not a character's, by its components
	// (not its physics handles: those are this world's own).
	uint64_t PhysicsHash() const;
	// Hash of the physics world as Box3D serializes it (M247): every body, shape, joint, contact and
	// its caches. Two simulations with it equal step on alike.
	uint64_t PhysicsWorldHash();
	// Those bytes (tests that say where two worlds differ).
	void PhysicsWorldBytes( std::vector<uint8_t>& out );
	// Which section of the bytes PhysicsWorldBytes last made a byte is in ("contacts, 1234 in"), and
	// the sizes of Box3D's structs as this build lays them out.
	static std::string PhysicsByteSection( size_t at );
	static std::string PhysicsStructSizes();

	// The state network model (ROADMAP step 3; stand_ins.cpp). No physics engine internals cross:
	// SaveState writes the entities and globals, and a description of every body read from the
	// physics engine (its type, its shapes). ApplyState makes this simulation's world the server's
	// as those bytes have it: every entity's components but its physics handles; a body kept when
	// the server's description of it is the one it was built from, built from the description
	// otherwise; what the server no longer has removed (nothing else happens: a chest's items do
	// not fall out). Every entity but `own`'s (the slot's body and what it carries) is then a
	// stand-in: nothing in Step() moves or drives it, its body is kinematic (a static one stays
	// static) and placed where the server has it, going on at its velocity until the next state.
	// From the first state on the world's rules are the server's: props' lifetimes and caps, glue,
	// breaking and items' events are not run here. False (and nothing changed) for bytes that are
	// not a state of this simulation's kind (its config, its components).
	// `compact`: what is sent to clients. Where things are and how they move rounded (positions to
	// 1/1024 m, rotations to 1/32768 and then made unit again, velocities to 1/256 m/s): their
	// changes then take fewer bits. This world's own state stays exact.
	void SaveState( std::vector<uint8_t>& out, bool compact = false );
	bool ApplyState( const uint8_t* data, size_t size, PlayerSlot own );
	// Whether ApplyState has run: the world's rules are the server's.
	bool FollowsServer() const
	{
		return m_followsServer;
	}
	bool IsStandIn( uint32_t netId ) const;
	// The components a state's record holds, in its mask's order: each one's name and size (tools
	// that say where a state's bytes go).
	std::vector<std::pair<std::string, uint32_t>> StateComponents() const;

	// Box3D handles for a PhysicsBody component.
	b3BodyId BodyOf( const PhysicsBody& pb ) const;
	b3ShapeId ShapeOf( const PhysicsBody& pb ) const;
	// The two Box3D worlds (M247): the physics world (the level, props, debris, items lying about,
	// ragdolls, pushers: a function of its inputs alone) and the characters' (their capsules, a
	// driven prop's body). A body is in one or the other; a stored handle says which (world0: 0 the
	// physics world, 1 the characters'), BodyOf and ShapeOf give the live one.
	b3WorldId PhysicsWorld() const
	{
		return m_physicsWorld;
	}
	b3WorldId CharacterWorld() const
	{
		return m_characterWorld;
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
		return slot < kMaxPlayers && m_globals.playerNetIds[slot] != 0;
	}

	// --- Read-only queries (server mods, tools) ------------------------------------------------
	// None of these change the state, and none are used by Step().

	// 0 if the slot is empty.
	uint32_t PlayerNetId( PlayerSlot slot ) const
	{
		return slot < kMaxPlayers ? m_globals.playerNetIds[slot] : 0; // (kNoSlot: a body no player drives)
	}
	// Null if the slot is empty.
	const Character* PlayerCharacter( PlayerSlot slot ) const;
	// How the player in `slot` moves (move_params.h): the server's parameters, with what mods set for
	// that player. The server's alone when the slot is empty.
	MoveParams PlayerMove( PlayerSlot slot ) const;
	// What a probe of the entity's motions holds on to, if one is out: where the line's end is now
	// (flying toward where it will hold, or holding), whether it holds, and the motion that threw
	// it. False when it has none.
	bool EntityHold( uint32_t netId, b3Vec3& end, bool& holds, uint16_t& motion, bool* missed = nullptr ) const;
	// Who threw it, for an item a motion launched (the player's NetId; 0: nobody, or it was
	// picked up since).
	uint32_t LaunchedBy( uint32_t netId ) const;
	const Transform* EntityTransform( uint32_t netId ) const;
	// A player's animation state, or null.
	AnimStateCopy EntityAnimState( uint32_t netId ) const;
	// A player's motions that are on, a bit each (the first 64 of the server's).
	uint64_t EntityMotionsOn( uint32_t netId ) const;
	// Every item a player carries, in the hand or put away, in NetId order.
	std::vector<uint32_t> CarriedItemsOf( uint32_t holder ) const;
	// A new copy of an item kind: its own fields as its kind says they start (ItemShape::fieldDefaults).
	void StartItemFields( flecs::entity item, uint16_t kind );
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
	// The motions of the player in `slot`, for the debug HUD: a line each that is on (its step, what
	// it waits for, how long, what its cast holds), and the last start refused in the last 3 s and
	// why. Empty without motions.
	std::string DescribeMotions( PlayerSlot slot ) const;
	// The closest thing a ray from `origin` along `translation` hits, skipping entity `ignoreNetId`
	// and disabled bodies (and every player's capsule with `skipPlayers`). Returns false when it hits
	// nothing.
	bool CastRay( b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit, bool skipPlayers = false );
	// The breakable walls near a point, nearest first: one entity each (a standing piece of it, or
	// the whole wall), to break with BreakWall. A wall is near when one of its pieces' centres is.
	std::vector<uint32_t> BreakablesNear( b3Vec3 point, float radius ) const;
	// A motion's probe from `origin` along `translation`: its shape swept, turned to the line (a line
	// without one: CastRay). What it meets first.
	bool CastProbe( const Motion& m, b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit );
	// How far a ball of `radius` travels from `origin` along `direction` (unit length) before the
	// static level stops it (every shape it has, meshes and terrain included), at most `maxDistance`.
	// A camera's question: presentation only, nothing in the simulation depends on it.
	float StaticFreeDistance( b3Vec3 origin, b3Vec3 direction, float maxDistance, float radius );
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

	// The characters a body may be (ModSchema::characters, in its order): each its state machine
	// (sim/anim_graph.h, compiled against the server's schema) and its capsule. Like the map they
	// must be the same everywhere, so they travel in the schema; set them before the first Step.
	// [0] is the server's own, every player's unless a SetCharacter says another. A character with
	// no state machine: its bodies' animation states carry how they move and nothing plays.
	struct CharacterBody
	{
		std::shared_ptr<const AnimGraph> graph;
		float radius = kDefaultCapsuleRadius;
		float halfHeight = kDefaultCapsuleHalfHeight;
		// Its movement values, laid over the server's (SimConfig::move) for its bodies.
		std::vector<std::pair<uint8_t, float>> move;
		// Whether its bodies may look out of their eyes (CharacterInfo::firstPerson): without, a
		// first-person view in the input is the third-person one (the body does not turn to face
		// the camera for it).
		bool firstPerson = true;
		// Whether its bodies fall as the ragdoll (CharacterInfo::ragdoll).
		bool ragdoll = true;
		// Which animation packs fit it (CharacterInfo::packs; empty: all).
		std::vector<uint8_t> packs;
	};
	void SetCharacters( std::vector<CharacterBody> characters );
	// Every character of a schema as the simulation runs it (its state machine compiled against the
	// schema, its capsule, its movement): what a client, a replay and a test hand SetCharacters.
	// False, with `error`, when a state machine does not compile.
	static bool CharactersOf( const ModSchema& schema, std::vector<CharacterBody>& out, std::string& error );
	// Character 0's state machine alone (its capsule the default): a server or a test with one.
	void SetAnimGraph( std::shared_ptr<const AnimGraph> graph )
	{
		m_characters.front().graph = std::move( graph );
	}
	// Character 0's state machine.
	const AnimGraph* Graph() const
	{
		return m_characters.front().graph.get();
	}
	// Which character a body is (0 for one with no CharacterKind, or an index past the list), and
	// that character.
	uint16_t CharacterOf( flecs::entity e ) const;
	const CharacterBody& Body( uint16_t character ) const
	{
		return m_characters[character < m_characters.size() ? character : 0];
	}
	size_t CharacterCount() const
	{
		return m_characters.size();
	}
	// The mods' animation packs (compiled from the schema), whose layers players can swap to.
	// The bodies of items lying in the world, by kind (ModSchema::itemShapes); set before the first
	// Step, like the graph.
	void SetItemShapes( std::vector<ItemShape> shapes )
	{
		m_itemShapes = std::move( shapes );
		m_anyBreaks = std::any_of( m_itemShapes.begin(), m_itemShapes.end(), []( const ItemShape& s ) { return s.breakSpeed > 0.0f; } );
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
	// What a player uses this tick by its input, as its slots are before the tick changes them:
	// the item in its hand by the use button, and one used by its kind's own key wherever it is
	// carried (ItemShape::use, useAction). `used`: on this tick's press; `inUse`: in use this tick.
	// The simulation's slots and the mods (Context::Used) both read it, so they agree.
	struct ItemUse
	{
		uint32_t item = 0;
		bool used = false;
		bool inUse = false;
	};
	void ItemUses( uint32_t player, const PlayerInput& in, uint8_t prevButtons, ActionBits prevActions, std::vector<ItemUse>& out ) const;
	// Whether this copy was in use on the last tick (a look's "using.<kind>", from the view frame).
	bool ItemInUse( uint32_t item ) const
	{
		return std::find( m_usingItems.begin(), m_usingItems.end(), item ) != m_usingItems.end();
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

	// The physics step's tasks go to the process's physics threads (PhysicsThreads::Start), or run
	// on the stepping thread (the default). What is simulated is the same either way.
	void SetPhysicsThreaded( bool threaded )
	{
		m_tasks->threaded = threaded;
	}
	// For tests: the number of workers simulations made from now on have (kPhysicsWorkers; what is
	// simulated must not depend on it).
	static void SetWorkersForNewSimulations( uint32_t workers );

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
	// A new entity with the next NetId of its side (SimGlobals::nextNetId): the characters' for a
	// player and what characters make (an item launched, given, split), the physics world's for the rest.
	flecs::entity CreateEntity( bool character = false );
	void DestroyEntity( flecs::entity e );

	// The physics world's inputs (physics_inputs.cpp).
	bool m_followsPhysics = false;
	InputFrame m_physicsMade;
	void PlacePushers( const InputFrame& frame );
	void MakePusherPlaces( std::vector<PusherPlace>& out );
	// Physics events: queued while characters act, applied at one point (after the characters move,
	// before the physics step); a follower applies its frame's instead of its own.
	std::vector<PhysicsEvent> m_physicsQueue;
	void ApplyPhysicsEvents( const InputFrame& frame, uint8_t phase, uint16_t after = 0 );
	void ApplyPhysicsEvent( PhysicsEvent& event );
	// A body leaving the physics world for a character (M247): its handle, kept until the TakeOver
	// event that destroys it is applied (this step), and the event queued. What reads it meanwhile
	// reads the character's own.
	std::vector<std::pair<uint32_t, b3BodyId>> m_leaving;
	void LeavePhysics( flecs::entity e, b3BodyId physicsBody );
	// An item a character lets go of: out of its hand now, into the world when the events are applied.
	void QueueIntoWorld( flecs::entity item, b3Vec3 grip, b3Quat rotation, b3Vec3 velocity, uint32_t propOwner = 0, uint32_t lifetimeTicks = 0 );
	// The entity with this NetId, made when there is none (what a follower's events name).
	flecs::entity EntityWithId( uint32_t netId );
	PhysicsEvent RagdollEvent( flecs::entity player, uint32_t lifetimeTicks ) const;

	// The state model (stand_ins.cpp).
	bool m_followsServer = false;
	bool StandsIn( flecs::entity e ) const
	{
		return m_followsServer && e.has<StandIn>();
	}
	// An entity's bodies described (the PhysicsBody's, or the ragdoll's parts), and built again.
	void DescribeBodies( flecs::entity e, std::vector<uint8_t>& out ) const;
	void DescribeBody( b3BodyId body, std::vector<uint8_t>& out ) const;
	// How a body is built from its description: as described; placed (kinematic, a static one static:
	// a stand-in, a driven prop); dynamic, weighing what its shapes do (a driven prop let go of).
	enum class BuildAs
	{
		Described,
		Placed,
		Dynamic,
	};
	b3BodyId BuildBody( ByteReader& r, b3WorldId world, BuildAs as, Transform at, b3Vec3 linear, b3Vec3 angular, b3ShapeId& first );
	// A body moved to the other world (M247: a prop driven, and let go of): the same body, built
	// there from its description, where it is and as fast; the old one is gone. Its new handles.
	PhysicsBody MoveBodyToWorld( b3BodyId body, b3WorldId to );
	void DestroyBodies( flecs::entity e );
	// Gone from the server's world: its bodies and the entity, nothing else.
	void RemoveForState( flecs::entity e );
	int SnapIndex( flecs::entity_t id ) const;

	flecs::entity CreateProp( ShapeKind kind, b3Vec3 position, b3Quat rotation, b3Vec3 halfExtents, b3Vec3 velocity,
							  uint32_t owner, uint32_t lifetimeTicks, const ShapeMaterial& material = {} );
	// Builds an entity from a map template: shape, body, material and initial velocity all come
	// from the authored values, with the engine's defaults for anything the author left alone.
	// `standing`: a static body whatever the template says (a breakable's piece left standing).
	flecs::entity CreateFromTemplate( uint32_t templateIndex, b3Vec3 position, b3Quat rotation, b3Vec3 extraVelocity,
									  uint32_t owner, bool standing = false );
	// A breakable's pieces near `blast` (all of them when `radius` is 0) fly away from it at
	// `speed / (1 + distance)`; then any standing piece no chain of touching pieces joins to a held
	// one falls. `target` is the whole wall or any piece of it left standing. `rubbleTicks` > 0:
	// the pieces that go are removed after that long.
	void BreakWall( flecs::entity target, b3Vec3 blast, float radius, float speed, uint32_t rubbleTicks );
	// A glued breakable (EntityTemplate::glue) where the map places it: its pieces made at once, the
	// held ones static, welded where they touch.
	void CreateGluedWall( uint32_t templateIndex, b3Vec3 position, b3Quat rotation );
	// After a step: the welds pulled harder than their glue, broken; then any glued piece left with
	// no weld is loose (rubble).
	void BreakGlue();
	void LoosenPieces();
	flecs::entity CreatePlayer( PlayerSlot slot );
	// Bodies nobody drives, and taking them over (M220).
	flecs::entity CreateIdleBody( uint16_t character, b3Vec3 feet, float yaw, b3Vec3 velocity );
	b3BodyId CreateIdlePhysics( const Shape& shape, const Transform& t, b3Vec3 velocity, float mass, b3ShapeId& shapeId );
	void MakeIdle( flecs::entity e );
	void MakeDriven( flecs::entity e, PlayerSlot slot );
	// A prop taken over (M221) and given back; its bounds holding its move back.
	void DriveProp( flecs::entity e, PlayerSlot slot );
	void ReleaseProp( flecs::entity e );
	// Everyone who moves this tick: the players, by slot, then the bodies mods drive, by NetId.
	struct DriverRef
	{
		uint32_t netId;
		PlayerInput input;
		int slot; // -1: a mod's
	};
	std::vector<DriverRef> m_drivers;
	void HoldBackProp( flecs::entity e, const DrivenProp& prop, b3Vec3 start, float startYaw, Transform& t, Character& c );
	void Possess( PlayerSlot slot, flecs::entity body );

public:
	// Whether a body may be seen through its eyes: its character's first-person camera, and not a prop.
	bool SeenInFirstPerson( flecs::entity e ) const;
	// The point a body's third-person camera orbits, given where the body is (its Transform): the
	// view pivot above its centre (a driven prop's own centre, below the mover's), where its line of
	// sight passes and its lines start.
	b3Vec3 ViewPivot( flecs::entity e, b3Vec3 position ) const;
	// Whether a slot may drive it (Possess): a character's body nobody drives, or a loose prop.
	bool Possessable( flecs::entity e ) const;

private:
	void PlaceCharacter( flecs::entity e, b3Vec3 position, float yaw );
	b3ShapeId CreateShape( b3BodyId body, const Shape& shape, uint64_t category, const ShapeMaterial& material = {} );
	// A convex hull of `points` (in the body's frame); null if they make none (all in a plane).
	b3ShapeId CreateHullShape( b3BodyId body, const std::vector<b3Vec3>& points, uint64_t category, const ShapeMaterial& material = {} );
	// A body's shapes as a set says (each piece where it is in the body's frame); the first one's
	// id, and in `bounds` the half size of a box about the body's origin that holds them all.
	b3ShapeId CreateShapeSet( b3BodyId body, const std::vector<LevelBox>& set, uint64_t category, const ShapeMaterial& material,
							  b3Vec3& bounds );
	// A level piece of a triangle mesh or a height field (static only), at the body's origin.
	b3ShapeId CreateSurfaceShape( b3BodyId body, const LevelBox& piece );
	// One shape of a body, `at` in its frame (a hull's `points` in its own); `bounds` grows to hold it.
	b3ShapeId CreatePiece( b3BodyId body, uint64_t category, const ShapeMaterial& material, ShapeKind kind, b3Vec3 half, b3Transform at,
						   const std::vector<b3Vec3>* points, b3Vec3& bounds );
	// The body's shapes' densities scaled so it weighs `mass` (nothing for 0).
	void WeighBody( b3BodyId body, float mass );
	// A body's handles as a component keeps them: which world it is in, not the world's id.
	PhysicsBody MakePhysicsBody( b3BodyId body, b3ShapeId shape ) const;

	void ApplyEvents( const InputFrame& frame );
	void ApplyCommands( const InputFrame& frame );
	void ApplyCommand( const SimCommand& command );
	// A Spawn physics event's prop (the mod's SpawnProp, its owner a NetId).
	void SpawnProp( const SimCommand& command );
	uint32_t ResolveTarget( uint32_t target ) const;
	void ApplyImpulse( flecs::entity e, const SimCommand& command );
	void KillPlayer( flecs::entity e, const SimCommand& command );
	void RespawnPlayer( flecs::entity e, const SimCommand& command );
	// A ragdoll as a Ragdoll physics event says (RagdollEvent makes one from a player).
	flecs::entity CreateRagdoll( const PhysicsEvent& event );
	void EnforceRagdollCap( uint32_t cap );
	void MoveCharacters( const InputFrame& frame );
	// Slots (types.h): each player's intent of this tick, before anything asks what it holds.
	void StepSlots( const InputFrame& frame );
	// The item a player has in a slot (0: none).
	uint32_t SlotItemOf( uint32_t holder, uint8_t slot ) const;
	// A new arrival finds its slot (its kind's own, pushing out what is there; else the first free
	// one). False when there is none: the player cannot carry it.
	bool GiveSlot( flecs::entity player, flecs::entity item, bool select );
	// Up to `count` of `from`'s stack onto `onto`, a stack of the same kind with room (the rest of
	// `from` stays; all of it: `from` is gone). False, and nothing done, when they do not stack.
	bool MergeStacks( flecs::entity from, flecs::entity onto, uint32_t count );
	// Into a container's slots: onto a stack of its kind with room, else the first free slot. False
	// (and the item as it was) when it is full.
	bool PutInContainer( flecs::entity box, flecs::entity item );
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
	// A line in flight goes on one tick: what it meets on the way it takes hold of (its motion takes
	// effect from now, and says its hold event), or at the end of its range it has found nothing.
	void FlyHold( flecs::entity e, MotionState& motion );
	// `hold` takes hold of what `hit` found: a player or a loose body it follows, else the world.
	void HoldOn( MotionHold& hold, const RayHit& hit ) const;
	// What a probe's hit is: kProbeHoldsLevel, kProbeHoldsBodies or kProbeHoldsPlayers (motions.h).
	uint8_t HoldKind( const RayHit& hit ) const;
	// The effects of the motions that are on for the player `e` this tick: impulses, forces and
	// links, on the player (`c`, not yet stored) and on their targets.
	struct MotionEnd;
	MotionEnd ResolveEnd( flecs::entity self, const Transform& t, const MotionTarget& target, const MotionHold* hold,
						  const Blackboard& board ) const;
	b3Vec3 EndVelocity( const MotionEnd& end, const Character& c ) const;
	void PushEnd( const MotionEnd& end, Character& c, b3Vec3 change );
	// The end an entity is (a live player, a dynamic body that is not a ragdoll); None otherwise.
	MotionEnd EndOf( flecs::entity self, const Transform& t, flecs::entity who ) const;
	// An impulse on `target` along `direction`, after taking away what it replaces.
	void ImpulseEnd( const MotionEffect& effect, float strength, const MotionEnd& target, Character& c, b3Vec3 direction );
	// A find of a motion step that starts now: what its shape overlaps, and what happens to each.
	void RunFind( flecs::entity e, Character& c, const Transform& t, const PlayerInput& in, const Motion& m, const MotionFind& find,
				  const MotionEnd& self, const MotionEnd& hit, const AnimGraphInputs& values, uint32_t sayer );
	// A movement parameter given to a player for some seconds (MoveEffects).
	void GiveMove( flecs::entity who, uint8_t param, float value, float seconds );
	// An item's motions (owner item) for an event said at it: each that answers it, once.
	void RunItemMotions( flecs::entity item, int event, bool expiring );
	// The motions an event said at a live item this tick starts (its own expiry runs before it goes).
	void RunItemEvents();
	// A step's walls: those near the owner broken, as its breaks say.
	void RunBreaks( const Motion& m, const MotionStep& step, const MotionSlot& slot, b3Vec3 at, const AnimGraphInputs& values );
	void ApplyMotionEffects( flecs::entity e, Character& c, const Transform& t, const PlayerInput& in, const MotionState& state,
							 const Blackboard& board, const std::vector<MotionActive>& active, const AnimGraphInputs& values );
	void ExpireProps();
	void EnforcePropCaps();
	void SyncFromPhysics();
	// Reads Box3D's contact hit events into the impact ring, strongest first.
	void CollectImpacts();
	// Items that break walls (ItemShape::breakSpeed): how they move before the step, and the walls
	// they met in it broken, there and then (so every simulation breaks them on the same tick).
	void RememberBreakers();
	void BreakByItems();
	// Advances each character's stride and counts a step whenever it completes one.
	void UpdateFootsteps();
	void HandleOutOfBounds();

	void SerializeEcs( std::vector<uint8_t>& out ) const;
	void DeserializeEcs( const std::vector<uint8_t>& in );

	SimConfig m_config;
	LevelLayout m_map;
	SimGlobals m_globals;
	StepProfile m_profile;
	flecs::world m_world;
	std::unique_ptr<PhysicsArena> m_arena;
	// The level's triangle meshes and height fields: static, never changed, so out of the world's
	// arena (a snapshot does not copy them, a restore leaves them where they are). Made only for a
	// map that has them; what a portable load brings is kept in `m_keptGeometry`.
	std::unique_ptr<PhysicsArena> m_levelArena;
	std::vector<std::unique_ptr<uint64_t[]>> m_keptGeometry;
	std::unique_ptr<PhysicsTasks> m_tasks;
	b3WorldId m_physicsWorld = {};
	b3WorldId m_characterWorld = {};

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
	// Shape index -> NetId, rebuilt only when a shape has to be named (impacts, ray casts) and
	// something changed since: a body made or gone, an entity added or removed, a snapshot loaded
	// (the observers in RegisterIndexes). Derived data: never hashed, never in a snapshot.
	std::vector<std::pair<uint32_t, uint32_t>> m_shapeLookup;
	bool m_shapeLookupStale = true;
	// Each holder's items, in m_entities' order: what SlotItemOf and SettleSlots walk instead of
	// every entity. Rebuilt when an item gains, loses or changes its holder (the HeldItem
	// observer), an entity is added or removed, or a snapshot is loaded. An item's own fields are
	// read fresh at each use, so only who holds what is cached. Derived data, like the above.
	mutable std::unordered_map<uint32_t, std::vector<EntityRef>> m_itemsByHolder;
	mutable bool m_itemsByHolderStale = true;
	std::unordered_map<flecs::entity_t, uint32_t> m_holderOf; // what the index was built with
	const std::vector<EntityRef>& ItemsOf( uint32_t holder ) const;
	void RegisterIndexes();

public:
	// Tests: every answer of the indexes above is compared with a scan of every entity, and a
	// difference stops the process (a stale index the observers missed). Off in the game.
	static inline bool s_checkIndexes = false;

private:
	void IndexesStale()
	{
		m_shapeLookupStale = true;
		m_itemsByHolderStale = true;
	}
	std::vector<ImpactRecord> m_impactScratch;
	std::vector<int> m_markerScratch;
	std::vector<std::pair<uint32_t, uint16_t>> m_heldScratch; // holder NetId, item kind
	std::vector<std::pair<uint32_t, uint16_t>> m_carriedScratch; // the same, with what is put away
	std::vector<std::pair<uint32_t, uint16_t>> m_usingScratch;	 // what each is using this tick ("using.<kind>")
	std::vector<uint32_t> m_usingItems;							 // the same, by the copy

	std::vector<CharacterBody> m_characters = std::vector<CharacterBody>( 1 );
	bool m_someSeenFromBehind = false; // a character without first person (Step narrows the views)
	bool AnyGraph() const;
	std::vector<std::shared_ptr<const AnimGraph>> m_animPacks;
	std::vector<ItemShape> m_itemShapes;
	// Some kind breaks walls (ItemShape::breakSpeed); and those of its copies lying in the world, as
	// they moved before this step (what they had before they hit something).
	bool m_anyBreaks = false;
	std::vector<std::pair<uint32_t, b3Vec3>> m_breakersBefore;
	std::vector<ItemUse> m_useScratch;
	std::shared_ptr<const Motions> m_motions;
	std::vector<ModEventRecord> m_motionEvents;
	std::vector<MotionActive> m_motionActive;
	std::vector<uint32_t> m_findScratch; // what a find's shape overlaps
	// The item motions of each item kind (schema index): places in m_motions' list.
	std::vector<std::vector<uint16_t>> m_itemMotions;
	// What this tick's motions throw (a Launch effect), made once every player has moved: an item
	// of a kind, where, how fast, whose, and for how long.
	struct Launched
	{
		uint16_t kind;
		b3Vec3 position;
		b3Quat rotation;
		b3Vec3 velocity;
		uint32_t owner;
		uint32_t lifetimeTicks;
		uint32_t item = 0; // the carried copy thrown; 0: a new one of `kind`
	};
	std::vector<Launched> m_launched;
	void CreateLaunched();
	// What players' motions do to other players this tick, kept until every player's motions have
	// run, then applied in the order they were made (MoveCharacters). So whoever has the lower slot,
	// a push is felt on the tick it is given, after the target's own motions, and every motion reads
	// the others' velocities as the tick found them. Scratch: empty between steps.
	struct PlayerPush
	{
		uint32_t netId;
		MotionEffect::Replace replace;
		b3Vec3 change;
	};
	std::vector<PlayerPush> m_playerPushes;
	// The last start of a motion refused for each player, and when (DescribeMotions). Not state:
	// only the debug HUD reads it.
	struct MotionNote
	{
		uint16_t motion = 0;
		MotionRefusal::Why why = MotionRefusal::Why::None;
		uint32_t tick = 0;
	};
	std::array<MotionNote, kMaxPlayers> m_motionNotes{};
	std::vector<MotionRefusal> m_refusalScratch;
	bool m_deferPlayerPushes = false;
	// Each player's character between its motions and its move (MoveCharacters), by slot.
	std::vector<Character> m_turnCharacters;
	void ApplyPlayerPushes();
	// The values of the moves of `motion`'s step that are expressions, read as it starts (MoveValues).
	void KeepMoveValues( flecs::entity e, uint16_t motion, const MotionStep& step, const MotionSlot& slot, const AnimGraphInputs& values );
	// The copy of `kind` the player `holder` carries that a launch throws (in a hand first, then the
	// lowest slot's), not one already thrown this tick; 0: none.
	uint32_t CarriedCopyOf( uint32_t holder, uint16_t kind ) const;
	// An item leaves the hand and lies in the world at its grip (a body of its kind's shape), or
	// the other way round.
	void PutItemInWorld( flecs::entity item, b3Vec3 grip, b3Quat rotation, b3Vec3 velocity );
	void TakeItemFromWorld( flecs::entity item, uint32_t holder, uint8_t socket );
	void RecordModEvent( const ModEventRecord& record );
	// A probe that takes hold says its hold_emits (Motion::holdEvent), with what it holds.
	void SayHold( flecs::entity e, const Motion& m, uint32_t anchor, b3Vec3 point );
	void FollowHolders();
};

} // namespace cb
