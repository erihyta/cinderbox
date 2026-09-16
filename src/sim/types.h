#pragma once

// Types shared by server, client and tools. Everything that crosses the network or feeds the
// simulation is integer-quantized so it is identical on every machine.

#include <array>
#include <cstdint>
#include <vector>

namespace cb
{

inline constexpr int kMaxPlayers = 64;
using PlayerSlot = uint8_t;

enum InputButton : uint8_t
{
	BtnJump = 1 << 0,
	BtnSprint = 1 << 1,
	BtnSpawnProp = 1 << 2,
};

// One player's input for one tick. 6 bytes, no padding.
struct PlayerInput
{
	int8_t moveRight = 0;	// [-127, 127]
	int8_t moveForward = 0; // [-127, 127]
	uint16_t cameraYaw = 0; // full turn = 65536, 0 looks down +Z
	uint8_t buttons = 0;	// InputButton bits (held state; the sim detects edges)
	uint8_t reserved = 0;

	bool operator==( const PlayerInput& ) const = default;
};

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

// Everything the simulation consumes for one tick. The server's copy is authoritative.
// Events are applied before inputs, in the order listed.
struct InputFrame
{
	uint32_t tick = 0;
	std::vector<PlayerEvent> events;
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
