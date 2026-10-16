// Chat: players type, everyone reads. The engine only carries the words (a channel a mod declares,
// Context::Typed, Context::Say); the rules are here: how long a line may be, how fast a player may
// talk. A server without this mod has no chat: its box comes with this mod's look (ui/hud_chat).
//
//   chat.max_length       120   characters a line keeps (bytes of UTF-8, cut on a character)
//   chat.burst            4     lines a player may say ...
//   chat.burst_seconds    8     ... within this long; more, and it is told to slow down
//
// Switched off like any mod: started without it (cb_server --mods ...), or a file named
// "disabled" in this folder.

#include "mod_api.h"
#include "protocol.h"

#include <deque>

namespace
{

using namespace cb;
using namespace cb::mods;

class ChatMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "chat";
	}

	void Declare( Declarations& declare ) override
	{
		declare.TextChannel( "chat" );
	}

	void Start( Context& ctx ) override
	{
		m_maxLength = size_t( std::max( 1.0, ctx.Option( "chat.max_length", 120.0 ) ) );
		m_burst = size_t( std::max( 1.0, ctx.Option( "chat.burst", 4.0 ) ) );
		m_burstTicks = uint32_t( std::max( 0.0, ctx.Option( "chat.burst_seconds", 8.0 ) ) / double( ctx.Config().TimeStep() ) );
	}

	void Tick( Context& ctx ) override
	{
		for ( const TypedText& typed : ctx.Typed() )
		{
			if ( typed.channel != "chat" || typed.slot >= kMaxPlayers )
			{
				continue;
			}
			std::deque<uint32_t>& said = m_said[typed.slot];
			while ( said.empty() == false && ctx.Tick() - said.front() >= m_burstTicks )
			{
				said.pop_front();
			}
			if ( said.size() >= m_burst )
			{
				ctx.Say( "chat", "Slow down: a few lines at a time.", kNoSlot, typed.slot );
				continue;
			}
			said.push_back( ctx.Tick() );
			ctx.Say( "chat", net::SanitizeText( typed.text, m_maxLength ), typed.slot );
		}
	}

private:
	size_t m_maxLength = 120;
	size_t m_burst = 4;
	uint32_t m_burstTicks = 480;
	std::deque<uint32_t> m_said[kMaxPlayers]; // the ticks each player last said something
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_chat()
{
	return std::make_unique<ChatMod>();
}
