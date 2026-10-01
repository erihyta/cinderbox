// Expire: items left lying do not stay forever.
//
// An item that someone held and then left in the world (dropped, thrown, let go of on death) is
// removed after --mod-option expire.seconds=N (60 by default; 0 keeps everything). Picking it up
// again stops the clock; dropping it starts a new one. Items nobody ever held (a map's own, or the
// ones the pickup mod seeds) are not this mod's to remove.
//
// It needs no look: the item's own "destroying" cue fires, like for anything that goes.

#include "mod_api.h"

#include <map>

namespace
{

using namespace cb;
using namespace cb::mods;

struct Seen
{
	bool held = false;	// someone has held it
	uint32_t since = 0; // the tick it was first seen lying after that (0: it is held)
};

class ExpireMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "expire";
	}

	void Declare( Declarations& ) override
	{
	}

	void Start( Context& ctx ) override
	{
		double seconds = ctx.Option( "expire.seconds", 60.0 );
		m_ticks = seconds > 0.0 ? uint32_t( seconds * double( ctx.Config().tickRate ) + 0.5 ) : 0;
	}

	void Tick( Context& ctx ) override
	{
		uint32_t tick = ctx.Tick();
		if ( m_ticks == 0 )
		{
			return;
		}
		// Every tick: an item held for a moment between two looks would never be known as held.
		std::map<uint32_t, Seen> next;
		for ( uint32_t item : ctx.Items() )
		{
			auto known = m_seen.find( item );
			Seen seen = known != m_seen.end() ? known->second : Seen{};
			if ( ctx.ItemHolder( item ) != 0 )
			{
				seen.held = true;
				seen.since = 0;
			}
			else if ( seen.held )
			{
				if ( seen.since == 0 )
				{
					seen.since = tick;
				}
				else if ( tick - seen.since >= m_ticks )
				{
					ctx.Destroy( item );
					continue;
				}
			}
			next[item] = seen;
		}
		m_seen.swap( next ); // items that are gone drop out
	}

private:
	uint32_t m_ticks = 0;
	std::map<uint32_t, Seen> m_seen;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_expire()
{
	return std::make_unique<ExpireMod>();
}
