#pragma once

// A ViewFrame as bytes (view.h): what crosses between a source and a viewer that do not share a
// library, and what a view file holds.
//
// A packet is one frame, whole or as a delta against a frame both sides have (the base). Deltas
// are per 32-bit word: an entity that did not change costs one bit, a player that moved costs the
// words of its transform and of the animation times that advanced, a board write costs one word.
// The session (map, schema, names) is only in a packet when it differs from the base's.
//
// Two precisions. Exact: every word that differs travels as it is, so the receiver has the
// sender's frame bit for bit (between two libraries of one process, in a file to study). Compact:
// a small file. Positions on a 1/512 m grid and animation values on a 1/1024 grid,
// sent as how far they moved; rotations in 32 bits; no velocity; no transform for an item in a
// hand; no inputs (a viewer draws none of them). A compact delta needs a base the receiver decoded
// from compact packets: both sides then stand on the same grid.
//
// Layout (little-endian, like everything else):
//   "CBV4", u64 serial, u64 base serial (0: none), f32 age, f32 alpha, f32 rate, u16 presses,
//   u16 stride, u8 flags, string state, [stats], [map], [schema], [names], [world]
// Decoding never trusts the bytes: counts are bounded and a short packet fails.

#include "view.h"

#include <cstdint>
#include <vector>

namespace cb::present
{

enum class ViewPrecision
{
	Exact,
	Compact,
};

// Where a packet's bytes went, for whoever wants to make packets smaller (cb_replay view).
// EncodeView adds to it.
struct ViewCost
{
	size_t header = 0;	  // serial, timing, state, the world's counters
	size_t stats = 0;
	size_t session = 0;	  // map, schema, names
	size_t inputs = 0;	  // exact packets only
	size_t events = 0;	  // the global board and the two event rings
	size_t lists = 0;	  // which entities there are, and which of them changed
	size_t rest = 0;	  // identity, shape, flags, step counts
	size_t positions = 0;
	size_t rotations = 0;
	size_t animation = 0;
	size_t boards = 0;
	size_t ragdolls = 0;
	size_t exact = 0;	  // exact packets: the entities and ragdolls, undivided
};

// Appends nothing to `out`: it is cleared first. `base` null (or without a world when the frame
// has one) makes a packet that stands alone. `ageSeconds` is how long ago the frame was published;
// the receiver's clock is its own, so only the age travels.
void EncodeView( const ViewFrame& frame, const ViewFrame* base, double ageSeconds, std::vector<uint8_t>& out,
				 ViewPrecision precision = ViewPrecision::Exact, ViewCost* cost = nullptr );

// Whether a packet is compact (false also for bytes that are not a packet).
bool ViewPacketCompact( const uint8_t* data, size_t size );

// The serial of the frame a packet is a delta against (0: it stands alone, or it is not a packet).
uint64_t ViewPacketBase( const uint8_t* data, size_t size );

// False for bytes that are not a packet, or a delta whose base is not `base`. `out` must not be
// the base; it may be a frame decoded earlier from the same sender (what it already holds of the
// session is kept instead of copied). On failure `out` is left in an unspecified but valid state.
bool DecodeView( const uint8_t* data, size_t size, const ViewFrame* base, ViewFrame& out );

} // namespace cb::present
