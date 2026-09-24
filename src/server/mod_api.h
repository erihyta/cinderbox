#pragma once

// Server gameplay mods.
//
// A mod is C++ compiled into cb_server (see server_mods/). It holds the game's rules: what a pistol
// does, when a player dies, how props are spawned. It runs only on the server, once per tick,
// before the simulation steps, and it never touches the simulation directly. Everything it wants
// to happen goes out as commands in that tick's authoritative frame, which every client applies
// exactly like the server does, so the rules stay on the server and the world stays deterministic.
//
// What a mod can do:
//   - declare board fields, events and actions (Declare), which become the schema clients get;
//   - read the world as it is before this tick (players, positions, board values, ray casts)
//     and this tick's inputs;
//   - keep its own state however it likes; every mod shares one flecs world for it, so mods can
//     see each other's components;
//   - emit commands: set fields, announce events, spawn, destroy, push, kill, respawn.
//
// What it cannot do is see the future or the client's prediction: it reads the confirmed state, and
// its commands reach clients with the frame, where rollback fixes up whatever they predicted.

#include "mod_schema.h"
#include "simulation.h"

#include "flecs.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cb
{
class HitTester;
}

namespace cb::mods
{

struct FieldHandle
{
	int slot = -1;
	BoardScope scope = BoardScope::Entity;
	BoardType type = BoardType::Int;

	bool Valid() const
	{
		return slot >= 0;
	}
};

struct EventHandle
{
	int index = -1;

	bool Valid() const
	{
		return index >= 0;
	}
};

struct ActionHandle
{
	uint16_t mask = 0;

	bool Valid() const
	{
		return mask != 0;
	}
};

// Collects what every mod declares. Names are shared: two mods declaring the same field get the
// same slot (so one mod can read what another publishes), as long as they agree on its type.
class Declarations
{
public:
	FieldHandle Field( const std::string& name, BoardType type, BoardScope scope = BoardScope::Entity );
	EventHandle Event( const std::string& name );
	// `key` is the suggested binding, as Godot names keys ("F", "R", "1") or "MouseLeft".
	ActionHandle Action( const std::string& name, const std::string& key );

	const ModSchema& Schema() const
	{
		return m_schema;
	}
	// Empty when everything fit; otherwise why a declaration was refused.
	const std::vector<std::string>& Errors() const
	{
		return m_errors;
	}

	void BeginMod( const std::string& name );

private:
	ModSchema m_schema;
	std::vector<std::string> m_errors;
	std::string m_mod;
	int m_entitySlots = 0;
	int m_globalSlots = 0;
};

// One tick, as a mod sees it.
class Context
{
public:
	Context( Simulation& sim, const ModSchema& schema, InputFrame& frame, const std::array<PlayerInput, kMaxPlayers>& previous,
			 flecs::world& world, uint64_t& rng );

	// --- Reading ------------------------------------------------------------------------------

	// The tick being built; the world is as it was after the previous one.
	uint32_t Tick() const
	{
		return m_frame.tick;
	}
	const SimConfig& Config() const
	{
		return m_sim.Config();
	}
	const LevelLayout& Map() const
	{
		return m_sim.Map();
	}
	const ModSchema& Schema() const
	{
		return m_schema;
	}

	// Join and leave events in this tick's frame. A joining player's entity does not exist yet,
	// but commands addressed to its slot are applied after it is created.
	bool Joining( PlayerSlot slot ) const;
	bool Leaving( PlayerSlot slot ) const;
	// In the world before this tick.
	bool InWorld( PlayerSlot slot ) const
	{
		return m_sim.IsPlayerActive( slot );
	}
	uint32_t PlayerNetId( PlayerSlot slot ) const
	{
		return m_sim.PlayerNetId( slot );
	}
	const Character* PlayerCharacter( PlayerSlot slot ) const
	{
		return m_sim.PlayerCharacter( slot );
	}
	const Transform* EntityTransform( uint32_t netId ) const
	{
		return m_sim.EntityTransform( netId );
	}
	// The slot of the player with this NetId, or -1.
	int SlotOf( uint32_t netId ) const;
	// Where a player looks from: the point the third-person camera orbits (and so the point the
	// crosshair ray passes through), and the direction of its camera.
	b3Vec3 EyePosition( PlayerSlot slot ) const;
	b3Vec3 AimDirection( PlayerSlot slot ) const;

	const PlayerInput& Input( PlayerSlot slot ) const
	{
		return m_frame.inputs[slot];
	}
	bool Held( PlayerSlot slot, ActionHandle action ) const
	{
		return ( m_frame.inputs[slot].actions & action.mask ) != 0;
	}
	// Went down this tick.
	bool Pressed( PlayerSlot slot, ActionHandle action ) const
	{
		return Held( slot, action ) && ( m_previous[slot].actions & action.mask ) == 0;
	}

	int32_t Get( uint32_t netId, FieldHandle field ) const;
	float GetFloat( uint32_t netId, FieldHandle field ) const
	{
		return BoardToFloat( Get( netId, field ) );
	}
	int32_t GetGlobal( FieldHandle field ) const;

	// The closest thing along the ray. Players are hit by their character's hitboxes, posed as they
	// are this tick, and `hit.zone` names the one hit ("head", "torso", ...).
	bool CastRay( b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit ) const;

	// What kind of entity a NetId is.
	bool IsPlayer( uint32_t netId ) const
	{
		return SlotOf( netId ) >= 0;
	}
	bool IsDynamic( uint32_t netId ) const;

	// Entities of a kind, in NetId order (creation order).
	std::vector<uint32_t> Props() const;
	// Only props a player spawned (the level's own props are left out).
	std::vector<uint32_t> SpawnedProps() const;
	std::vector<uint32_t> Ragdolls() const;

	// Mod events the previous tick recorded (any mod's): how one mod reacts to another's news
	// ("combat.killed") without knowing it. At most the ring's size per tick.
	std::vector<ModEventRecord> RecentEvents() const;

	// Server options for mods: cb_server --mod-option deathmatch.kills=15.
	double Option( const std::string& name, double fallback ) const;
	void SetHitTester( HitTester* hits )
	{
		m_hits = hits;
	}
	void SetOptions( const std::map<std::string, std::string>* options )
	{
		m_options = options;
	}

	// The mods' shared world for their own state. Never rolled back, never sent anywhere.
	flecs::world& World()
	{
		return m_world;
	}
	// Server-side randomness. It does not need to be repeatable: what it decides reaches clients
	// as the values in the commands.
	uint64_t Random();
	float RandomRange( float lo, float hi );

	// --- Commands -----------------------------------------------------------------------------
	// Targets are NetIds; use SlotTarget( slot ) for a player, which also works for one joining
	// this tick. Each call appends to this tick's frame, applied in order.

	void Set( uint32_t target, FieldHandle field, int32_t value );
	void SetFloat( uint32_t target, FieldHandle field, float value )
	{
		Set( target, field, BoardFromFloat( value ) );
	}
	void Emit( EventHandle event, uint32_t a, uint32_t b = 0, int32_t value = 0, b3Vec3 point = {}, b3Vec3 vector = {} );
	void SpawnProp( ShapeKind kind, b3Vec3 halfExtents, b3Vec3 position, float yaw, b3Vec3 velocity, uint32_t owner,
					uint32_t lifetimeTicks );
	void SpawnTemplate( uint32_t templateIndex, b3Vec3 position, float yaw, b3Vec3 velocity, uint32_t owner, uint32_t lifetimeTicks );
	void Destroy( uint32_t target );
	void Push( uint32_t target, b3Vec3 point, b3Vec3 vector, ImpulseMode mode );
	// ragdollLifetimeTicks 0 keeps it; ragdollCap 0 means no limit.
	void Kill( uint32_t target, bool ragdoll, b3Vec3 hitPoint = {}, b3Vec3 hitVelocity = {}, uint32_t ragdollLifetimeTicks = 0,
			   uint32_t ragdollCap = 0 );
	void Respawn( uint32_t target );
	void RespawnAt( uint32_t target, b3Vec3 position, float yaw );
	// A frozen player ignores movement and jump (mods decide what else a freeze means for them).
	void Freeze( uint32_t target, bool frozen );
	// Points the player's aim chain (its character's arm, by default) where it looks, or lets it
	// go. Part of the pose every client draws and every hit test uses.
	void Aim( uint32_t target, bool aiming );
	// true: the body faces where the camera looks (a shooter's stance; the legs still walk where it
	// goes). false: it turns toward where it walks (freelook, the default).
	void FaceCamera( uint32_t target, bool faceCamera );

	// Commands emitted so far this tick (tests).
	const std::vector<SimCommand>& Commands() const
	{
		return m_frame.commands;
	}

private:
	void Add( const SimCommand& command );

	Simulation& m_sim;
	const ModSchema& m_schema;
	InputFrame& m_frame;
	const std::array<PlayerInput, kMaxPlayers>& m_previous;
	flecs::world& m_world;
	uint64_t& m_rng;
	const std::map<std::string, std::string>* m_options = nullptr;
	HitTester* m_hits = nullptr;
};

class ServerMod
{
public:
	virtual ~ServerMod() = default;

	virtual const char* Name() const = 0;
	// Called once, before the server starts: declare fields, events and actions here.
	virtual void Declare( Declarations& declare ) = 0;
	// Called once the simulation exists, before the first tick.
	virtual void Start( Context& ) {}
	// Every tick, before the simulation steps.
	virtual void Tick( Context& ctx ) = 0;
};

using ModFactory = std::unique_ptr<ServerMod> ( * )();

struct ModInfo
{
	const char* name;
	ModFactory create;
	// The mod has a client project (server_mods/<name>/client): players need its workshop item.
	bool clientContent;
};

} // namespace cb::mods
