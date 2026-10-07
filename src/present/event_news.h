#pragma once

// Which of a frame's mod events are news to a viewer.
//
// Mod events are a ring in the simulation's state, and a viewer that predicts sees the ring change
// under it: its own click's event is there first, alone, and when the server's frame for that tick
// arrives, the commands' events of that tick come before it. So what is new cannot be told by an
// event's place in the ring or by a count. It is told by what the event is (its tick, type,
// entities and value): each one is shown as many times as the ring has it, less the times it was
// shown already, and one a rollback takes back and brings again is not shown twice.

#include "events.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace cb::present
{

class ModEventNews
{
public:
	// The places in `ring` (kModEventHistory records, `count` recorded in all) of the events to
	// show now, oldest first. `reset`: a new world, of which nothing is news.
	std::vector<uint32_t> Sync( const ModEventRecord* ring, uint32_t count, uint32_t tick, bool reset )
	{
		std::vector<uint32_t> news;
		std::vector<Shown> now;
		const uint32_t held = std::min( count, kModEventHistory );
		for ( uint32_t i = 0; i < held; ++i )
		{
			const uint32_t at = ( count - held + i ) % kModEventHistory;
			const ModEventRecord& record = ring[at];
			const Shown key{ record.tick, record.netIdA, record.netIdB, record.value, record.type, 0 };
			auto seen = Find( now, key );
			if ( seen == now.end() )
			{
				now.push_back( key );
				seen = now.end() - 1;
			}
			seen->times += 1;
			auto before = Find( m_shown, key );
			if ( reset == false && ( before == m_shown.end() || before->times < seen->times ) )
			{
				news.push_back( at );
			}
		}
		// An event stays known for as long as a rollback could still bring it back, in the ring or not.
		for ( const Shown& was : m_shown )
		{
			auto still = Find( now, was );
			if ( still != now.end() )
			{
				still->times = std::max( still->times, was.times );
			}
			else if ( reset == false && was.tick + kKeepTicks > tick && now.size() < 4 * kModEventHistory )
			{
				now.push_back( was );
			}
		}
		m_shown = std::move( now );
		return news;
	}

private:
	struct Shown
	{
		uint32_t tick;
		uint32_t a;
		uint32_t b;
		int32_t value;
		uint16_t type;
		uint16_t times;
	};
	// Longer than any prediction window.
	static constexpr uint32_t kKeepTicks = 120;

	static std::vector<Shown>::iterator Find( std::vector<Shown>& in, const Shown& key )
	{
		return std::find_if( in.begin(), in.end(), [&]( const Shown& s ) {
			return s.tick == key.tick && s.type == key.type && s.a == key.a && s.b == key.b && s.value == key.value;
		} );
	}

	std::vector<Shown> m_shown;
};

} // namespace cb::present
