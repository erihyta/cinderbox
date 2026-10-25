#pragma once

// What the server's mods declared: the names behind board slots, mod event types and action bits.
//
// The simulation stores numbers (board slot 3, event type 1, action bit 0). The schema is how
// everyone else puts names on them: the server builds it from its mods at startup and sends it to
// every client on join, and presentation looks fields, events and actions up by name. A client
// therefore never needs to know what a pistol is to show "pistol.ammo" or play "pistol.fired".
//
// It is not simulation state and is never hashed: two servers with different mods can run the same
// build, and a client simply shows what its server declared. One part feeds the simulation: the
// character's state machine, which travels here (like the map in the welcome) so that every client
// runs exactly the server's.

#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cb
{

enum class BoardType : uint8_t
{
	Int = 0,
	Float = 1, // stored as the float's bits
	Bool = 2,
};

enum class BoardScope : uint8_t
{
	Entity = 0, // one value per entity (Blackboard component)
	Global = 1, // one value for the whole game (SimGlobals::board)
	// One value per player that only that player is sent (a role, a hand of cards). It is never in
	// the simulation: not in its state, its hash, its frames or a recording. The server keeps it
	// and gives it to its owner alone, so no other client has it to find.
	Private = 2,
};

struct BoardField
{
	std::string name; // "pistol.ammo": mod name, a dot, then the field
	BoardType type = BoardType::Int;
	BoardScope scope = BoardScope::Entity;
	uint16_t slot = 0;

	bool operator==( const BoardField& ) const = default;
};

struct ModAction
{
	std::string name; // "fire"
	uint8_t bit = 0;  // bit in PlayerInput::actions
	// Suggested binding, for clients to use until the player changes it: a key name as Godot spells
	// it ("F", "R", "1", "Space"), or "MouseLeft" / "MouseRight" / "MouseMiddle".
	std::string key;

	bool operator==( const ModAction& ) const = default;
};

// A mod's client content, as a workshop item: the look players subscribe to (bindings, HUD,
// meshes, sounds). The server never sends it; it only says which item, and clients must have that
// exact content (the SHA-256 of the item's pack) to join.
struct ModItem
{
	std::string mod;	// "pistol"
	std::string sha256; // 64 lowercase hex digits

	bool operator==( const ModItem& ) const = default;
};

// An animation pack a mod provides: AnimationTree layers a player's own can be swapped for ("FullBody"
// for a crouch walk). Baked into the mod's workshop item under anim/<name>/; the graph travels here
// so every simulation runs the server's.
// A character a body may be (Simulation's CharacterKind): a scene authored in the editor (a
// CbCharacter), baked into the item it ships in. Its state machine and its capsule travel here, so
// every simulation runs the same; how it looks is the item's. Character 0 is the server's own,
// every player's unless a mod says another.
inline constexpr float kDefaultCapsuleRadius = 0.3f;
inline constexpr float kDefaultCapsuleHalfHeight = 0.5f; // the centre to a sphere's centre
inline constexpr size_t kMaxCharacters = 64;
struct CharacterInfo
{
	std::string name;  // res://characters/<name>/: the game's own, a character item's, or a mod's
	std::string mod;   // the mod whose item it is in ("": the server's own character)
	std::string graph; // its state machine (graph.cfg text); "" for the procedural rig
	float radius = kDefaultCapsuleRadius;
	float halfHeight = kDefaultCapsuleHalfHeight;
	// How it moves, where its scene says so (CbCharacter.movement): movement parameters (move_params.h)
	// and their values, laid over the server's for its bodies. [0]'s are in the server's already.
	std::vector<std::pair<uint8_t, float>> move;
	// Whether its bodies may be seen through their eyes (a first-person camera in its scene:
	// AnimSet::FirstPerson). Without, a first-person view is the third-person one, everywhere.
	bool firstPerson = true;

	bool operator==( const CharacterInfo& ) const = default;
};

struct AnimPackInfo
{
	std::string mod;
	std::string name;
	std::string graph; // graph.cfg text

	bool operator==( const AnimPackInfo& ) const = default;
};

// A set of motions a mod provides (sim/motions.h): what its players can do besides walking and
// jumping (a dash, a double jump). Baked into the mod's workshop item as motions/<name>.cfg; the
// text travels here so every simulation runs the server's.
struct MotionSetInfo
{
	std::string mod;
	std::string name;
	std::string text; // motions/<name>.cfg; empty: the item did not have it, and the set does nothing

	bool operator==( const MotionSetInfo& ) const = default;
};

// The body of an item lying in the world. The item's frame is its grip (held in a socket's frame:
// the grip at the origin, pointing along -Z); `center` is where the shape's centre is in it.
// How an item kind is used (ItemShape::use, the item property "use").
inline constexpr uint8_t kItemUseInHand = 0; // its slot's key selects it; the use button uses it
inline constexpr uint8_t kItemUsePress = 1;	 // its slot's key uses it where it is, once a press
inline constexpr uint8_t kItemUseHold = 2;	 // the same, and it is in use while the key is held
// Which hand a carried item is in (ItemShape::hand, the item property "hand").
inline constexpr uint8_t kItemHandSelected = 0; // the slots' hand, while its slot is the selected one
inline constexpr uint8_t kItemHandLeft = 1;		// the left hand while it is carried, when that hand is free

// One shape of an item's body of several (its scene's CollisionShape3D nodes), in the frame the
// item is carried in.
struct ItemPart
{
	uint8_t kind = 0; // ShapeKind: 0 box, 1 sphere, 2 capsule, 3 hull
	Float3 half;	  // as Shape's: a box's half size; a radius in x; a capsule's half segment in y
	Float3 center;
	float turn[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // x y z w
	std::vector<Float3> points;					// a hull's, in the part's own frame

	bool operator==( const ItemPart& ) const = default;
};

// Parts an item's body may have, and points one of its hulls may have.
inline constexpr size_t kItemPartLimit = 16;
inline constexpr size_t kItemHullPointLimit = 256;

struct ItemShape
{
	uint8_t kind = 0; // ShapeKind: 0 box (half extents), 1 sphere (radius = half.x)
	Float3 half = { 0.05f, 0.05f, 0.15f };
	Float3 center = { 0.0f, 0.0f, -0.15f };
	float mass = 1.0f;
	// How it grips and how it bounces while it lies in the world, as for any body.
	float friction = 0.6f;
	float restitution = 0.0f;
	// Rolling resistance and drag, as any body's (0: rolls on, nothing slows it in the air).
	float rolling = 0.0f;
	float drag = 0.0f;
	// How the body is turned in the item's frame (x y z w): the item is carried as its scene's
	// carrying grip says, and its body lies as the scene has it, whichever way that grip is turned.
	float turn[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	// A body of several shapes, each where it is in the item's frame; then the body sits at the
	// item's frame (center 0, no turn) and `kind` / `half` are the box that holds them. Empty: the
	// one box or sphere above.
	std::vector<ItemPart> parts;
	// Where the other hand holds it, in the item's frame (the hand that carries it is at the origin):
	// 0 nowhere (a one-handed item); 1 the other hand's wrist goes to `gripPosition`; 2 and its hand
	// takes `gripRotation` (x y z w), as a hand carrying an item placed there would be turned; 3 the
	// other hand stays where the animation has it relative to the carrying hand (no place is given).
	uint8_t grip = 0;
	Float3 gripPosition;
	float gripRotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	// Slots: the slot this kind would go to when a player comes to carry one (1 is the first), if it
	// is free; else, or with 0, the first free one. And the socket it hangs in while it is not the
	// selected one (255: out of sight).
	uint8_t slot = 0;
	uint8_t holster = 255;
	// How it is used (kItemUse*), from the hand (its slot selected, then the use button) or by its
	// own key (`useAction`) wherever it is carried: 0 the use button is down while it is in use (a
	// gun, a bat: a tool). 1 once a press: a consumable, an ability (a grenade, a dash, a hook's
	// throw). 2 a press, and in use while the button or key is held (a jetpack). Each copy is used on
	// its own: a slot is a place, not a kind's. The simulation records `usedEvent` (a mod event,
	// schema order; 0xFFFF: none) at the player, with the item as the other entity, on the press.
	uint8_t use = 0;
	uint16_t usedEvent = 0xFFFF;
	// Its own key: a mod action's bit (`<kind>.use`, CbItem.use_key) that uses the first copy the
	// player carries (the selected one, else the lowest slot's) where it is, without taking it out.
	// 255: none, it is used from the hand only.
	uint8_t useAction = 255;
	// 1: its slot's key uses it where it is (a press, or held for one used while held), and does not
	// select the slot: a skill on a Roblox-style hotbar. 0: the key selects the slot, as for any item.
	uint8_t slotKeyUses = 0;
	// When a copy's time is up (one launched with seconds, removed then): `expiredEvent` (a mod event,
	// "<kind>.expired" where a mod declares it; 0xFFFF: none), with the item as the first entity,
	// whoever threw it as the second, at where it was. A fused grenade goes off on it.
	uint16_t expiredEvent = 0xFFFF;
	// It breaks walls (CbItem.break_speed): lying in the world, a copy that meets a breakable (a
	// map's CbBreakable, or a piece of one left standing) at `breakSpeed` m/s or faster knocks a hole
	// in it there, in the same step, so every simulation sees it at once: `breakHole` metres across
	// (its radius) at 30 m/s, wider the faster; the pieces flung its way; it goes on with
	// `breakThrough` of the velocity it had (0: it bounces off as it would). `breakEvent`
	// ("<kind>.smash" where a mod declares it; 0xFFFF: none) is said there, the item first, the wall
	// second. 0: it breaks nothing.
	float breakSpeed = 0.0f;
	float breakHole = 0.0f;
	float breakThrough = 0.0f;
	uint16_t breakEvent = 0xFFFF;
	// Which hand it is in (kItemHand*): the slots' hand while its slot is selected, or the left hand
	// for as long as it is carried (a torch, a shield): out unless what is selected has its other
	// hand on a marker (grip 1, 2), or another such item of a lower slot is there first.
	uint8_t hand = kItemHandSelected;
	// 1: lying in the world it is a bullet: moved with continuous collision against what moves too
	// (a crate, a door), not only against the level, so a small fast throw does not pass a thin one
	// between two steps. Costs a little more a step; for kinds that are thrown fast.
	uint8_t bullet = 0;
	// How many of it a slot holds, one stack (0, 1: one). An item given to a player who has a stack
	// of its kind with room goes onto it; one thrown from a stack is split off it.
	uint8_t stack = 0;
	// Its own fields (declared by its CbItem: the rifle's "gun.rounds"): what a new copy starts with,
	// as board slots of entity fields and their raw values. A copy keeps its own from then on.
	struct FieldDefault
	{
		uint16_t slot = 0;
		int32_t raw = 0;
		bool operator==( const FieldDefault& ) const = default;
	};
	std::vector<FieldDefault> fieldDefaults;

	bool operator==( const ItemShape& ) const = default;
};

struct ModSchema
{
	std::vector<std::string> mods; // names of the mods the server runs, for display
	std::vector<ModItem> items;	   // the client items those mods need, and the character's
	// The characters bodies may be: [0] the server's own, every player's unless a mod gives another;
	// then those mods declare. A workshop character is also in `items`; one that ships with the game
	// is not. [0]'s name is empty for the procedural rig. Never empty.
	std::vector<CharacterInfo> characters = std::vector<CharacterInfo>( 1 );
	std::vector<BoardField> fields;
	std::vector<std::string> events; // index = ModEventRecord::type
	// Intents mods declare ("shop.buy"): a player's input carries one as kFirstModIntent + its index.
	std::vector<std::string> intents;
	std::vector<ModAction> actions;
	// Animation layers and stances, declared by mods: names. A mod sets a stance on a layer; a
	// character's state machine reads stances by name ("pistol") in its conditions and weights.
	// AnimState stores their indices.
	std::vector<std::string> layers;
	std::vector<std::string> stances;
	// Kinds of items mods spawn ("melee.bat"; a look per kind in the mod's client item), their
	// bodies when they lie in the world (same index), and the sockets they are held in: the hands
	// every character has, then any a mod names (a character provides it as a CbSocket node, or the
	// item is not drawn there).
	std::vector<std::string> itemKinds;
	std::vector<ItemShape> itemShapes;
	std::vector<std::string> sockets = { "RightHand", "LeftHand" };
	std::vector<AnimPackInfo> animPacks;
	std::vector<MotionSetInfo> motionSets;

	bool operator==( const ModSchema& ) const = default;

	const BoardField* FindField( const std::string& name ) const;
	// How many fields a scope has: one past the highest slot declared in it.
	int FieldCount( BoardScope scope ) const;
	// -1 when not declared.
	int FindEvent( const std::string& name ) const;
	int FindIntent( const std::string& name ) const;
	const ModAction* FindAction( const std::string& name ) const;
	// Bit mask of the named action, 0 when not declared.
	ActionBits ActionMask( const std::string& name ) const;
	// -1 when not declared.
	int FindLayer( const std::string& name ) const;
	int FindStance( const std::string& name ) const;
	int FindItemKind( const std::string& name ) const;
	int FindSocket( const std::string& name ) const;
	int FindCharacter( const std::string& name ) const;
};

inline constexpr size_t kMaxSchemaName = 64;
inline constexpr size_t kMaxSchemaText = 1u << 20;

// True for 64 lowercase hex digits.
bool IsSha256( const std::string& hex );

void EncodeSchema( const ModSchema& schema, std::vector<uint8_t>& out );
// Rejects anything out of range (slots, bits, name lengths), so a client can trust what it read.
bool DecodeSchema( const uint8_t* data, size_t size, ModSchema& out );

// Board values are int32 on the wire and in the state; these are the float conversions.
inline int32_t BoardFromFloat( float value )
{
	int32_t bits;
	std::memcpy( &bits, &value, sizeof( bits ) );
	return bits;
}

inline float BoardToFloat( int32_t bits )
{
	float value;
	std::memcpy( &value, &bits, sizeof( value ) );
	return value;
}

} // namespace cb
