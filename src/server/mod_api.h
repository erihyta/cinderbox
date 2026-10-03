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

// An animation layer and a stance, by name ("upper", "pistol"): what a mod sets on a player, and a
// character's state machine reads in its conditions. A default StanceHandle means "no stance".
struct LayerHandle
{
	int index = -1;

	bool Valid() const
	{
		return index >= 0;
	}
};

struct StanceHandle
{
	int index = -1;

	bool Valid() const
	{
		return index >= 0;
	}
};

// A kind of held item ("melee.bat") and a socket to hold it in ("RightHand"; the two hands are
// built in).
struct ItemKindHandle
{
	int index = -1;

	bool Valid() const
	{
		return index >= 0;
	}
};

struct SocketHandle
{
	int index = -1;

	bool Valid() const
	{
		return index >= 0;
	}
};

// An animation pack the mod ships (AnimationTree layers baked into its item under anim/<name>/).
struct AnimPackHandle
{
	int index = -1;

	bool Valid() const
	{
		return index >= 0;
	}
};

// The body an item has when it lies in the world, in its grip's frame (held in a socket, the grip is
// at the socket and the item points along -Z): a box of half extents, or a sphere, whose centre is
// `center` from the grip, weighing `mass` kg.
inline ItemShape BoxItem( b3Vec3 halfExtents, b3Vec3 center, float mass )
{
	return { 0, { halfExtents.x, halfExtents.y, halfExtents.z }, { center.x, center.y, center.z }, mass };
}
inline ItemShape SphereItem( float radius, b3Vec3 center, float mass )
{
	return { 1, { radius, radius, radius }, { center.x, center.y, center.z }, mass };
}

// An item a player carries, as CarriedItems finds it.
struct CarriedItem
{
	uint32_t netId = 0;
	ItemKindHandle kind;
	bool stowed = false; // false: in use, in `socket`
	SocketHandle socket; // invalid: stowed out of sight
};

// An item lying in the world, as ItemsNear finds it.
struct WorldItem
{
	uint32_t netId = 0;
	ItemKindHandle kind;
	b3Vec3 position = {}; // its body's centre
	float distance = 0.0f;
};

// What mods asked each player's layers to play (SwapLayer; 0: nothing asked), kept by the server
// between ticks. The layer that plays is the mod's wish, else what a held item brings, else the
// player's own (Context::ResolveLayers).
struct LayerWishes
{
	uint8_t mod[kMaxPlayers][kMaxAnimLayers] = {};
};

// The item the player in `slot` holds in `socket`, as a command target (SetField, Emit, Destroy).
inline uint32_t ItemTarget( PlayerSlot slot, SocketHandle socket )
{
	return cb::ItemTarget( slot, uint32_t( socket.index ) );
}

// Collects what every mod declares. Names are shared: two mods declaring the same field get the
// same slot (so one mod can read what another publishes), as long as they agree on its type.
class Declarations
{
public:
	FieldHandle Field( const std::string& name, BoardType type, BoardScope scope = BoardScope::Entity );
	EventHandle Event( const std::string& name );
	// `key` is the suggested binding, as Godot names keys ("F", "R", "1") or "MouseLeft".
	ActionHandle Action( const std::string& name, const std::string& key );
	// Layers apply in declaration order (a later one wins where masks overlap). At most
	// kMaxAnimLayers. "full" (every bone) and "upper" (the spine up) work on any character; others
	// need the character to define the mask.
	LayerHandle Layer( const std::string& name );
	StanceHandle Stance( const std::string& name );
	// Held items: a kind (its look is the mod's client item's, by this name) and a socket.
	// "RightHand" and "LeftHand" exist on every character; another socket is drawn only on
	// characters that define it.
	ItemKindHandle ItemKind( const std::string& name );
	// The same, with the body it has when it lies in the world (BoxItem, SphereItem), for a mod
	// without a look. A body baked from the item's scene (a CbItemBody, items/<kind>.cfg in the mod's
	// item) replaces it; with neither it is a small box. The first shape declared for a kind is kept.
	ItemKindHandle ItemKind( const std::string& name, const ItemShape& shape );
	// While a player holds an item of `kind`, `pack`'s layers play instead of the player's own of the
	// same names: a bat that changes how its holder stands and walks, wherever the bat came from.
	// A mod's own SwapLayer on such a layer wins while it lasts (a crouch over a carry).
	void ItemLayers( ItemKindHandle kind, AnimPackHandle pack );
	// A named number about an item kind that any mod may read (Context::ItemProperty): how mods agree
	// on what an item is like without knowing each other ("pickup.hold_seconds" = 0.5: the pickup
	// mod makes players hold the key that long for it). The first value declared for a name is kept.
	void ItemProperty( ItemKindHandle kind, const std::string& name, float value );
	// The same for a socket ("inventory.holster" = Back: where the inventory mod hangs it while it is
	// put away). Read with Context::ItemSocket.
	void ItemProperty( ItemKindHandle kind, const std::string& name, SocketHandle socket );
	// An animation pack in this mod's client item: its layers can replace a player's own of the same
	// name (Context::SwapLayer). Name it like the mod's other names ("sneak.crouch").
	AnimPackHandle AnimPack( const std::string& name );
	SocketHandle Socket( const std::string& name );

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
	std::vector<bool> m_shapeDeclared; // per item kind: a mod gave it a shape
	std::vector<std::vector<std::string>> m_itemMods; // per item kind: the mods that declared it
	std::map<std::pair<int, std::string>, float> m_itemProperties;
	std::map<int, int> m_itemLayers; // item kind -> animation pack

public:
	const std::map<int, int>& ItemLayersByKind() const
	{
		return m_itemLayers;
	}
	const std::map<std::pair<int, std::string>, float>& ItemProperties() const
	{
		return m_itemProperties;
	}
	// The mods that declared each item kind (schema order): where its baked body may be found.
	const std::vector<std::vector<std::string>>& ItemMods() const
	{
		return m_itemMods;
	}
	int m_entitySlots = 0;
	int m_globalSlots = 0;
	int m_privateSlots = 0;
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
	// The point above a player's body that a third-person camera orbits, and the direction its
	// camera looks in. (Mods take the chest from it for things done at arm's length.)
	b3Vec3 EyePosition( PlayerSlot slot ) const;
	b3Vec3 AimDirection( PlayerSlot slot ) const;
	// The head of the player's pose this tick (it bows and leans with the body): what a shot starts
	// from, whatever camera the player looks through.
	b3Vec3 HeadPosition( PlayerSlot slot ) const;
	// Where the player's line of sight starts for the camera it looks through (PlayerInput::view):
	// in first person the character's eye height above its feet (fixed on the mover: the animation
	// does not move it), in third the camera's pivot (moved to the shoulder, if it is).
	b3Vec3 ViewPosition( PlayerSlot slot ) const;
	// What the player hits when it shoots at what is under its crosshair, up to `range` metres:
	// the line of sight says what is aimed at, and the shot goes from the head to that point.
	// Something between the head and the target stops the shot even when the camera sees past it. `origin` and `direction` are the shot's. False: it hit nothing.
	bool CastAim( PlayerSlot slot, float range, RayHit& hit, b3Vec3& origin, b3Vec3& direction ) const;

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
	// ("combat.killed") without knowing it. At most the ring's size per tick. Markers in the
	// character's animations record events too, from the player they played on.
	std::vector<ModEventRecord> RecentEvents() const;
	// Whether the character's animations have a marker that emits `event` ("melee.strike" on the
	// frame the swing connects). A mod can then time its effect by the animation, and keep its own
	// timer for characters without one.
	bool AnimationEmits( EventHandle event ) const;

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
	// Sets `stance` on the player's `layer` (a default StanceHandle clears it). The character's
	// state machine reads stances by name in its conditions and weights ("pistol", "melee_swing"),
	// so what a stance looks like is the character's: part of the pose everyone draws and hit
	// tests use.
	void SetStance( uint32_t target, LayerHandle layer, StanceHandle stance );
	// Gives the player `holder` (SlotTarget) an item of `kind` in `socket`, replacing what it held
	// there. The item is an entity of its own: address it with ItemTarget( slot, socket ) (from this
	// tick on) to set its fields, send it events, or Destroy it. To take away your own item, Destroy
	// the NetId HeldItem() gives, not ItemTarget: ItemTarget is resolved when the command runs, and
	// another mod may have put its item in that socket in the same tick (a weapon swap).
	void SpawnItem( uint32_t holder, ItemKindHandle kind, SocketHandle socket );
	// The same, stowed: the player carries it without holding it (HoldItem takes it out). `holster`
	// is the socket it is drawn in meanwhile; none: out of sight. Nothing is dropped for it.
	void GiveItem( uint32_t holder, ItemKindHandle kind, SocketHandle holster = {} );
	// Puts a held item away (in `holster`, or out of sight), or takes a carried one in use into
	// `socket`. HoldItem does nothing when that socket has an item in use: stow that one first, in
	// the same tick. Stowed items are in no hand: HeldItem, item layers and state machines do not see
	// them; CarriedItems does.
	void StowItem( uint32_t item, SocketHandle holster = {} );
	void HoldItem( uint32_t item, SocketHandle socket );
	// Everything the player in `slot` carries, in use and stowed, in NetId order.
	std::vector<CarriedItem> CarriedItems( PlayerSlot slot ) const;
	// Plays `pack`'s layer named `layer` ("Base") instead of the player's own, from its start; the
	// layer's name is the character's (its AnimationTree's). Does nothing when the character has no
	// such layer. The pose follows it everywhere, the server's hit tests too.
	void SwapLayer( uint32_t target, AnimPackHandle pack, const std::string& layer );
	// The player's own layer again (or the one a held item brings).
	void RestoreLayer( uint32_t target, const std::string& layer );
	// The server's: where mods' swaps are kept, and which packs items bring. After every mod has
	// ticked, ResolveLayers turns both into SwapLayer commands for the layers that change.
	void SetLayers( LayerWishes* wishes, const std::map<int, int>* itemLayers )
	{
		m_layerWishes = wishes;
		m_itemLayers = itemLayers;
	}
	void ResolveLayers();
	// The NetId of what the player in `slot` holds in `socket` (as of the start of this tick), or 0.
	uint32_t HeldItem( PlayerSlot slot, SocketHandle socket ) const;
	// A number a mod declared about an item kind (Declarations::ItemProperty), or `fallback`.
	float ItemProperty( ItemKindHandle kind, const std::string& name, float fallback ) const;
	// A socket a mod declared about an item kind, or an invalid handle.
	SocketHandle ItemSocket( ItemKindHandle kind, const std::string& name ) const;
	// Where private fields live: one board per player slot, kept by the server (not the simulation)
	// and sent to that player alone. `changed` gets the slots a Set wrote to.
	void SetPrivates( std::array<Blackboard, kMaxPlayers>* privates, std::array<bool, kMaxPlayers>* changed )
	{
		m_privates = privates;
		m_privatesChanged = changed;
	}
	void SetItemProperties( const std::map<std::pair<int, std::string>, float>* properties )
	{
		m_itemProperties = properties;
	}
	// Its kind (an invalid handle when it is not an item), and who holds it (0: it lies in the world).
	ItemKindHandle ItemKindOf( uint32_t netId ) const;
	uint32_t ItemHolder( uint32_t netId ) const;
	// Every item, held or lying, in NetId order (ItemHolder says which).
	std::vector<uint32_t> Items() const;
	// Items lying in the world within `radius` of `point` (their bodies' centres), nearest first
	// (ties by NetId).
	std::vector<WorldItem> ItemsNear( b3Vec3 point, float radius ) const;

	// Items in the world. `grip` is where the item's grip goes, `rotation` how it is turned (the item
	// points along its -Z); it gets its kind's body and falls from there.
	void SpawnWorldItem( ItemKindHandle kind, b3Vec3 grip, b3Quat rotation, b3Vec3 velocity = {} );
	// Out of the hand (any item target: a NetId, or ItemTarget) into the world.
	void DropItem( uint32_t item, b3Vec3 grip, b3Quat rotation, b3Vec3 velocity = {} );
	// Into `holder`'s (SlotTarget) socket, from the world. Nothing happens when the socket is taken
	// (drop what is there first, in the same tick) or the item is held already.
	void PickUpItem( uint32_t holder, uint32_t item, SocketHandle socket );
	// From the world straight to stowed, whatever the hands hold.
	void PickUpStowed( uint32_t holder, uint32_t item, SocketHandle holster = {} );

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
	const std::map<std::pair<int, std::string>, float>* m_itemProperties = nullptr;
	LayerWishes* m_layerWishes = nullptr;
	std::array<Blackboard, kMaxPlayers>* m_privates = nullptr;
	std::array<bool, kMaxPlayers>* m_privatesChanged = nullptr;
	const std::map<int, int>* m_itemLayers = nullptr;
	// Which layer of the character `layer` names, and whose slot `target` is (-1: neither a slot nor
	// a player).
	int LayerIndex( const std::string& layer ) const;
	int SlotOfTarget( uint32_t target ) const;
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
