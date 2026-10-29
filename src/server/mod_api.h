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

#include "look_item.h"
#include "mod_schema.h"
#include "simulation.h"

#include "flecs.h"

#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cb
{
class NavMesh;
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

// An intent a mod declares (Declarations::Intent): a click on a screen sends it, the mod answers.
struct IntentHandle
{
	int index = -1;

	bool Valid() const
	{
		return index >= 0;
	}
};

// What a player typed into a channel's box (Context::Typed): who, where, and the words (printable,
// at most net::kMaxText bytes).
struct TypedText
{
	PlayerSlot slot = 0;
	std::string channel;
	std::string text;
};

// A line a mod says on a channel (Context::Say): to everyone, or to one player.
struct SaidText
{
	std::string channel;
	std::string text;
	PlayerSlot from = kNoSlot; // the player it is from (kNoSlot: the server's own words)
	PlayerSlot to = kNoSlot;	 // kNoSlot: everyone
};

// One a player sent (Context::Intents): who, and its two numbers.
struct IntentSent
{
	PlayerSlot slot = 0;
	uint8_t a = 0;
	uint8_t b = 0;
};

struct ActionHandle
{
	ActionBits mask = 0;

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

// A character the mod ships (ModSchema::characters): a body may be it (Context::SetCharacter).
struct CharacterHandle
{
	int index = -1;

	bool Valid() const
	{
		return index >= 0;
	}
};

// A set of motions the mod ships (sim/motions.h: CbMotion nodes baked into its item as
// motions/<name>.cfg).
struct MotionsHandle
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
	int slot = -1;		 // which of the player's slots it is in (-1: none)
	int count = 1;		 // how many it is: a stack of its kind
};

// An item lying in the world, as ItemsNear finds it.
struct WorldItem
{
	uint32_t netId = 0;
	ItemKindHandle kind;
	b3Vec3 position = {}; // its body's centre
	float distance = 0.0f;
};

// A loose body near a point, as BodiesNear finds it: a prop, an item lying in the world, a ragdoll.
struct NearBody
{
	uint32_t netId = 0;
	b3Vec3 position = {}; // its centre
	float distance = 0.0f;
	// About how big it is: from its centre to its furthest corner (a ragdoll: its pelvis). A push
	// at a point off its centre by some share of this turns it as well as moving it.
	float radius = 0.0f;
	// A ragdoll: the tick it fell at (0: it is not one). A push on one hits the part nearest its
	// point; ImpulseThrow throws the whole body.
	uint32_t ragdollSince = 0;
};

// What mods asked each player's layers to play (SwapLayer; 0: nothing asked), kept by the server
// between ticks. The layer that plays is the mod's wish, else what a held item brings, else the
// player's own (Context::ResolveLayers).
struct LayerWishes
{
	SmallList<uint8_t, 4> mod[kMaxPlayers];
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
	// Something a player asks of the mod from a screen, with two numbers ("shop.buy", the offer):
	// a CbClick's mod_intent sends it in the player's input; Context::Intents reads it. What it
	// does is the mod's to say (on the server: it is not predicted).
	IntentHandle Intent( const std::string& name );
	// A channel players can type into ("chat"): what they type there reaches the mods
	// (Context::Typed). Text for a channel no mod declared goes nowhere. Names are short (at most
	// 32 bytes): letters, digits, '.', '_' and '-'.
	void TextChannel( const std::string& name );
	const std::vector<std::string>& TextChannels() const
	{
		return m_textChannels;
	}
	// `key` is the suggested binding, as Godot names keys ("F", "R", "1") or "MouseLeft".
	ActionHandle Action( const std::string& name, const std::string& key );
	// Layers apply in declaration order (a later one wins where masks overlap). At most
	// kLayerLimit. "full" (every bone) and "upper" (the spine up) work on any character; others
	// need the character to define the mask.
	LayerHandle Layer( const std::string& name );
	StanceHandle Stance( const std::string& name );
	// Slots (sim/types.h): how many each player has. The engine carries out selecting, moving and
	// dropping from the players' own input; the mod that asks for slots says the rules (what a
	// life is given, what a death drops). The largest count asked for wins; at most kMaxSlots.
	// What a kind is like in a slot is two item properties the engine reads: "slot" (the slot it
	// goes to, from 1; without it the first free one) and "holster" (the socket it hangs in while
	// another slot is selected).
	void Slots( int count );
	// A server option (cb_server --mod-option inventory.rows=4), while declaring: what a mod declares
	// may depend on it (how many slots). The fallback where there is none (and for the editor's names).
	double Option( const std::string& name, double fallback ) const;
	void SetOptions( const std::map<std::string, std::string>* options )
	{
		m_options = options;
	}
	// A template the server adds to the map's, so a mod can place it (ctx.FindTemplate,
	// ctx.SpawnTemplate) and clients draw it: a container (a chest), a static box `size` metres
	// across with `slots` of its own, drawn as `visual` (a scene of the mod's look:
	// res://prefabs/<visual>.tscn). One the map has by that name already is the map's. Names are a
	// map's: letters, digits, _ and -.
	void ContainerTemplate( const std::string& name, const std::string& visual, b3Vec3 size, int slots );
	const std::vector<EntityTemplate>& Templates() const
	{
		return m_templates;
	}
	int SlotCount() const
	{
		return m_slots;
	}
	// Held items: a kind (its look is the mod's client item's, by this name) and a socket.
	// "RightHand" and "LeftHand" exist on every character; another socket is drawn only on
	// characters that define it.
	ItemKindHandle ItemKind( const std::string& name );
	// The same, with the body it has when it lies in the world (BoxItem, SphereItem), for a mod
	// without a look. A body baked from the item's scene (a CbItem, items/<kind>.cfg in the mod's
	// item) replaces it; with neither it is a small box. The first shape declared for a kind is kept.
	ItemKindHandle ItemKind( const std::string& name, const ItemShape& shape );
	// While a player holds an item of `kind`, `pack`'s layers play instead of the player's own of the
	// same names: a bat that changes how its holder stands and walks, wherever the bat came from.
	// A mod's own SwapLayer on such a layer wins while it lasts (a crouch over a carry).
	void ItemLayers( ItemKindHandle kind, AnimPackHandle pack );
	// The items of this mod's look: every scene of its client project whose root is a CbItem
	// (baked to items/<kind>.cfg). Each becomes an item kind of this mod, with what its scene says:
	// its body and properties (as for any kind), the animation pack it brings while it is held,
	// and the socket it hangs in while it is put away ("holster", for the inventory mod). So a
	// mod's rules can be about "whatever my look has that says gun.damage", and a new item is a
	// new scene: nothing is compiled.
	struct OwnItem
	{
		ItemKindHandle kind;
		LookItem look;
	};
	std::vector<OwnItem> ItemsOfLook();
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
	// A character in this mod's client item: a CbCharacter scene of its client project, baked into
	// characters/<name>/ (its skeleton, its state machine, its hitboxes, its capsule). A body may be
	// it (Context::SetCharacter); players download it with the mod's item. Name it like the mod's
	// other names ("zoo.dragon"). The server's own character is 0; a name it already has is it.
	CharacterHandle Character( const std::string& name );
	// A set of motions in this mod's client item: what its players can do besides walking and
	// jumping (a dash, a double jump), run by every simulation, so a player's own are predicted.
	// Name it like the mod's other names ("dash.moves"). The mod declares the actions, fields and
	// events the motions name; who may use one is a field its condition reads.
	MotionsHandle Motions( const std::string& name );
	// An item kind's own key ("H" for a grenade): the action "<kind>.use", which uses the first copy
	// a player carries where it is (ItemShape::useAction). The player's own key wins (the Keys page).
	// An item's scene says it (CbItem.use_key).
	void ItemUseKey( ItemKindHandle kind, const std::string& key );
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
	void SetLookItems( std::function<std::vector<LookItem>( const std::string& mod )> read )
	{
		m_lookItems = std::move( read );
	}

private:
	std::vector<std::string> m_textChannels;
	std::vector<EntityTemplate> m_templates;
	const std::map<std::string, std::string>* m_options = nullptr;
	ModSchema m_schema;
	std::vector<std::string> m_errors;
	std::string m_mod;
	std::function<std::vector<LookItem>( const std::string& mod )> m_lookItems;
	std::map<int, std::vector<ItemShape::FieldDefault>> m_itemFieldDefaults;
	std::vector<bool> m_shapeDeclared; // per item kind: a mod gave it a shape
	std::vector<std::vector<std::string>> m_itemMods; // per item kind: the mods that declared it
	std::map<std::pair<int, std::string>, float> m_itemProperties;
	std::map<int, int> m_itemLayers; // item kind -> animation pack
	std::map<int, uint8_t> m_itemUseActions; // item kind -> the action bit of its own key
	int m_slots = 0;

public:
	const std::map<int, int>& ItemLayersByKind() const
	{
		return m_itemLayers;
	}
	const std::map<int, uint8_t>& ItemUseActions() const
	{
		return m_itemUseActions;
	}
	// What a new copy of each kind starts with in its own fields (declared by its CbItem).
	const std::map<int, std::vector<ItemShape::FieldDefault>>& ItemFieldDefaults() const
	{
		return m_itemFieldDefaults;
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

	// How the player in `slot` moves before this tick: the server's parameters (its options, its
	// character's values), with what mods set for that player.
	float Move( PlayerSlot slot, MoveParam param ) const
	{
		return m_sim.PlayerMove( slot )[param];
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
	// Any body's controller state (a player's, or one a mod drives); null for a body nobody drives.
	const Character* BodyCharacter( uint32_t netId ) const
	{
		const flecs::entity e = m_sim.FindEntity( netId );
		return e.is_valid() ? e.try_get<Character>() : nullptr;
	}
	const Character* PlayerCharacter( PlayerSlot slot ) const
	{
		return m_sim.PlayerCharacter( slot );
	}
	// What the player's body is posed from: is it aiming, which stance each layer has. A mod that
	// shares a stance with another reads here whether its own is still set.
	AnimStateCopy PlayerAnim( PlayerSlot slot ) const
	{
		return m_sim.EntityAnimState( m_sim.PlayerNetId( slot ) );
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
	// Using items (on a server with slots). A copy is used from the hand (its slot selected, then the
	// use button), by its slot's key where it is (CbItem.slot_key_uses: a skill on the hotbar), or by
	// its kind's own key wherever it is carried (CbItem.use_key: the selected copy, else the lowest
	// slot's). Every copy is its own: two bats in two slots are two bats.
	// How long a use lasts is the kind's (the item property "use": a tool while the button is down,
	// a press, or a press and held).
	//   Used      the player used one of that kind on this tick (the press)
	//   Using     one of that kind is in use this tick (automatic fire, a jetpack)
	//   UsedItem  which copy: Used (held false) or Using (true); 0 for none
	// The simulation records "<kind>.used" for the same tick, when a mod declared that event: a
	// look reacts to it at once on the player's own screen.
	bool Used( PlayerSlot slot, ItemKindHandle kind ) const;
	bool Using( PlayerSlot slot, ItemKindHandle kind ) const;
	uint32_t UsedItem( PlayerSlot slot, ItemKindHandle kind, bool held = false ) const;
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
	// A wall that breaks (a map's CbBreakable, whole, or a piece of it a blast left standing): what
	// Break takes.
	bool IsBreakable( uint32_t netId ) const;
	// How fast a body moves (its centre, m/s): a prop, an item lying or flying, a player. Zero for
	// anything static or gone.
	b3Vec3 EntityVelocity( uint32_t netId ) const;
	// Everything loose within `radius` of `point`, nearest first: props, items lying in the world,
	// ragdolls (what Push moves). Not players, and not the map. For a blast, a magnet, a wind.
	std::vector<NearBody> BodiesNear( b3Vec3 point, float radius ) const;
	// What breaks (a map's CbBreakable: whole, or what a blast left standing of it) with a piece's
	// centre within `radius` of `point`, nearest first: one each, to give Break (the whole wall, or
	// its standing piece nearest the point).
	std::vector<uint32_t> BreakablesNear( b3Vec3 point, float radius ) const;

	// Entities of a kind, in NetId order (creation order).
	std::vector<uint32_t> Props() const;
	// Only props a player spawned (the level's own props are left out).
	std::vector<uint32_t> SpawnedProps() const;
	std::vector<uint32_t> Ragdolls() const;

	// Mod events the previous tick recorded (any mod's): how one mod reacts to another's news
	// ("combat.killed") without knowing it. At most the ring's size per tick. Markers in the
	// character's animations record events too, from the player they played on.
	std::vector<ModEventRecord> RecentEvents() const;
	// The players who sent `intent` this tick (a click on a screen), and its numbers. Each is here
	// once, on the tick it arrived.
	std::vector<IntentSent> Intents( IntentHandle intent ) const;
	// What players typed since the last tick, into channels the mods declared, in the order it
	// arrived. Nothing is said to anyone until a mod says it (Say).
	const std::vector<TypedText>& Typed() const;
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
	void SetNavMesh( const NavMesh* nav )
	{
		m_nav = nav;
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

	// A line on a channel, to everyone (`to` kNoSlot) or one player, from a player or (`from`
	// kNoSlot) from nobody: the server's own words. Words only: it never touches the simulation,
	// and recordings do not keep it. Printable characters, at most net::kMaxText bytes.
	void Say( const std::string& channel, const std::string& text, PlayerSlot from = kNoSlot, PlayerSlot to = kNoSlot );

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
	// A breakable comes apart (CommandType::Break; `target` from BreakablesNear): its pieces within
	// `radius` of `blast` (0: all of them) fly away from it, a piece at the blast at `speed` m/s,
	// slower with distance; the rest stand, but any that nothing holds up any more falls. What goes
	// stays `seconds` (0: for good).
	void Break( uint32_t target, b3Vec3 blast, float speed, float seconds = 0.0f, float radius = 0.0f );
	void Push( uint32_t target, b3Vec3 point, b3Vec3 vector, ImpulseMode mode );
	// ragdollLifetimeTicks 0 keeps it; ragdollCap 0 means no limit.
	void Kill( uint32_t target, bool ragdoll, b3Vec3 hitPoint = {}, b3Vec3 hitVelocity = {}, uint32_t ragdollLifetimeTicks = 0,
			   uint32_t ragdollCap = 0 );
	void Respawn( uint32_t target );
	// Tosses a living character (M249): its ragdoll flies at its velocity and `velocity` more, and
	// it is down, not dead (nothing of it changes: its fields, its items), until it gets up where
	// the ragdoll lies, once that has been still a moment, or after `mostSeconds` (0: 5). Flying,
	// the ragdoll breaks a breakable it hits at `breakSpeed` m/s or more (0: none), a hole of radius
	// `holeRadius` m at 30 m/s, wider the faster, as a thrown item's. Killed while down, it stays the
	// player's ragdoll (or goes, without one); respawned, it goes. IsDown says whether it is.
	void Toss( uint32_t target, b3Vec3 velocity, float breakSpeed = 0.0f, float holeRadius = 1.0f, float mostSeconds = 0.0f );
	bool IsDown( uint32_t target ) const;
	void RespawnAt( uint32_t target, b3Vec3 position, float yaw );
	// A frozen player ignores movement and jump (mods decide what else a freeze means for them).
	void Freeze( uint32_t target, bool frozen );
	// How one player moves (move_params.h): its walk speed, jump, gravity and the rest. The value
	// is clamped to the parameter's range and stays until ResetMove gives the server's back (a
	// respawn keeps it: only the mod lets it go). Not predicted: like every command it reaches the
	// player's own simulation with the frame.
	void SetMove( uint32_t target, MoveParam param, float value );
	void ResetMove( uint32_t target, MoveParam param );
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
	// (On a server with slots the item goes into a slot, and its kind's "holster" is where it hangs.)
	void GiveItem( uint32_t holder, ItemKindHandle kind, SocketHandle holster = {} );
	// Puts a held item away (in `holster`, or out of sight), or takes a carried one in use into
	// `socket`. HoldItem does nothing when that socket has an item in use: stow that one first, in
	// the same tick. Stowed items are in no hand: HeldItem, item layers and state machines do not see
	// them; CarriedItems does.
	void StowItem( uint32_t item, SocketHandle holster = {} );
	void HoldItem( uint32_t item, SocketHandle socket );
	// Slots: how many players have (0: this server has none), which one a player has selected
	// (-1: empty hands), and the item in one (0: empty).
	int SlotCount() const;
	int SelectedSlot( PlayerSlot player ) const;
	uint32_t SlotItem( PlayerSlot player, int slot ) const;
	// Containers (chests): the entities that have slots of their own, in NetId order; one's item in
	// a slot (0: empty); and an item of `kind` put in one (onto a stack of its kind with room, else
	// the first free slot; a full one takes nothing), on the next tick.
	std::vector<uint32_t> Containers() const;
	int ContainerSlots( uint32_t container ) const;
	uint32_t ContainerItem( uint32_t container, int slot ) const;
	void PutInContainer( uint32_t container, ItemKindHandle kind );
	// A template of the map's (or a mod's: Declarations::ContainerTemplate) by its name; -1: none.
	int FindTemplate( const std::string& name ) const;
	// Everything the player in `slot` carries, in use and stowed, in NetId order.
	std::vector<CarriedItem> CarriedItems( PlayerSlot slot ) const;
	// Plays `pack`'s layer named `layer` ("FullBody") instead of the player's own, from its start; the
	// layer's name is the character's (its AnimationTree's). Does nothing when the character has no
	// such layer. The pose follows it everywhere, the server's hit tests too.
	void SwapLayer( uint32_t target, AnimPackHandle pack, const std::string& layer );
	// The player `target` is `character` from the next tick: its capsule (feet where they were), its
	// state machine (its layers start over), its hitboxes, its look. An invalid handle: the server's
	// own character again.
	void SetCharacter( uint32_t target, CharacterHandle character );
	// A body of `character` nobody drives, its feet at `feet`, facing `yaw` (radians): drawn as its
	// character, standing; a physics body of its capsule (it falls, it is pushed, it settles).
	// `possessor`: a slot that drives it at once, as Possess (kNoSlot: nobody). An invalid handle:
	// the server's own character.
	void SpawnCharacter( CharacterHandle character, b3Vec3 feet, float yaw, PlayerSlot possessor = kNoSlot );
	// The player in `slot` drives `body` (one of IdleBodies) from the next tick; the body it drove
	// stands where it is, nobody's, what it held put away (a dead one goes). Nothing happens for a
	// body someone drives.
	void Possess( PlayerSlot slot, uint32_t body );
	// The characters' bodies nobody drives (NetIds).
	std::vector<uint32_t> IdleBodies() const;
	// The mod drives `body` (a character's body nobody drives, or a loose prop: CanPossess) with
	// `input`, as a player's keys would drive it: from the next tick, and with that input until it
	// says otherwise (so call it when the input changes, or every tick: only a change is sent). Its
	// motions, its state machine, its hit tests are a player's; every simulation predicts it with
	// its last input. An NPC is a body a mod drives.
	void Drive( uint32_t body, const PlayerInput& input );
	// It is driven no more: it stands where it is, nobody's (a prop: a loose body again).
	void StopDriving( uint32_t body );
	// Whether a mod drives `body` (and with what, if `input`).
	bool DrivenByMod( uint32_t body, PlayerInput* input = nullptr ) const;
	// The bodies mods drive (NPCs), by NetId.
	std::vector<uint32_t> ModDrivenBodies() const;

	// Finding the way (on the server's navigation mesh: where its own character can walk, made from
	// what does not move when the game starts). The corners of a way from `from` to `to` (feet
	// heights): the first where `from` is, the last `to` or the nearest point to it there is a way
	// to. False when either end is far from anywhere to walk.
	bool FindPath( b3Vec3 from, b3Vec3 to, std::vector<b3Vec3>& corners ) const;
	// The nearest point one can walk to (within a few metres of `point`).
	bool NearestWalkable( b3Vec3 point, b3Vec3& out ) const;
	// The input that walks `body` toward `target` (its feet there): facing it, forward; nothing
	// once within `near` metres. For Drive: an NPC following a path drives toward its next corner.
	PlayerInput WalkToward( uint32_t body, b3Vec3 target, float near = 0.3f, bool sprint = false ) const;
	// Whether a slot may drive `body` (Possess): a character's body nobody drives, or a loose prop
	// (not an item, a ragdoll, a container or anything static).
	bool CanPossess( uint32_t body ) const
	{
		return m_sim.Possessable( m_sim.FindEntity( body ) );
	}
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
	void SetTexts( const std::vector<TypedText>* typed, std::vector<SaidText>* said )
	{
		m_typed = typed;
		m_said = said;
	}
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
	// Who threw it, for an item a motion launched (a CbLaunch): the player's NetId, or 0 (nobody
	// did, or it was picked up since).
	uint32_t ThrownBy( uint32_t item ) const;
	// What items of a kind ran into on the tick before this one: each with the item, what it hit
	// (a player, a prop, a piece of the map), how fast they met and where. The simulation keeps
	// the hardest few impacts of a tick, so a soft touch in a busy scene may not be among them.
	struct ItemHit
	{
		uint32_t item = 0;
		uint32_t other = 0;
		float speed = 0.0f;
		b3Vec3 point = {};
	};
	std::vector<ItemHit> Hits( ItemKindHandle kind ) const;
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
	const std::vector<TypedText>* m_typed = nullptr;
	std::vector<SaidText>* m_said = nullptr;
	const std::map<int, int>* m_itemLayers = nullptr;
	// Which layer of the character `layer` names, and whose slot `target` is (-1: neither a slot nor
	// a player).
	int LayerIndex( const std::string& layer ) const;
	int SlotOfTarget( uint32_t target ) const;
	HitTester* m_hits = nullptr;
	const NavMesh* m_nav = nullptr;
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
	// Runs when cb_server is started without --mods. False for a mod whose folder has a "disabled"
	// file: it is compiled in, and runs only when --mods names it.
	bool onByDefault;
};

} // namespace cb::mods
