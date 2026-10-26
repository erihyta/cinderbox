// Possess: a test of driving bodies (ROADMAP.md, step 8). Off by default:
//
//   cb_server --mods <the others>,possess
//
// Its key (P) makes the player drive the nearest other body it may: a crate, a ball, a character's
// body nobody drives (one the player left before, say). The body it leaves stands where it is,
// nobody's; a prop it leaves is a loose body again. Nothing else: who may drive what is a game's
// rule, and this one allows everything the engine does.

#include "mod_api.h"

namespace
{

using namespace cb;
using namespace cb::mods;

// How far the nearest body may be.
constexpr float kReach = 15.0f;

class PossessMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "possess";
	}

	void Declare( Declarations& declare ) override
	{
		m_take = declare.Action( "possess.nearest", "P" );
	}

	void Tick( Context& ctx ) override
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			const PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Pressed( slot, m_take ) == false )
			{
				continue;
			}
			const uint32_t mine = ctx.PlayerNetId( slot );
			const Transform* at = ctx.EntityTransform( mine );
			if ( at == nullptr )
			{
				continue;
			}
			// (Nearest first.)
			for ( const NearBody& body : ctx.BodiesNear( at->position, kReach ) )
			{
				if ( body.netId != mine && ctx.CanPossess( body.netId ) )
				{
					ctx.Possess( slot, body.netId );
					break;
				}
			}
		}
	}

private:
	ActionHandle m_take;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_possess()
{
	return std::make_unique<PossessMod>();
}
