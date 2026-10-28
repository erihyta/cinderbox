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

// Buttons the engine itself understands. "Use" uses the item in the hand (what using it does is
// its mod's). Everything else a game adds (reload, spawn a prop) is a mod action instead: a bit in
// PlayerInput::actions whose meaning only the server's mods know.
enum InputButton : uint8_t
{
	BtnJump = 1 << 0,
	BtnSprint = 1 << 1,
	BtnUse = 1 << 2,
};
inline constexpr uint8_t kEngineButtons = BtnJump | BtnSprint | BtnUse;

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
	Move = 2,	// intentA to intentB: the two slots trade what they hold (either may be empty); onto a
				// stack of the same kind, as many as fit go onto it instead
	Drop = 3,	// intentA: the slot, or kNoSlot for the selected one. Its item is thrown out
	Split = 4,	// intentA to intentB: half of intentA's stack (rounded up) goes to intentB, empty or a
				// stack of the same kind with room
	Open = 5,	// the container the player looks at, within reach, is open to it (its slots are
				// kContainerSlotBit | index in the others)
	Close = 6,	// the open container is closed
};
inline constexpr uint8_t kLastSlotIntent = uint8_t( SlotIntent::Close );
// An intent a mod declares ("shop.buy"): kFirstModIntent + its place in the schema's intents, with
// intentA and intentB as its two numbers. The simulation carries it and does nothing with it; the
// mod reads it (Context::Intents) and answers.
inline constexpr uint8_t kFirstModIntent = 32;
inline constexpr bool KnownIntent( uint8_t intent )
{
	return intent <= kLastSlotIntent || intent >= kFirstModIntent;
}
// In an intent's slot: the open container's slot of this index, not the player's own.
inline constexpr uint8_t kContainerSlotBit = 0x80;
// How far a container is reached to open it, and how far the player may go before it closes.
inline constexpr float kContainerReach = 3.0f;
inline constexpr float kContainerKeep = 4.0f;

// Mod actions per player: a bit each in the input, held or not. The server tells clients which
// bit is which (and the default key for it) when they join. An input is a record of one size,
// copied for every player every tick, so the bits are a word of 64; what is sent is only the
// bytes of it that are in use (ActionByteCount).
using ActionBits = uint64_t;
inline constexpr int kMaxActions = 64;

// How many of an action word's bytes are not zero at the top: what a packet carries of it.
inline int ActionByteCount( ActionBits actions )
{
	int bytes = 0;
	while ( actions != 0 )
	{
		++bytes;
		actions >>= 8;
	}
	return bytes;
}

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
	uint8_t buttons = 0;	 // InputButton bits (held state; the sim detects edges)
	uint8_t view = 0;		 // ViewMode
	ActionBits actions = 0;	 // mod action bits (held state)
	uint8_t intent = 0;		 // SlotIntent, carried out when intentSeq changes
	uint8_t intentA = 0;
	uint8_t intentB = 0;
	uint8_t intentSeq = 0;	 // counts the player's intents (it wraps)
	uint16_t slotKeys = 0;	 // the slots' keys held, a bit each (slot 1: bit 0); the sim detects edges
	uint8_t reserved[2] = {};

	bool operator==( const PlayerInput& ) const = default;
};
static_assert( sizeof( PlayerInput ) == 24, "PlayerInput has padding" );

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
	// target (a breakable: an entity whose template has pieces), a = where the blast is, b.x = the
	// speed (m/s) a piece at the blast is sent away with (less with distance: b.x / (1 + metres)),
	// other = the pieces' lifetime in ticks (0: forever). The entity goes, its pieces come.
	Break = 18,
	// target (a player), index = character (ModSchema::characters): from the next tick its body is
	// that character's: its capsule (the feet stay where they are), its state machine (its layers
	// start over). An index the schema does not have does nothing.
	SetCharacter = 19,
	// index = character (ModSchema::characters), a = where its feet stand, value = its yaw (the
	// input's units, 65536 a turn): a body of that character nobody drives (IdleBody). target = a
	// slot (SlotTarget): that slot possesses it at once, as Possess does.
	SpawnBody = 20,
	// target = a slot (SlotTarget), other = a body nobody drives: a character's (IdleBody), or a
	// prop (a loose body: not an item, a ragdoll, a container or static): the slot drives it from now
	// on. The body it drove is left: what it held is put away (not dropped), and it stands where it
	// is, nobody's (a prop is a loose body again, with the speed it had); a dead one is removed with
	// what it carried. Nothing happens for a body someone drives, or anything else.
	Possess = 21,
	// target = a body, mode = 1: a mod drives it with the input packed in a, b, c (PackInput), from
	// this tick on and with that input until the next Drive says otherwise: a character's body
	// nobody drives, or a loose prop, gets the controller (as Possess gives it, with no slot). mode
	// = 0: it is driven no more and stands where it is, nobody's. An NPC is a body a mod drives.
	Drive = 22,
};
inline constexpr uint8_t kLastCommandType = uint8_t( CommandType::Drive );

enum ImpulseMode : uint8_t
{
	ImpulseLinear = 0,	// b is an impulse in N*s
	ImpulseVelocity = 1, // b is a change of velocity in m/s (mass independent)
	// The same change of velocity for the whole of it: every part of a ragdoll, where the two above
	// hit the part nearest the point (a shot). A blast, a wind.
	ImpulseThrow = 2,
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

// How many fields a scope has (an entity's board, the game's, a player's private one) is what the
// server's mods declared: SimConfig::fields and globalFields. This is only what a field's number
// can say in a command and in the schema (16 bits).
inline constexpr int kFieldLimit = 65535;
// Likewise for motions (SimConfig::motions): what a motion's number can say where it is kept.
inline constexpr int kMotionLimit = 65535;
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

// A PlayerInput in a command's a, b, c: three of its bytes to each float, as a whole number below
// 2^24, which a float holds exactly (and is always finite, so the command can be sent).
inline void PackInput( const PlayerInput& in, SimCommand& c )
{
	uint8_t bytes[27] = {};
	std::memcpy( bytes, &in, sizeof( PlayerInput ) );
	float* out[9] = { &c.a.x, &c.a.y, &c.a.z, &c.b.x, &c.b.y, &c.b.z, &c.c.x, &c.c.y, &c.c.z };
	for ( int i = 0; i < 9; ++i )
	{
		*out[i] = float( uint32_t( bytes[3 * i] ) | ( uint32_t( bytes[3 * i + 1] ) << 8 ) | ( uint32_t( bytes[3 * i + 2] ) << 16 ) );
	}
}
inline PlayerInput UnpackInput( const SimCommand& c )
{
	uint8_t bytes[27] = {};
	const float in[9] = { c.a.x, c.a.y, c.a.z, c.b.x, c.b.y, c.b.z, c.c.x, c.c.y, c.c.z };
	for ( int i = 0; i < 9; ++i )
	{
		const uint32_t v = in[i] >= 0.0f && in[i] < 16777216.0f ? uint32_t( in[i] ) : 0u;
		bytes[3 * i] = uint8_t( v );
		bytes[3 * i + 1] = uint8_t( v >> 8 );
		bytes[3 * i + 2] = uint8_t( v >> 16 );
	}
	PlayerInput out;
	std::memcpy( &out, bytes, sizeof( PlayerInput ) );
	return out;
}

// Everything the simulation consumes for one tick. The server's copy is authoritative.
// Events are applied first, then commands in the order listed, then inputs.
// Where a character's pusher stands for a tick (M246; ROADMAP step 3): a capsule in the physics
// world that shoves the bodies pushed by characters. Everything the physics world needs is here:
// a simulation that follows another's physics places pushers from these alone. Its centre on a
// 1/1024 m grid, its size in millimetres.
inline constexpr float kPusherGrid = 1024.0f;
// Where the characters' NetIds start (SimGlobals::nextCharacterId); the physics world's are below.
inline constexpr uint32_t kCharacterIdBase = 0x10000000u;
// Where a hybrid client's own NetIds for the characters' side start (M248): what its simulation
// makes there itself (a mod's character, an item given, in the physics' past or predicted) is never
// the server's, whose characters come in its states; the next state removes it.
inline constexpr uint32_t kLocalCharacterIdBase = 0x70000000u;
struct PusherPlace
{
	uint32_t netId = 0; // the character's
	int32_t x = 0, y = 0, z = 0;
	uint16_t radius = 0, halfHeight = 0;

	bool operator==( const PusherPlace& ) const = default;
};

// What a character does to the physics world (M246; ROADMAP step 3): every write to a physics
// body that reads a character (its motions' pushes and breaks, what it launches or lets go of, the
// ragdoll it leaves) is one of these, queued and applied at one of two points in the step (after
// the characters move, before the physics step; after the items' own motions), self-contained: a simulation that follows another's
// physics applies these as they are, whatever its own characters do. (A mod's command whose
// numbers are all in it is applied directly: every simulation has the frame's commands.)
enum class PhysicsEventType : uint8_t
{
	Push = 0,	   // an impulse on a body: `netId`, at `at`, `vector`
	Break = 1,	   // the breakable `netId` breaks: blast at `at`, radius `vector.x`, speed `vector.y`, rubble lasts `ticks`
	IntoWorld = 2, // the item `netId` (made with that NetId when missing: of `kind`, `count`, `board`) is put at
				   // `at` (its grip), turned `turn`, going `vector`; `owner` != 0 also makes it a prop of
				   // `owner`'s that lasts `ticks` (0: for ever)
	Ragdoll = 3,   // a ragdoll of the player `netId` (slot `slot`): its feet at `at`, facing `yaw`, going
				   // `vector`, weighing `mass`, lasting `ticks`; then hit (`hitAt`, `hitVector`, `mode`), and
				   // the ragdolls kept to `count` (0: no cap). It is made with the NetId `made`
	// Bodies between the two worlds (M247): what a character takes from the physics world or gives
	// back to it.
	Remove = 4,	  // the entity `netId` is gone (an item its own motion removed)
	TakeOver = 5, // the body of `netId` leaves the physics world (a prop driven, a body taken over, an item picked up)
	Enter = 6,	  // the body of `netId` joins it: as `body` describes it (Simulation::DescribeBody), made dynamic,
				  // its shapes of category `owner`, at `at` turned `turn`, going `vector` (a driven prop let go)
	Idle = 7,	  // `netId` is a body nobody drives (a player's left behind): of character `kind`, its capsule
				  // `hitAt` (radius, half height), its centre at `at` turned `turn`, going `vector`, weighing `mass`
	Spawn = 8,	  // a mod's SpawnProp as `command` says, its owner (`command.target`) the NetId it named: what
				  // a player spawned is the player's (its caps), which a simulation without the characters
				  // could not tell
};

struct PhysicsEvent
{
	PhysicsEventType type = PhysicsEventType::Push;
	// Where in the step it is applied: 0 after the commands (a kill's ragdoll, a body taken over), 1
	// after the characters move, 2 after the items' own motions (after the physics step: a frag's
	// blast). Nothing waits for a later tick.
	uint8_t phase = 0;
	// Phase 0: after which of the frame's commands (0: the players' joins and leaves; k + 1: command
	// k). Each command's are applied before the next command runs, which may read what they did (a
	// body left behind, taken over in the same tick by another player).
	uint16_t after = 0;
	uint8_t mode = 0;
	uint16_t kind = 0;
	PlayerSlot slot = 0;
	uint8_t count = 0;
	uint32_t netId = 0;
	uint32_t owner = 0;
	uint32_t ticks = 0;
	uint32_t made = 0; // the entity it made (a ragdoll), once applied
	Float3 at, vector, hitAt, hitVector;
	float turn[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float yaw = 0.0f, mass = 0.0f;
	std::vector<int32_t> board; // IntoWorld: the item's fields (empty: none)
	std::vector<uint8_t> body;	// Enter: the body's description
	SimCommand command;			// Spawn

	bool operator==( const PhysicsEvent& ) const = default;
};

struct InputFrame
{
	uint32_t tick = 0;
	std::vector<PlayerEvent> events;
	std::vector<SimCommand> commands;
	std::array<PlayerInput, kMaxPlayers> inputs{};
	// The physics world's inputs for a simulation that follows another's physics
	// (Simulation::SetFollowsPhysics): where the pushers stand, and what the characters did to it.
	// Empty otherwise, and never sent (yet: M248).
	std::vector<PusherPlace> pushers;
	std::vector<PhysicsEvent> physics;

	bool operator==( const InputFrame& ) const = default;
};

// Must be identical on server and clients; the server sends it on join.
// Animation layers per player (a character's state machine has them; mods name the ones they set
// stances on) and stances (names a state machine's conditions read: "pistol"). A stance index in
// AnimState is the schema's index + 1; 0 is none.
// How many layers there are is the server's (SimConfig::layers: its character's and its mods');
// this is only what a layer's number can say in the schema (a byte).
inline constexpr int kLayerLimit = 255;
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

	// How much state the server's mods asked for: the fields an entity's board has and the game's
	// (the schema's, per scope). Every simulation of a session sizes its state by these, so they
	// are part of what a snapshot and a state hash are.
	uint16_t fields = 0;
	uint16_t globalFields = 0;
	// And how many motions its mods' sets have, all together: a player has a slot for each.
	uint16_t motions = 0;
	// And how many animation layers a player has: the most its character's state machine or the
	// mods name.
	uint8_t layers = 0;
	uint8_t reserved = 0;

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
