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

inline constexpr uint32_t kProtocolVersion = 66; // 66: an item's own motions (owner item), break and remove in a motion set; 65: markers, turns, a launch's start, paths and follow in a motion set; 64: curves and expressions in a motion set (an effect's strength), time, distance and reach; 63: a probe's shape (probe_shape, in a motion set); 62: a motion's finds (shapes in a motion set), movement parameters given for a while (MoveEffects in the snapshot); 61: a motion's steps, guards, ends and edge (motion sets of version 3; its slot 28 bytes); 60: glued breakables (maps of version 11); 59: items that break walls (their break speed, hole, through and smash event in the schema); 58: holes in breakables (a wall piece left standing, a map of version 10, Break with a radius); 57: what a probe may hold on to (holds, in a motion set); 56: the Break command, maps of version 9 (breakables); 55: an item's rolling and drag (the schema), maps of version 8 (a prop's), max_slope; 54: an item's body of several shapes (the schema); maps of version 4 to 6 (pieces of any shape, hulls, bodies of several shapes); 53: asking a server about itself before joining (InfoRequest, Info); 52: typed text (Text, both ways); 51: an item its slot's key uses; 50: a slot is a place (items used from the hand, or by a kind's own key); 49: intents a mod declares (in the schema, in the input); 48: containers (the Open and Close intents, a player's open one in its slots); 47: the Split intent; 46: stacks (a kind's in the schema, an item's count in the frame); 45: an item thrown fast (a bullet) in the schema; 44: an item's time is up (its event in the schema); 43: launch_carried in a motion, an event said after the motions; 42: a line in flight in the snapshot (a projectile); 41: an item's hand in the schema; 40: the slot keys in the input, a slot's suggested key in the schema; 39: an input packet's later ticks as changes, a pitch difference in a frame; 38: a look published again while the server runs (Items); 37: what is loose gives way to a player by its weight; a throw moves a whole ragdoll; 36: launches (a motion makes an item); 35: an item's friction and bounce, a map prop's body; 34: state sized by the mods (how many fields; the config says); 33: the use button, how an item kind is used; 32: slots (a player's intent in its input, an item's slot, the config's slots); 31: (unused: texts, parked); 30: mass; motions as triggers and effects (probes, forces, links, targets); 29: one more movement parameter (airborne); 28: tethers (a component, a line of a motion set); 27: two more movement parameters (air_friction, move_frame); 26: motion sets in the schema; 25: movement parameters (the config, SetMove); 24: an item body's turn; 23: grips as animated; 22: grips in the schema; 21: the view mode in the input; 20: no streaming clients; 19: private fields; 18: one animation system; 17: streaming clients; 4: pitch, mod actions, commands, mod schema; 5: names; 6: character; 7: aim in the pose; 8: facing, legs; 9: layers and stances; 10: characters shipped with the game; 11: state machines in the schema; 12: held items; 13: animation packs; 14: items in the world; 15: stowed items; 16: 32 board slots
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
	uint8_t players = 0;
	uint8_t maxPlayers = 0;
	std::vector<ModItem> items; // the workshop items its mods need (and its character's)
};

// Typed text, both ways. C -> S: what the player typed into a channel's box (`from` is ignored:
// the server knows who sent it). S -> C: a line a mod said on a channel, from a player or from
// nobody (kNoSlot: the server's own words). Presentation only: never part of the simulation, its
// hash or recordings. What reaches whom is the mods' (mod_api.h: Context::Typed, Context::Say);
// text for a channel no mod declared goes nowhere.
inline constexpr size_t kMaxText = 200;		// bytes of UTF-8
inline constexpr size_t kMaxTextChannel = 32;
struct MsgText
{
	std::string channel;
	PlayerSlot from = kNoSlot;
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
bool Decode( ByteReader& r, MsgPlayerNames& m );
bool Decode( ByteReader& r, MsgItems& m );
bool Decode( ByteReader& r, MsgPrivateFields& m );
bool Decode( ByteReader& r, MsgText& m );
bool Decode( ByteReader& r, MsgInfoRequest& m );
// False also for an Info of another version past its version (only `version` is then set).
bool Decode( ByteReader& r, MsgInfo& m );

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
	in.view = in.view < kViewModes ? in.view : uint8_t( 0 );
	in.intent = KnownIntent( in.intent ) ? in.intent : uint8_t( 0 );
	return in;
}

} // namespace cb::net
