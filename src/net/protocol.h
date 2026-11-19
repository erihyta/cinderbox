#pragma once

// Wire protocol between server and clients.
//
// Channel 0 (reliable, ordered): Hello, Welcome, Reject, Checksum, ResyncRequest, PlayerNames, PrivateFields, Items, Text,
// InfoRequest, Info.
// Channel 1 (unreliable), both directions:
//   client -> server: Input, repeating the last few ticks and acknowledging received frames;
//   server -> client: FrameBatch, every frame from the client's acknowledgement to the newest.
// Nothing waits for a retransmission: a lost batch is simply covered by the next one.

#include "components.h"
#include "mod_schema.h"
#include "bytes.h"
#include "types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cb::net
{

inline constexpr uint32_t kProtocolVersion = 92; // each version's change: docs/versions.md (add a row when raising it)
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
	PrivateFields = 10,
	Items = 11,
	Text = 12,
	InfoRequest = 13,
	Info = 14,
	State = 15, // S -> C, the hybrid: the server's characters' state (state_codec.h), unreliable
	InputLoss = 16, // S -> C (M261): how many of its input packets the server did not get lately
};

// How a server keeps its clients' worlds (ROADMAP step 3), its choice for everyone. Inputs: every
// client simulates the whole world from everyone's inputs (FrameBatch with the inputs, Checksum,
// rollback). Hybrid: every client simulates the physics from the server's events, a little in the
// past (FrameBatch without the inputs: the commands, the physics events and pushers' places, the
// physics checksum), and the characters are the server's (State): a client predicts its own, the
// others stand where the server had them (sim/hybrid_session.h).
enum class NetModel : uint8_t
{
	Inputs = 0,
	Hybrid = 1,
};
const char* ToString( NetModel model );

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
	// Every player's name (a client's first list), or only the slots whose name changed since the
	// last (M255: "" for a slot left empty), laid over the names it has.
	bool whole = true;
};

// S -> C, when a mod's look was published again while the server runs, and the new one changes
// nothing the simulation reads: the workshop items players need now, all of them (the schema's
// list, replaced). Presentation only. A client loads the new item over the old and shows it; one
// that does not have it keeps playing with the look it has.
inline constexpr size_t kMaxItems = 64;
struct MsgItems
{
	std::vector<ModItem> items;
};

// C -> S, instead of a Hello: what is this server? It answers with an Info and nothing else (the
// asker is not a player and is never welcomed), whatever version the asker is: the answer's own
// version tells the asker whether it could join. A menu asks before joining, to list the workshop
// items the server needs and fetch the missing ones (the server never sends them).
struct MsgInfoRequest
{
	uint32_t version = kProtocolVersion;
};
struct MsgInfo
{
	uint32_t version = kProtocolVersion; // first, so any version can read it
	uint64_t fingerprint = 0;			 // the simulation's build: equal, or joining is refused
	std::string map;
	uint16_t players = 0; // (two bytes since M252)
	uint16_t maxPlayers = 0;
	std::vector<ModItem> items; // the workshop items its mods need (and its character's)
	NetModel model = NetModel::Inputs;
};

// Typed text, both ways. C -> S: what the player typed into a channel's box (`from` is ignored:
// the server knows who sent it). S -> C: a line a mod said on a channel, from a player or from
// nobody (kNoPlayer: the server's own words). Presentation only: never part of the simulation, its
// hash or recordings. What reaches whom is the mods' (mod_api.h: Context::Typed, Context::Say);
// text for a channel no mod declared goes nowhere.
inline constexpr size_t kMaxText = 200;		// bytes of UTF-8
inline constexpr size_t kMaxTextChannel = 32;
struct MsgText
{
	std::string channel;
	PlayerSlot from = kNoPlayer;
	std::string text;
};
// Printable characters only (no control characters), trimmed, at most `limit` bytes cut on a
// character boundary.
std::string SanitizeText( const std::string& text, size_t limit );

// S -> C, to one client: its own private fields (BoardScope::Private), after its Welcome and
// whenever a mod changes one. All of them each time (the ones that are not 0); nobody else's are
// ever sent to it.
struct MsgPrivateFields
{
	Blackboard values;
};

// The name the server keeps: printable characters only, trimmed, at most kMaxPlayerName bytes,
// "Player <slot + 1>" when nothing is left.
std::string SanitizeName( const std::string& name, PlayerSlot slot );

// S -> C, on join, reconnect and desync recovery. The client loads `image` (the state before
// `snapshotTick`), then applies frames starting at `snapshotTick`. `baseInputs` are the inputs of
// frame snapshotTick - 1 (the delta base and the prediction seed). The hybrid: the frames bring
// the physics, and the characters' states come every `stateEvery` ticks.
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
	NetModel model = NetModel::Inputs;
	uint8_t stateEvery = 2;
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
// S -> C (M261): of the client's input packets lately, how many in a thousand did not arrive (by
// their sequence): what it sizes its repeats by (net::InputRepeats). Every two seconds.
struct MsgInputLoss
{
	uint16_t perMille = 0;
};

struct MsgInput
{
	uint32_t newestTick = 0;
	// The packet's number, one more each packet (wrapping): the server counts those that do not come.
	uint8_t sequence = 0;
	// The client has every authoritative frame with tick < ackTick.
	uint32_t ackTick = 0;
	// The hybrid: the newest characters' state the client has and keeps as a base (0: none).
	uint32_t stateAck = 0;
	std::vector<PlayerInput> inputs; // oldest first
};

// How many packets in a row an input goes in (M261): enough that losing all of them happens once
// in a million at the measured loss (each packet lost on its own), and at least
// kLeastInputRepeats, since losses come in bursts; at most `most`. (12 before M261, whatever the
// loss.) The loss is the server's count of the client's (MsgInputLoss).
inline constexpr uint32_t kLeastInputRepeats = 4;
// The most ticks an input packet carries.
inline constexpr uint32_t kMaxInputsPerPacket = 32;
uint32_t InputRepeats( uint32_t lossPerMille, uint32_t most );

void Encode( const MsgHello& m, std::vector<uint8_t>& out );
void Encode( const MsgWelcome& m, std::vector<uint8_t>& out );
void Encode( const MsgReject& m, std::vector<uint8_t>& out );
void Encode( const MsgChecksum& m, std::vector<uint8_t>& out );
void Encode( const MsgResyncRequest& m, std::vector<uint8_t>& out );
void Encode( const MsgInput& m, std::vector<uint8_t>& out );
void Encode( const MsgInputLoss& m, std::vector<uint8_t>& out );
void Encode( const MsgPlayerNames& m, std::vector<uint8_t>& out );
void Encode( const MsgItems& m, std::vector<uint8_t>& out );
void Encode( const MsgPrivateFields& m, std::vector<uint8_t>& out );
void Encode( const MsgText& m, std::vector<uint8_t>& out );
void Encode( const MsgInfoRequest& m, std::vector<uint8_t>& out );
void Encode( const MsgInfo& m, std::vector<uint8_t>& out );

bool Decode( ByteReader& r, MsgHello& m );
bool Decode( ByteReader& r, MsgWelcome& m );
bool Decode( ByteReader& r, MsgReject& m );
bool Decode( ByteReader& r, MsgChecksum& m );
bool Decode( ByteReader& r, MsgResyncRequest& m );
bool Decode( ByteReader& r, MsgInput& m );
bool Decode( ByteReader& r, MsgInputLoss& m );
bool Decode( ByteReader& r, MsgPlayerNames& m );
bool Decode( ByteReader& r, MsgItems& m );
bool Decode( ByteReader& r, MsgPrivateFields& m );
bool Decode( ByteReader& r, MsgText& m );
bool Decode( ByteReader& r, MsgInfoRequest& m );
// False also for an Info of another version past its version (only `version` is then set).
bool Decode( ByteReader& r, MsgInfo& m );

// Returns the message type and leaves the reader positioned at the payload.
std::optional<MsgType> ReadType( ByteReader& r );

// Input frames are delta-coded against the previous frame on the same reliable stream (its inputs,
// and its pushers' places).
class FrameCodec
{
public:
	// `pushers`: the places of the frame before (M256: a batch's first frame codes its pushers
	// against them; null: none, every place whole), and `before` those of the frame before that
	// (a pusher's change is against where it would be going on as it went; null: its place before).
	void Reset( const InputArray& base, const std::vector<PusherPlace>* pushers = nullptr, const std::vector<PusherPlace>* before = nullptr );

	void Encode( const InputFrame& frame, std::vector<uint8_t>& out );
	bool Decode( ByteReader& r, InputFrame& frame );

	// Frame without the message type byte (used inside batches). `withInputs` false: the hybrid's,
	// whose clients simulate no one else's character (decoded, its inputs are the previous ones).
	// `checksum` != 0: the server's physics checksum after the frame's tick (decoded into
	// `checksum`, 0 when it has none).
	void EncodeBody( const InputFrame& frame, ByteWriter& w, bool withInputs = true, uint64_t checksum = 0 );
	bool DecodeBody( ByteReader& r, InputFrame& frame, uint64_t* checksum = nullptr );

	const InputArray& Previous() const
	{
		return m_previous;
	}

private:
	InputArray m_previous{};
	std::vector<PusherPlace> m_previousPushers;
	std::vector<PusherPlace> m_beforePushers; // (the frame's before the previous)
};

// S -> C frame batch: `frames` must be consecutive; `base` holds the inputs of the frame before
// the first one (the delta base), and `basePushers` that frame's pushers' places, `beforePushers`
// the frame's before it (M256; null: the client has no such frame, the first after its snapshot).
// Layout: type, u32 firstTick, u8 count, frame bodies. The hybrid: without the inputs, and with a
// physics checksum for each frame whose `checksums` entry is not 0.
inline constexpr size_t kMaxBatchFrames = 64;
void EncodeFrameBatch( const InputArray& base, const InputFrame* const* frames, size_t count, std::vector<uint8_t>& out,
					   bool withInputs = true, const uint64_t* checksums = nullptr, const std::vector<PusherPlace>* basePushers = nullptr,
					   const std::vector<PusherPlace>* beforePushers = nullptr );

// First step of decoding: which ticks does the batch cover?
bool ReadFrameBatchHeader( ByteReader& r, uint32_t& firstTick, uint32_t& count );
// Second step, once the caller has found the inputs of firstTick - 1. `checksums`: each frame's
// physics checksum (0: none).
bool ReadFrameBatchBody( ByteReader& r, const InputArray& base, uint32_t firstTick, uint32_t count,
						 std::vector<InputFrame>& out, std::vector<uint64_t>* checksums = nullptr,
						 const std::vector<PusherPlace>* basePushers = nullptr, const std::vector<PusherPlace>* beforePushers = nullptr );

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
	in.view = in.view < kViewModes ? in.view : uint8_t( 0 );
	in.intent = KnownIntent( in.intent ) ? in.intent : uint8_t( 0 );
	return in;
}

} // namespace cb::net
