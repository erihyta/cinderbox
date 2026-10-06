#pragma once

// Types shared by server, client and tools. Everything that crosses the network or feeds the
// simulation is integer-quantized so it is identical on every machine.

#include "move_params.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cb
{

inline constexpr int kMaxPlayers = 64;
using PlayerSlot = uint8_t;

// Buttons the engine itself understands. Everything a game adds (fire, reload, spawn a prop) is a
// mod action instead: a bit in PlayerInput::actions whose meaning only the server's mods know.
enum InputButton : uint8_t
{
	BtnJump = 1 << 0,
	BtnSprint = 1 << 1,
};
inline constexpr uint8_t kEngineButtons = BtnJump | BtnSprint;

// Slots: what a player carries, in numbered places (0 .. SimConfig::slots - 1). The slot that is
// selected has its item in the hand; the others' items are stowed. Which item is in which slot is
// on the item (HeldItem::slot); which slot is selected is on the player (Slots).
inline constexpr int kMaxSlots = 36;
inline constexpr uint8_t kNoSlot = 255;

// What a player asks of its slots, in its input: so every simulation runs it, the player's own
// prediction included. An intent is carried out once, on the tick PlayerInput::intentSeq changes.
enum class SlotIntent : uint8_t
{
	None = 0,
	Select = 1, // intentA: the slot. The selected slot again, or kNoSlot: its item is put away, empty hands
	Move = 2,	// intentA to intentB: the two slots trade what they hold (either may be empty)
	Drop = 3,	// intentA: the slot, or kNoSlot for the selected one. Its item is thrown out
};
inline constexpr uint8_t kLastSlotIntent = uint8_t( SlotIntent::Drop );

// Mod actions per player. The server tells clients which bit is which (and the default key for
// it) when they join; the simulation never looks at them.
inline constexpr int kMaxActions = 16;

// Camera pitch is limited to just short of straight up or down (full turn = 65536).
inline constexpr int16_t kMaxCameraPitch = 16000;

// Which camera a player looks through: the player's choice. The server's mods read it to know
// where the player's line of sight starts (mods::Context::ViewPosition), so that what is under the
// crosshair is what is aimed at; and in first person the character faces where the camera looks
// (anim_controller.h FacesCamera), as it does with a weapon out.
enum class ViewMode : uint8_t
{
	ThirdPerson = 0,   // behind the player, orbiting the point above its body
	FirstPerson = 1,   // from the character's eye height, on its mover (it does not follow the animation)
	ShoulderRight = 2, // third person, moved sideways by kShoulderOffset
	ShoulderLeft = 3,
};
inline constexpr uint8_t kViewModes = 4;
inline constexpr float kShoulderOffset = 0.45f; // metres to the side of the pivot
// The eye, from the head joint: ahead along the look (metres), and above the joint.
inline constexpr float kEyeAhead = 0.14f;
inline constexpr float kEyeUp = 0.09f;
// The point a third-person camera orbits, above the character's centre (metres).
inline constexpr float kViewPivotHeight = 0.4f;

// One player's input for one tick. 10 bytes, no padding.
struct PlayerInput
{
	int8_t moveRight = 0;	 // [-127, 127]
	int8_t moveForward = 0;	 // [-127, 127]
	uint16_t cameraYaw = 0;	 // full turn = 65536, 0 looks down +Z
	int16_t cameraPitch = 0; // full turn = 65536, positive looks up, within +/- kMaxCameraPitch
	uint16_t actions = 0;	 // mod action bits (held state)
	uint8_t buttons = 0;	 // InputButton bits (held state; the sim detects edges)
	uint8_t view = 0;		 // ViewMode
	uint8_t intent = 0;		 // SlotIntent, carried out when intentSeq changes
	uint8_t intentA = 0;
	uint8_t intentB = 0;
	uint8_t intentSeq = 0;	 // counts the player's intents (it wraps)

	bool operator==( const PlayerInput& ) const = default;
};
static_assert( sizeof( PlayerInput ) == 14, "PlayerInput has padding" );

enum class PlayerEventType : uint8_t
{
	Join = 1,
	Leave = 2,
};

struct PlayerEvent
{
	PlayerEventType type = PlayerEventType::Join;
	PlayerSlot slot = 0;

	bool operator==( const PlayerEvent& ) const = default;
};

struct Float3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;

	bool operator==( const Float3& ) const = default;
};

// --- Commands ------------------------------------------------------------------------------------
//
// What the server's gameplay mods decide, in a form every simulation can apply. Mods run only on
// the server; they never touch the simulation directly. Instead each authoritative frame carries
// the commands they produced, and clients apply them exactly like inputs, so the world stays
// deterministic while the rules stay on the server.
//
// A command names its target by NetId, by player slot with SlotTarget(), or as the item a player
// holds in a socket with ItemTarget(). Commands whose target does not exist (any more) are
// ignored, identically everywhere.

enum class CommandType : uint8_t
{
	// target (0: the global board), index = board slot, value.
	SetField = 1,
	// index = event type, target = entity A, other = entity B, value, a = point, b = vector.
	Event = 2,
	// mode = shape kind, index = yaw (full turn = 65536), target = owner (0: the level),
	// other = lifetime in ticks (0: forever), value = template index (-1: plain shape),
	// a = position, b = velocity, c = half extents.
	SpawnProp = 3,
	// target.
	Destroy = 4,
	// target, mode = ImpulseMode, a = world point, b = impulse or velocity change.
	Impulse = 5,
	// target (a player), mode = 1 leaves a ragdoll, other = its lifetime in ticks (0: forever),
	// value = most ragdolls kept (oldest go first; 0: no limit), a = hit point,
	// b = velocity change of the body part nearest the hit point.
	Kill = 6,
	// target (a player), mode = 1 places it at a with yaw index, otherwise at its spawn point.
	Respawn = 7,
	// target (a player), mode = 1 freezes it (it stands, falls and can be pushed, but its movement
	// and jump inputs are ignored), 0 releases it.
	Freeze = 8,
	// target (a player), mode = 1 turns its aim chain toward where it looks, 0 lets it go.
	Aim = 9,
	// target (a player), mode = 1: the body faces where the camera looks; 0: it turns toward where
	// it walks (freelook, the default).
	Facing = 10,
	// target (a player), index = layer (schema order), value = stance + 1, or 0 for none.
	Stance = 11,
	// target (the player who holds it), index = item kind (schema order), mode = socket (schema
	// order). A new entity with its own board, drawn in that socket; what that socket held is
	// dropped at the holder's chest. Destroy removes it; it also goes when its holder leaves.
	// value = 1: it is given stowed instead (mode: its holster socket, or kNoSocket), and nothing drops.
	// target 0: the item lies in the world instead, at a (its grip) turned by c (a unit
	// quaternion's x, y, z; w >= 0 follows), moving at b.
	SpawnItem = 12,
	// target (a player), index = the character's layer (graph order), value = source: 0 plays its
	// own layer again, n plays animation pack n-1's layer of the same name. The layer starts over.
	SwapLayer = 13,
	// target (a held item), a = its grip, c = its rotation (as SpawnItem), b = velocity: it leaves
	// the hand and lies in the world, with a body of its kind's shape.
	DropItem = 14,
	// target (a player), other = an item lying in the world, mode = socket: the player holds it.
	// Nothing happens when that socket is taken, or the item is held already.
	// value = 1: it is taken stowed (mode: its holster socket, or kNoSocket), whatever the hands hold.
	PickUpItem = 15,
	// target (a held item), mode = socket, value = 1 stows it (mode: its holster socket, or
	// kNoSocket), 0 takes it in use into that socket. Nothing happens when it is not held, or the
	// socket already has an item in use (stow that one first, in the same frame).
	MoveItem = 16,
	// target (a player), index = MoveParam, mode = 1: a.x is that player's value for the parameter
	// (clamped to its range) until mode = 0 gives it back to the server's (SimConfig::move).
	SetMove = 17,
};
inline constexpr uint8_t kLastCommandType = uint8_t( CommandType::SetMove );

enum ImpulseMode : uint8_t
{
	ImpulseLinear = 0,	// b is an impulse in N*s
	ImpulseVelocity = 1, // b is a change of velocity in m/s (mass independent)
};

inline constexpr uint32_t kSlotTargetBit = 0x80000000u;
inline constexpr uint32_t SlotTarget( PlayerSlot slot )
{
	return kSlotTargetBit | uint32_t( slot );
}

// The item the player in `slot` holds in `socket` (schema order): an item a command spawned this
// very tick can be addressed before anyone knows its NetId.
inline constexpr uint32_t kItemTargetBit = 0x40000000u;
inline constexpr uint32_t ItemTarget( PlayerSlot slot, uint32_t socket )
{
	return kItemTargetBit | ( ( socket & 0xFFu ) << 8 ) | uint32_t( slot );
}

// Sockets every character has (the first in every schema); characters may add their own.
inline constexpr uint32_t kSocketRightHand = 0;
inline constexpr uint32_t kSocketLeftHand = 1;

// Per-entity and global board sizes (see Blackboard in components.h): how many field names all the
// mods of a server can declare per scope. Part of the snapshot layout: changing it changes every
// state hash (tests/reference_hashes.txt) and the protocol version.
inline constexpr int kBoardSlots = 32;
// Most commands one frame can carry.
inline constexpr size_t kMaxCommandsPerFrame = 1024;

struct SimCommand
{
	CommandType type = CommandType::SetField;
	uint8_t mode = 0;
	uint16_t index = 0;
	uint32_t target = 0;
	uint32_t other = 0;
	int32_t value = 0;
	Float3 a;
	Float3 b;
	Float3 c;

	// Bitwise, so a NaN never makes a frame unequal to itself (which would roll back forever).
	bool operator==( const SimCommand& o ) const
	{
		return std::memcmp( this, &o, sizeof( SimCommand ) ) == 0;
	}
};
static_assert( sizeof( SimCommand ) == 52, "SimCommand has padding" );

// Everything the simulation consumes for one tick. The server's copy is authoritative.
// Events are applied first, then commands in the order listed, then inputs.
struct InputFrame
{
	uint32_t tick = 0;
	std::vector<PlayerEvent> events;
	std::vector<SimCommand> commands;
	std::array<PlayerInput, kMaxPlayers> inputs{};

	bool operator==( const InputFrame& ) const = default;
};

// Must be identical on server and clients; the server sends it on join.
// Animation layers per player (a character's state machine has them; mods name the ones they set
// stances on) and stances (names a state machine's conditions read: "pistol"). A stance index in
// AnimState is the schema's index + 1; 0 is none.
inline constexpr int kMaxAnimLayers = 4;
inline constexpr int kMaxStances = 254;
// A layer's weight eases to where its weight expression says over this long.
inline constexpr float kStanceFadeSeconds = 0.2f;

struct SimConfig
{
	uint32_t tickRate = 60;
	uint32_t subSteps = 4;
	uint64_t seed = 0xC1DEB0C5ull;

	uint32_t propLifetimeSeconds = 20;
	uint32_t propsPerPlayer = 10;
	uint32_t propsGlobal = 256;
	float killY = -20.0f;

	// Memory reserved for Box3D. Only the used part is copied per snapshot.
	uint32_t physicsArenaMB = 256;

	// How players move, unless a mod says otherwise for one of them (move_params.h): the engine's
	// defaults, with the server's options and then its character's values laid over them.
	MoveParams move;

	// How many slots a player has (0: none; a mod then holds items in sockets itself), and the
	// socket (schema order) the selected slot's item is held in.
	uint8_t slots = 0;
	uint8_t slotHand = 0;

	bool operator==( const SimConfig& ) const = default;

	float TimeStep() const
	{
		return 1.0f / float( tickRate );
	}

	uint32_t PropLifetimeTicks() const
	{
		return propLifetimeSeconds * tickRate;
	}
};

} // namespace cb
