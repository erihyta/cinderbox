#include "crowd_bench.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cb
{

namespace
{

// ENet's headers (enet/protocol.h): a datagram's (peer, sent time), an unsequenced or unreliable
// message's, a reliable one's, a fragment's; UDP and IPv4 around each datagram.
constexpr size_t kDatagramHeader = 4;
constexpr size_t kUnreliableHeader = 8;
constexpr size_t kReliableHeader = 6;
constexpr size_t kFragmentHeader = 24;
constexpr size_t kUdpIp = 28;

const char* CommandName( size_t type )
{
	static const char* const names[] = { "?",		 "SetField", "Event",		 "SpawnProp", "Destroy",	  "Impulse",   "Kill",	 "Respawn",
										 "Freeze",	 "Aim",		 "Facing",		 "Stance",	  "SpawnItem",	  "SwapLayer", "DropItem", "PickUpItem",
										 "MoveItem", "SetMove",	 "Break",		 "SetCharacter", "SpawnBody", "Possess",	  "Drive",	   "Toss" };
	return type < sizeof( names ) / sizeof( names[0] ) ? names[type] : "?";
}

} // namespace

uint64_t WireBytes( const std::vector<std::pair<uint32_t, bool>>& messages, uint64_t& datagrams )
{
	uint64_t bytes = 0;
	size_t room = 0; // what is left in the datagram being filled (0: none open)
	for ( const auto& [size, reliable] : messages )
	{
		const size_t header = reliable ? kReliableHeader : kUnreliableHeader;
		if ( header + size + kDatagramHeader <= kEnetMtu )
		{
			if ( header + size > room )
			{
				datagrams += 1;
				bytes += kUdpIp + kDatagramHeader;
				room = kEnetMtu - kDatagramHeader;
			}
			bytes += header + size;
			room -= header + size;
			continue;
		}
		// Fragments, each a datagram of its own.
		const size_t each = kEnetMtu - kDatagramHeader - kFragmentHeader;
		const size_t fragments = ( size + each - 1 ) / each;
		datagrams += fragments;
		bytes += fragments * ( kUdpIp + kDatagramHeader + kFragmentHeader ) + size;
		room = 0;
	}
	return bytes;
}

void CountChangedBytes( const uint8_t* now, uint32_t nowSize, const uint8_t* base, uint32_t baseSize, const std::vector<uint32_t>& sizes,
						std::vector<uint64_t>& out )
{
	if ( nowSize < 4 || baseSize < 4 || out.size() < sizes.size() )
	{
		return;
	}
	uint32_t nowMask = 0, baseMask = 0;
	std::memcpy( &nowMask, now, 4 );
	std::memcpy( &baseMask, base, 4 );
	size_t a = 4, b = 4;
	for ( size_t i = 0; i < sizes.size() && i < 32; ++i )
	{
		const bool inNow = ( nowMask >> i ) & 1u;
		const bool inBase = ( baseMask >> i ) & 1u;
		const size_t size = sizes[i];
		if ( inNow && a + size > nowSize )
		{
			return;
		}
		if ( inBase && b + size > baseSize )
		{
			return;
		}
		if ( inNow && inBase )
		{
			for ( size_t k = 0; k < size; ++k )
			{
				out[i] += now[a + k] != base[b + k] ? 1 : 0;
			}
		}
		else if ( inNow )
		{
			out[i] += size;
		}
		a += inNow ? size : 0;
		b += inBase ? size : 0;
	}
}

std::string FormatBench( const CrowdBench& b )
{
	std::string out;
	char line[1024];
	auto add = [&]( const char* text ) { out += text; };
	const double seconds = b.tickRate > 0 ? double( b.ticks ) / double( b.tickRate ) : 0.0;
	if ( b.fakes == 0 || seconds <= 0.0 )
	{
		return "crowd bench   nothing counted (no fake players, or no ticks)\n";
	}
	const double perPlayerSecond = 1.0 / ( double( b.fakes ) * seconds );
	auto kbits = [&]( uint64_t bytes ) { return double( bytes ) * 8.0 / 1000.0 * perPlayerSecond; };
	const uint64_t payload = b.frames.bytes + b.states.bytes + b.other.bytes;

	std::snprintf( line, sizeof( line ), "crowd bench   %u fake players, %.1f s counted at %u Hz (after %.1f s of warming up); %u started over\n", b.fakes,
				   seconds, b.tickRate, double( b.fromTick ) / double( b.tickRate ), b.resyncs );
	add( line );
	std::snprintf( line, sizeof( line ), "  what a player was thought to have: %llu entities at most, against %llu in a state at most\n",
				   (unsigned long long)b.knownMost, (unsigned long long)b.entitiesMost );
	add( line );
	std::snprintf( line, sizeof( line ),
				   "to a player   %.0f kbit/s on the wire (estimated: %.0f of headers, %.1f datagrams a second); payload %.0f: states %.0f, "
				   "frames %.0f, other %.0f\n",
				   kbits( b.wire ), kbits( b.wire - std::min( b.wire, payload ) ), double( b.datagrams ) * perPlayerSecond, kbits( payload ),
				   kbits( b.states.bytes ), kbits( b.frames.bytes ), kbits( b.other.bytes ) );
	add( line );
	std::snprintf( line, sizeof( line ), "server out    %.1f Mbit/s for %u players\n", double( b.wire ) * 8.0 / 1e6 / seconds, b.fakes );
	add( line );

	if ( b.states.messages > 0 )
	{
		std::snprintf( line, sizeof( line ), "a state       %.0f B on average, %.1f%% of them whole\n", double( b.states.bytes ) / double( b.states.messages ),
					   100.0 * double( b.statesWhole ) / double( b.states.messages ) );
		add( line );
	}
	if ( b.sampled > 0 )
	{
		const double n = double( b.sampled );
		const double head = double( b.headOnly ) / n;
		const double own = double( b.ownOnly - std::min( b.ownOnly, b.headOnly ) ) / n;
		const double others = double( b.sampledBytes - std::min( b.sampledBytes, b.ownOnly ) ) / n;
		const double items = double( b.sampledBytes - std::min( b.sampledBytes, b.withoutItems ) ) / n;
		const double inputs = double( b.sampledBytes - std::min( b.sampledBytes, b.withoutInputs ) ) / n;
		const double characters = double( b.othersUpdated ) / n;
		std::snprintf( line, sizeof( line ),
					   "  its parts   (%llu sampled) the head %.0f B, its own character %.0f, others %.0f: %.1f characters updated with %.1f "
					   "held items; of the others' part, held items ~%.0f, inputs ~%.0f\n",
					   (unsigned long long)b.sampled, head, own, others, characters, double( b.othersItemsUpdated ) / n, items, inputs );
		add( line );
		if ( b.othersUpdated > 0 )
		{
			std::snprintf( line, sizeof( line ), "  per other character updated: %.1f B on the wire (with what it holds and its input)\n",
						   others * n / double( b.othersUpdated ) );
			add( line );
		}
	}
	auto changed = [&]( const char* who, const std::vector<uint64_t>& bytes, uint64_t updates ) {
		if ( updates == 0 )
		{
			return;
		}
		std::string parts;
		uint64_t total = 0;
		for ( size_t i = 0; i < bytes.size() && i < b.components.size(); ++i )
		{
			total += bytes[i];
		}
		for ( size_t i = 0; i < bytes.size() && i < b.components.size(); ++i )
		{
			if ( bytes[i] * 200 < total ) // (under half a percent)
			{
				continue;
			}
			char part[96];
			std::snprintf( part, sizeof( part ), "%s%s %.1f", parts.empty() ? "" : ", ", b.components[i].c_str(), double( bytes[i] ) / double( updates ) );
			parts += part;
		}
		std::snprintf( line, sizeof( line ), "  changed in %s' records, bytes an update (before packing): %.1f: %s\n", who,
					   double( total ) / double( updates ), parts.c_str() );
		add( line );
	};
	changed( "others' characters", b.changedCharacters, b.othersUpdated );
	changed( "their held items", b.changedItems, b.othersItemsUpdated );
	if ( b.puppets > 0 )
	{
		const double n = double( b.puppets );
		std::snprintf( line, sizeof( line ),
					   "  their puppets: %.1f B a change; of the changes %.0f%% moved, %.0f%% turned, %.0f%% looked elsewhere, %.0f%% flags, %.0f%% "
					   "layers (%.1f layers each)\n",
					   double( b.puppetBytes ) / n, 100.0 * double( b.puppetPlaces ) / n, 100.0 * double( b.puppetFacings ) / n,
					   100.0 * double( b.puppetAims ) / n, 100.0 * double( b.puppetFlags ) / n, 100.0 * double( b.puppetLayerChanges ) / n,
					   b.puppetLayerChanges > 0 ? double( b.puppetLayers ) / double( b.puppetLayerChanges ) : 0.0 );
		add( line );
	}
	if ( b.wholeRecords > 0 )
	{
		std::snprintf( line, sizeof( line ),
					   "  records sent whole: %.1f a state, %.0f B each (before packing); %.0f%% characters; new to the player %.1f, not "
					   "acknowledged yet %.1f, base too old %.1f\n",
					   double( b.wholeRecords ) / double( std::max<uint64_t>( b.sampled, 1 ) ), double( b.wholeRecordBytes ) / double( b.wholeRecords ),
					   100.0 * double( b.wholeCharacters ) / double( b.wholeRecords ), double( b.wholeNew ) / double( std::max<uint64_t>( b.sampled, 1 ) ),
					   double( b.wholeUnacknowledged ) / double( std::max<uint64_t>( b.sampled, 1 ) ),
					   double( b.wholeOld ) / double( std::max<uint64_t>( b.sampled, 1 ) ) );
		add( line );
	}

	if ( b.ticks > 0 )
	{
		const double t = double( b.ticks );
		std::snprintf( line, sizeof( line ),
					   "a frame       %.0f B alone (commands ~%.0f, physics events ~%.0f, pushers ~%.0f); %.1f physics events and %.1f pushers' "
					   "places a tick; each player gets %.0f B of frames a tick (repeats included)\n",
					   double( b.frameAlone ) / t, double( b.frameAlone - std::min( b.frameAlone, b.frameNoCommands ) ) / t,
					   double( b.frameAlone - std::min( b.frameAlone, b.frameNoPhysics ) ) / t,
					   double( b.frameAlone - std::min( b.frameAlone, b.frameNoPushers ) ) / t, double( b.physicsEvents ) / t, double( b.pushers ) / t,
					   double( b.frames.bytes ) / t / double( b.fakes ) );
		add( line );
		std::string commands;
		for ( size_t i = 0; i < b.commands.size(); ++i )
		{
			if ( b.commands[i] == 0 )
			{
				continue;
			}
			char part[64];
			std::snprintf( part, sizeof( part ), "%s%s %.2f", commands.empty() ? "" : ", ", CommandName( i ), double( b.commands[i] ) / t );
			commands += part;
		}
		std::snprintf( line, sizeof( line ), "  commands a tick: %s\n", commands.empty() ? "none" : commands.c_str() );
		add( line );
		// The simulation's events (in every state's head), the most frequent first.
		std::vector<std::pair<uint64_t, size_t>> byCount;
		uint64_t modEvents = 0;
		for ( size_t i = 0; i < b.events.size(); ++i )
		{
			modEvents += b.events[i];
			if ( b.events[i] > 0 )
			{
				byCount.push_back( { b.events[i], i } );
			}
		}
		std::sort( byCount.begin(), byCount.end(), []( const auto& x, const auto& y ) { return x.first > y.first; } );
		std::string events;
		for ( size_t k = 0; k < byCount.size() && k < 8; ++k )
		{
			char part[96];
			const size_t i = byCount[k].second;
			std::snprintf( part, sizeof( part ), "%s%s %.2f", events.empty() ? "" : ", ", i < b.eventNames.size() ? b.eventNames[i].c_str() : "?",
						   double( byCount[k].first ) / t );
			events += part;
		}
		std::snprintf( line, sizeof( line ), "  the simulation's events a tick: %.2f impacts, %.2f mod events (%s)\n", double( b.impacts ) / t,
					   double( modEvents ) / t, events.empty() ? "none" : events.c_str() );
		add( line );
		std::snprintf( line, sizeof( line ), "server time   a tick: planning states %.2f ms, encoding them %.2f, frames %.2f (the fake players' inputs %.2f)\n",
					   b.planMs / t, b.encodeMs / t, b.framesMs / t, b.fakeInputMs / t );
		add( line );
	}
	return out;
}

} // namespace cb
