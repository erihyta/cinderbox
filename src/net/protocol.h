#pragma once

// Wire protocol between server and clients.
//
// Channel 0 (reliable, ordered): Hello, Welcome, Reject, Checksum, ResyncRequest, PlayerNames.
// Channel 1 (unreliable), both directions:
//   client -> server: Input, repeating the last few ticks and acknowledging received frames;
//   server -> client: FrameBatch, every frame from the client's acknowledgement to the newest.
// Nothing waits for a retransmission: a lost batch is simply covered by the next one.

#include "bytes.h"
#include "types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cb::net
{

inline constexpr uint32_t kProtocolVersion = 5; // 4: pitch, mod actions, commands, mod schema; 5: names
inline constexpr uint16_t kDefaultPort = 7777;

enum Channel : uint8_t
{
	ChannelReliable = 0,
	ChannelInput = 1,
	ChannelCount = 2,
};

enum class MsgType : uint8_t
{
	Hello = 1,
	Welcome = 2,
	Reject = 3,
	Frame = 4,
	Checksum = 5,
	ResyncRequest = 6,
	Input = 7,
	FrameBatch = 8,
	PlayerNames = 9,
};

inline constexpr size_t kMaxPlayerName = 24;

using InputArray = std::array<PlayerInput, kMaxPlayers>;

// C -> S. reconnectToken is 0 for a new player.
struct MsgHello
{
	uint32_t version = kProtocolVersion;
	uint64_t fingerprint = 0;
	uint64_t reconnectToken = 0;
	std::string name; // what the player wants to be called; the server sanitizes it
};

// S -> C, whenever the roster changes, and after every Welcome. The names of the players in the
// world, by slot. Presentation only: never part of the simulation or its hash.
struct MsgPlayerNames
{
	std::vector<std::pair<PlayerSlot, std::string>> names;
};

// The name the server keeps: printable characters only, trimmed, at most kMaxPlayerName bytes,
// "Player <slot + 1>" when nothing is left.
std::string SanitizeName( const std::string& name, PlayerSlot slot );

// S -> C, on join, reconnect and desync recovery. The client loads `image` (the state before
// `snapshotTick`), then applies frames starting at `snapshotTick`. `baseInputs` are the inputs of
// frame snapshotTick - 1 (the delta base and the prediction seed).
//
// `map` is the server's baked level (a .cbmap, see sim/map.h). The client builds its simulation
// from these bytes, so it can never run a different level: entities created later (a player
// spawning, a prop) depend on the map, and a mismatch would desync.
//
// `schema` names what the server's mods declared (sim/mod_schema.h): board fields, mod events and
// actions. Presentation and input binding read it; the simulation never does.
struct MsgWelcome
{
	uint32_t version = kProtocolVersion;
	uint64_t fingerprint = 0;
	SimConfig config;
	PlayerSlot slot = 0;
	uint64_t reconnectToken = 0;
	uint32_t snapshotTick = 0;
	InputArray baseInputs{};
	uint64_t mapHash = 0;
	std::vector<uint8_t> map;
	std::vector<uint8_t> image;
	std::vector<uint8_t> schema;
};

struct MsgReject
{
	std::string reason;
};

struct MsgChecksum
{
	uint32_t tick = 0; // hash of the state in which Tick() == tick
	uint64_t hash = 0;
};

struct MsgResyncRequest
{
	uint32_t tick = 0;
};

// C -> S. Inputs for ticks [newestTick - count + 1, newestTick].
struct MsgInput
{
	uint32_t newestTick = 0;
	uint32_t ackTick = 0; // the client has every authoritative frame with tick < ackTick
	std::vector<PlayerInput> inputs; // oldest first
};

void Encode( const MsgHello& m, std::vector<uint8_t>& out );
void Encode( const MsgWelcome& m, std::vector<uint8_t>& out );
void Encode( const MsgReject& m, std::vector<uint8_t>& out );
void Encode( const MsgChecksum& m, std::vector<uint8_t>& out );
void Encode( const MsgResyncRequest& m, std::vector<uint8_t>& out );
void Encode( const MsgInput& m, std::vector<uint8_t>& out );
void Encode( const MsgPlayerNames& m, std::vector<uint8_t>& out );

bool Decode( ByteReader& r, MsgHello& m );
bool Decode( ByteReader& r, MsgWelcome& m );
bool Decode( ByteReader& r, MsgReject& m );
bool Decode( ByteReader& r, MsgChecksum& m );
bool Decode( ByteReader& r, MsgResyncRequest& m );
bool Decode( ByteReader& r, MsgInput& m );
bool Decode( ByteReader& r, MsgPlayerNames& m );

// Returns the message type and leaves the reader positioned at the payload.
std::optional<MsgType> ReadType( ByteReader& r );

// Input frames are delta-coded against the previous frame on the same reliable stream.
class FrameCodec
{
public:
	void Reset( const InputArray& base )
	{
		m_previous = base;
	}

	void Encode( const InputFrame& frame, std::vector<uint8_t>& out );
	bool Decode( ByteReader& r, InputFrame& frame );

	// Frame without the message type byte (used inside batches).
	void EncodeBody( const InputFrame& frame, ByteWriter& w );
	bool DecodeBody( ByteReader& r, InputFrame& frame );

	const InputArray& Previous() const
	{
		return m_previous;
	}

private:
	InputArray m_previous{};
};

// S -> C frame batch: `frames` must be consecutive; `base` holds the inputs of the frame before
// the first one (the delta base). Layout: type, u32 firstTick, u8 count, frame bodies.
inline constexpr size_t kMaxBatchFrames = 64;
void EncodeFrameBatch( const InputArray& base, const InputFrame* const* frames, size_t count, std::vector<uint8_t>& out );

// First step of decoding: which ticks does the batch cover?
bool ReadFrameBatchHeader( ByteReader& r, uint32_t& firstTick, uint32_t& count );
// Second step, once the caller has found the inputs of firstTick - 1.
bool ReadFrameBatchBody( ByteReader& r, const InputArray& base, uint32_t firstTick, uint32_t count,
						 std::vector<InputFrame>& out );

// Commands a server is about to send: anything that is not finite, or not a known type, is dropped
// here so every client applies exactly the same list.
bool IsSendableCommand( const SimCommand& command );

// Canonical input sanitation, applied by the server before an input enters a frame.
inline PlayerInput SanitizeInput( PlayerInput in )
{
	if ( in.moveRight < -127 )
	{
		in.moveRight = -127;
	}
	if ( in.moveForward < -127 )
	{
		in.moveForward = -127;
	}
	if ( in.cameraPitch > kMaxCameraPitch )
	{
		in.cameraPitch = kMaxCameraPitch;
	}
	if ( in.cameraPitch < -kMaxCameraPitch )
	{
		in.cameraPitch = -kMaxCameraPitch;
	}
	in.buttons &= kEngineButtons;
	in.reserved = 0;
	return in;
}

} // namespace cb::net
