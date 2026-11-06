#pragma once

// The state network model's message (ROADMAP step 3): one of the server's states
// (Simulation::SaveState), whole or as a change against a state the client said it has.
//
// The change lines the two states up entity by entity (by NetId; the state's layout gives every
// entity its size, so no component needs knowing): an entity whose record is the base's costs its
// NetId and a byte; one of the same size goes as its bytes XOR the base's, which are mostly zeros;
// anything else whole. A body's description goes only when its hash is not the base's. Then the
// whole is deflated. Decoding gives back the server's state byte for byte.
//
// Layout: u8 MsgType::State, u32 tick, u32 base tick (UINT32_MAX: none), u32 size before
// deflating, the deflated payload. Decoding never trusts the bytes: sizes are bounded, and a state
// that does not line up with its base fails.

#include "bytes.h"
#include "events.h"
#include "puppet.h"
#include "types.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cb::net
{

inline constexpr uint32_t kNoStateBase = UINT32_MAX;
inline constexpr size_t kMaxStateBytes = 16u << 20;

// What a client holds of one of the server's states: its tick (the state in which the server's
// Tick() == tick), the inputs the server used for the tick before, and the state. And (M253) the
// other characters it was sent as puppets (sim/puppet.h), by NetId: their records in `state` are
// as Simulation::PuppetView makes them, and so are those of what they hold.
// M257: a mod event at a character (or at what it holds), as a client is sent it: with the
// character, to the clients it is sent to (not in every frame to everyone). Before the state's tick.
struct CharacterEvent
{
	uint32_t character = 0;
	ModEventRecord record;
	uint32_t serial = 0; // the server's count of events: one sent again (a state lost) is heard once
};
struct StateFrame
{
	uint32_t tick = 0;
	std::array<PlayerInput, kMaxPlayers> inputs{};
	std::vector<uint8_t> state;
	std::vector<PuppetEntry> puppets;
	// The events at characters it came with (updates only: M257), by character, each's oldest first.
	std::vector<CharacterEvent> events;
};

// A puppet as a change against another (null: none), in a state of `tick`: a few bytes for one that
// moved (M253). Exact: the decoded one is the encoded one.
void EncodePuppet( const Puppet& now, const Puppet* base, uint32_t tick, std::vector<uint8_t>& out );
bool DecodePuppet( ByteReader& r, const Puppet* base, uint32_t tick, Puppet& out );

// `base` null: the state whole.
void EncodeState( const StateFrame& frame, const StateFrame* base, std::vector<uint8_t>& out );

// A state's parts, pointing into its bytes (sim/stand_ins.cpp has the layout): read once, so a
// server can build each client's view from the states it keeps (M250).
struct StateSpan
{
	uint32_t netId = 0;
	const uint8_t* record = nullptr;
	uint32_t recordSize = 0;
	const uint8_t* bodies = nullptr;
	uint32_t bodiesSize = 0;
	uint64_t hash = 0;
	// A server's (M253; ReadLayout leaves them null): the record as a puppet's client is sent it
	// (PuppetView: a character's or what one holds), and a character's puppet.
	const uint8_t* view = nullptr;
	uint32_t viewSize = 0;
	const Puppet* puppet = nullptr;
};
struct StateLayout
{
	uint32_t magic = 0;
	const uint8_t* head = nullptr;
	uint32_t headSize = 0;
	std::vector<StateSpan> entities; // by NetId
	// The entity's part (null: not in this state).
	const StateSpan* Find( uint32_t netId ) const;
};
// False: not a state's layout. `out` points into `state`, which must outlive it and not change.
bool ReadLayout( const std::vector<uint8_t>& state, StateLayout& out );

// Updates (M250): a state as a client is sent it by distance. Only the entities due this time
// (every state near the client's own character, every few states further off), each the newest
// state's record as a change against the client's record of it from an earlier one (the last it
// acknowledged having); what is left out the client keeps as it has it. The head and the inputs
// against the state it acknowledged; the entities that are gone, named.
struct EntityUpdate
{
	uint32_t netId = 0;
	uint32_t baseTick = 0; // the state whose record of it the client has (0: none: whole)
	// As a puppet's client is sent it (M253): its view (StateSpan::view) and its puppet, against the
	// base's (the client was sent it as a puppet there too).
	bool puppet = false;
	// M257: a puppet's record (and bodies) against the one the client has in its newest state, not
	// a base's: the server knows which (it acknowledged it, and was sent nothing of it since; the
	// server keeps a puppet's last few records), and the base it was in is no longer kept. Its puppet
	// whole; baseTick 0. And which record that is (the parts clients share are by it).
	const StateSpan* carried = nullptr;
	uint32_t version = 0;
};
// The kept state of a tick, read (null: not kept).
using LayoutOf = const StateLayout* ( * )( const void* context, uint32_t tick );
// What one state's updates for many clients share: an entity's change against a base is the same
// bytes whoever it goes to. Kept for one newest state (cleared when it is another's).
struct UpdateCache
{
	uint32_t tick = UINT32_MAX;
	std::unordered_map<uint64_t, std::vector<uint8_t>> parts; // (NetId << 32 | base tick) -> its bytes
};
// `frame` is the newest state (its layout `layoutOf( frame.tick )`), `base` the one the client
// acknowledged (null: none). `updates` and `removed` by NetId; `inputSlots` (in order) the slots
// whose inputs go (those whose characters are updated: the client keeps the others' as it has
// them). Every change is written sparse (runs of unchanged bytes skipped), an entity's part may
// come from `cache`, and the whole is deflated. False: a state named is not kept, or an entity is
// not in it.
// `sizes` (M257): each update's bytes, by `updates` (before packing). `events` (M257): the events at
// characters the client is sent (by character, NetId order; each before the state's tick).
// `unpacked` (M257): the message's bytes before packing (`out` is after).
bool EncodeUpdates( const StateFrame& frame, const StateFrame* base, const std::vector<EntityUpdate>& updates, const std::vector<uint32_t>& removed,
					const std::vector<PlayerSlot>& inputSlots, LayoutOf layoutOf, const void* context, std::vector<uint8_t>& out,
					UpdateCache* cache = nullptr, std::vector<uint32_t>* sizes = nullptr, const std::vector<CharacterEvent>* events = nullptr,
					size_t* unpacked = nullptr );
// After ReadType: which state, against which base.
bool ReadStateHeader( ByteReader& r, uint32_t& tick, uint32_t& baseTick );
// `base` must be the state named by the header (null when it names none).
bool DecodeState( ByteReader& r, uint32_t tick, const StateFrame* base, StateFrame& out );

// A client's states by tick, as it decoded them (null: not had).
using StateOf = const StateFrame* ( * )( const void* context, uint32_t tick );
// DecodeState for a message that may be updates: `previous` is the newest state the client has
// (null: none), on which they go; `stateOf` its states, the entities' bases. A whole state or a
// change against `base` decodes as DecodeState does.
bool DecodeState( ByteReader& r, uint32_t tick, const StateFrame* base, const StateFrame* previous, StateOf stateOf, const void* context,
				  StateFrame& out );

} // namespace cb::net
