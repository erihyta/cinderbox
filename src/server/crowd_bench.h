#pragma once

// The crowd bench (M251): what a server would send its players, measured on fake players
// (ServerOptions::fakePlayers) instead of sent. GameServer counts it; Bench() reads it, and
// FormatBench says it per player and per second.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cb
{

struct CrowdBench
{
	uint32_t fakes = 0;	   // fake players
	uint64_t ticks = 0;	   // ticks counted (after the warm-up: ServerOptions::fakeWarmupSeconds)
	uint32_t fromTick = 0; // the first counted
	// The most a fake player was thought to have, against the most entities a state had: more only
	// by the gone ones not acknowledged yet (a list that keeps growing is a leak).
	uint64_t knownMost = 0, entitiesMost = 0;
	uint32_t tickRate = 60;
	uint32_t resyncs = 0;  // fake players that fell behind the frames and started over

	// What would have gone to the fake players, summed over all of them: payload bytes and
	// messages, by kind.
	struct Kind
	{
		uint64_t messages = 0;
		uint64_t bytes = 0;
	};
	Kind frames, states, other;
	uint64_t statesWhole = 0; // against no base
	// The wire, estimated (WireBytes): each update's messages to a player in as few datagrams as
	// they fit in, with their headers.
	uint64_t datagrams = 0;
	uint64_t wire = 0;

	// A state's parts, on a sample of the messages (every 8th fake player's): each sampled message
	// encoded again without a part. The differences are about what the part costs (deflating
	// shares bytes between parts, so they do not add up exactly).
	uint64_t sampled = 0;
	uint64_t sampledBytes = 0;
	uint64_t withoutItems = 0;	// without the others' held items
	uint64_t withoutInputs = 0; // without the others' inputs
	uint64_t ownOnly = 0;		// only its own character and what it holds (and the head)
	uint64_t headOnly = 0;		// nothing but the head (the globals) and the header
	uint64_t othersUpdated = 0; // others' characters updated, in the sampled messages
	uint64_t othersItemsUpdated = 0;
	// Their puppets (M253): how many went against a base, and those changes' bytes (before packing);
	// how many changed where they are, their facing, their look, their flags, and their layers (how
	// many layers in all).
	uint64_t puppets = 0, puppetBytes = 0;
	uint64_t puppetPlaces = 0, puppetFacings = 0, puppetAims = 0, puppetFlags = 0, puppetLayerChanges = 0, puppetLayers = 0;
	// Bytes that changed in others' records against the record the player had, by component
	// (Simulation::StateComponents' order): characters, and the items they hold. Records sent
	// whole (first seen) apart.
	std::vector<std::string> components;
	std::vector<uint32_t> componentSizes;
	std::vector<uint64_t> changedCharacters, changedItems;
	uint64_t wholeRecords = 0, wholeRecordBytes = 0;
	// Why whole: new to the player, sent before but not acknowledged yet, its base no longer kept;
	// and how many of them were characters (the rest: held items).
	uint64_t wholeNew = 0, wholeUnacknowledged = 0, wholeOld = 0, wholeCharacters = 0;
	// Records carried (M257): their base too old, against the record the player has; how many had
	// changed since.
	uint64_t carried = 0, carriedChanged = 0;
	// The events at characters sent with them (M257), and the messages without them.
	uint64_t characterEvents = 0, withoutEvents = 0;

	// Frames, once a tick: the newest frame alone, and again without each part; what they held.
	uint64_t frameAlone = 0, frameNoCommands = 0, frameNoPhysics = 0, frameNoPushers = 0;
	std::array<uint64_t, 32> commands{}; // by CommandType
	uint64_t physicsEvents = 0, pushers = 0;
	// The simulation's own events (they ride in every state's head, in their rings): impacts, and
	// mod events by name (ModSchema::events; a clip's markers among them).
	uint64_t impacts = 0;
	std::vector<std::string> eventNames;
	std::vector<uint64_t> events;
	uint32_t impactCount = 0, modEventCount = 0; // (where the last count stopped)

	// The server's time for it all, ms: planning and encoding states, encoding frames, and the
	// fake players' own inputs (not a real server's work).
	double planMs = 0.0, encodeMs = 0.0, framesMs = 0.0, fakeInputMs = 0.0;
	// The bench's own: sampling states by part (in the server's sending time, not a real server's work).
	double sampleMs = 0.0;
};

// ENet over UDP and IPv4: a datagram is at most kEnetMtu bytes (ENet's header and commands), plus
// 28 of UDP and IP.
inline constexpr size_t kEnetMtu = 1392;
// Bytes on the wire for the messages one player is sent in one update (payload size, reliable),
// packed into datagrams as ENet packs them (a message too big for one is fragmented); `datagrams`
// gets how many.
uint64_t WireBytes( const std::vector<std::pair<uint32_t, bool>>& messages, uint64_t& datagrams );

// The bytes that differ between two records of an entity (Simulation::SaveState's: a component
// mask, then each component the mask names, `sizes` long in order), added to `out` by component
// (sized like `sizes`). A component only one of them has counts whole.
void CountChangedBytes( const uint8_t* now, uint32_t nowSize, const uint8_t* base, uint32_t baseSize, const std::vector<uint32_t>& sizes,
						std::vector<uint64_t>& out );

// The report: per player and per second, by part.
std::string FormatBench( const CrowdBench& bench );

} // namespace cb
