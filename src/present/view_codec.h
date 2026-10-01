#pragma once

// A ViewFrame as bytes (view.h): what crosses between a source and a viewer that do not share a
// library, what a view file holds, and what a server will stream.
//
// A packet is one frame, whole or as a delta against a frame both sides have (the base). Deltas
// are per 32-bit word: an entity that did not change costs one bit, a player that moved costs the
// words of its transform and of the animation times that advanced, a board write costs one word.
// The session (map, schema, names) is only in a packet when it differs from the base's.
//
// Layout (little-endian, like everything else):
//   "CBV1", u64 serial, u64 base serial (0: none), f32 age, f32 alpha, f32 rate, u16 presses,
//   u8 flags, string state, [stats], [map], [schema], [names], [world]
// Decoding never trusts the bytes: counts are bounded and a short packet fails.

#include "view.h"

#include <cstdint>
#include <vector>

namespace cb::present
{

// Appends nothing to `out`: it is cleared first. `base` null (or without a world when the frame
// has one) makes a packet that stands alone. `ageSeconds` is how long ago the frame was published;
// the receiver's clock is its own, so only the age travels.
void EncodeView( const ViewFrame& frame, const ViewFrame* base, double ageSeconds, std::vector<uint8_t>& out );

// The serial of the frame a packet is a delta against (0: it stands alone, or it is not a packet).
uint64_t ViewPacketBase( const uint8_t* data, size_t size );

// False for bytes that are not a packet, or a delta whose base is not `base`. `out` must not be
// the base; it may be a frame decoded earlier from the same stream (what it already holds of the
// session is kept instead of copied). On failure `out` is left in an unspecified but valid state.
bool DecodeView( const uint8_t* data, size_t size, const ViewFrame* base, ViewFrame& out );

} // namespace cb::present
