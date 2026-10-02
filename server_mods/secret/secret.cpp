// Secret: the example of a private field. Off unless the server asks for it:
//
//   cb_server --mod-option secret.numbers=1
//
// Each player is then given a number from 1 to 99 when it joins, in the private field
// "secret.number": the server tells it to that player and to nobody else. It is not in the
// simulation, so no other client has it to find, and a look reads it by name like any field
// ("Your number: {secret.number}"), for the viewer's own player only.
//
// A private field is for what a player must not know about another: a role, a hand of cards, a
// hidden objective. What happens in the world (where someone is, what they hold) is the
// simulation's, which every simulating client has.

#include "mod_api.h"

namespace
{

using namespace cb;
using namespace cb::mods;

class SecretMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "secret";
	}

	void Declare( Declarations& declare ) override
	{
		m_number = declare.Field( "secret.number", BoardType::Int, BoardScope::Private );
	}

	void Start( Context& ctx ) override
	{
		m_enabled = ctx.Option( "secret.numbers", 0.0 ) != 0.0;
	}

	void Tick( Context& ctx ) override
	{
		if ( m_enabled == false )
		{
			return;
		}
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Joining( slot ) )
			{
				ctx.Set( SlotTarget( slot ), m_number, 1 + int32_t( ctx.Random() % 99 ) );
			}
		}
	}

private:
	FieldHandle m_number;
	bool m_enabled = false;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_secret()
{
	return std::make_unique<SecretMod>();
}
