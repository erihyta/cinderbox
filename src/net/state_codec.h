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
#include "types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cb::net
{

inline constexpr uint32_t kNoStateBase = UINT32_MAX;
inline constexpr size_t kMaxStateBytes = 16u << 20;

// What a client holds of one of the server's states: its tick (the state in which the server's
// Tick() == tick), the inputs the server used for the tick before, and the state.
struct StateFrame
{
	uint32_t tick = 0;
	std::array<PlayerInput, kMaxPlayers> inputs{};
	std::vector<uint8_t> state;
};

// `base` null: the state whole.
void EncodeState( const StateFrame& frame, const StateFrame* base, std::vector<uint8_t>& out );

// After ReadType: which state, against which base.
bool ReadStateHeader( ByteReader& r, uint32_t& tick, uint32_t& baseTick );
// `base` must be the state named by the header (null when it names none).
bool DecodeState( ByteReader& r, uint32_t tick, const StateFrame* base, StateFrame& out );

} // namespace cb::net
