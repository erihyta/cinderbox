#pragma once

// Types shared by server, client and tools. Everything that crosses the network or feeds the
// simulation is integer-quantized so it is identical on every machine.

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

// Mod actions per player. The server tells clients which bit is which (and the default key for
// it) when they join; the simulation never looks at them.
inline constexpr int kMaxActions = 16;

// Camera pitch is limited to just short of straight up or down (full turn = 65536).
inline constexpr int16_t kMaxCameraPitch = 16000;

// One player's input for one tick. 10 bytes, no padding.
struct PlayerInput
{
	int8_t moveRight = 0;	 // [-127, 127]
	int8_t moveForward = 0;	 // [-127, 127]
	uint16_t cameraYaw = 0;	 // full turn = 65536, 0 looks down +Z
	int16_t cameraPitch = 0; // full turn = 65536, positive looks up, within +/- kMaxCameraPitch
	uint16_t actions = 0;	 // mod action bits (held state)
	uint8_t buttons = 0;	 // InputButton bits (held state; the sim detects edges)
	uint8_t reserved = 0;

	bool operator==( const PlayerInput& ) const = default;
};
static_assert( sizeof( PlayerInput ) == 10, "PlayerInput has padding" );

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
};

// --- Commands ------------------------------------------------------------------------------------
//
// What the server's gameplay mods decide, in a form every simulation can apply. Mods run only on
// the server; they never touch the simulation directly. Instead each authoritative frame carries
// the commands they produced, and clients apply them exactly like inputs, so the world stays
// deterministic while the rules stay on the server.
//
// A command names its target by NetId, or by player slot with SlotTarget(). Commands whose target
// does not exist (any more) are ignored, identically everywhere.

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
};
inline constexpr uint8_t kLastCommandType = uint8_t( CommandType::Freeze );

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

// Per-entity and global board sizes (see Blackboard in components.h).
inline constexpr int kBoardSlots = 16;
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
