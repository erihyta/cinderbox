#pragma once

// Wire protocol between server and clients.
//
// Channel 0 (reliable, ordered) carries everything the server sends, so a client always sees
// Welcome/Resync and the input frames that follow them in the right order.
// Channel 1 (unreliable) carries client inputs, each packet repeating the last few ticks.

#include "bytes.h"
#include "types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cb::net
{

inline constexpr uint32_t kProtocolVersion = 1;
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
};

using InputArray = std::array<PlayerInput, kMaxPlayers>;

// C -> S. reconnectToken is 0 for a new player.
struct MsgHello
{
	uint32_t version = kProtocolVersion;
	uint64_t fingerprint = 0;
	uint64_t reconnectToken = 0;
};

// S -> C, on join, reconnect and desync recovery. The client loads `image` (the state before
// `snapshotTick`), then applies frames starting at `snapshotTick`. `baseInputs` are the inputs of
// frame snapshotTick - 1 (the delta base and the prediction seed).
struct MsgWelcome
{
	uint32_t version = kProtocolVersion;
	uint64_t fingerprint = 0;
	SimConfig config;
	PlayerSlot slot = 0;
	uint64_t reconnectToken = 0;
	uint32_t snapshotTick = 0;
	InputArray baseInputs{};
	std::vector<uint8_t> image;
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
	std::vector<PlayerInput> inputs; // oldest first
};

void Encode( const MsgHello& m, std::vector<uint8_t>& out );
void Encode( const MsgWelcome& m, std::vector<uint8_t>& out );
void Encode( const MsgReject& m, std::vector<uint8_t>& out );
void Encode( const MsgChecksum& m, std::vector<uint8_t>& out );
void Encode( const MsgResyncRequest& m, std::vector<uint8_t>& out );
void Encode( const MsgInput& m, std::vector<uint8_t>& out );

bool Decode( ByteReader& r, MsgHello& m );
bool Decode( ByteReader& r, MsgWelcome& m );
bool Decode( ByteReader& r, MsgReject& m );
bool Decode( ByteReader& r, MsgChecksum& m );
bool Decode( ByteReader& r, MsgResyncRequest& m );
bool Decode( ByteReader& r, MsgInput& m );

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

	const InputArray& Previous() const
	{
		return m_previous;
	}

private:
	InputArray m_previous{};
};

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
	in.buttons &= uint8_t( BtnJump | BtnSprint | BtnSpawnProp );
	in.reserved = 0;
	return in;
}

} // namespace cb::net
