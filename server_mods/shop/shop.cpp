// A shop: the example of an intent a mod declares ("shop.buy"), what a screen's button sends.
//
// The engine carries the intent: a CbClick with mod_intent "shop.buy" and intent_a (the offer's
// number) puts it in the player's own input, and this mod reads it on the tick it arrives
// (ctx.Intents). What it means is this file's: the price is taken from the player's coins, and
// the item is given (into its slots, onto a stack of its kind if there is one). It is the server's
// answer, not predicted: the screen shows it when the server's state arrives.
//
// Coins: a private field of each player (shop.coins: only its own client sees it), shop.start of
// them when it joins, one more for every kill (the combat mod's combat.killed).
//
//   offer 0   a frag grenade   1 coin
//   offer 1   a bat            1 coin
//   offer 2   a rifle          2 coins
//
// It says shop.bought (at the player, its value the offer) or shop.refused (too few coins, an
// offer it does not have), for the screen's sounds. Server options: shop.start (default 3).

#include "mod_api.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace cb;
using namespace cb::mods;

namespace
{

struct Offer
{
	const char* kind;
	int32_t price;
};
const Offer kOffers[] = { { "frag.grenade", 1 }, { "melee.bat", 1 }, { "gun.rifle", 2 } };

class ShopMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "shop";
	}

	void Declare( Declarations& declare ) override
	{
		m_buy = declare.Intent( "shop.buy" );
		m_coins = declare.Field( "shop.coins", BoardType::Int, BoardScope::Private );
		m_bought = declare.Event( "shop.bought" );
		m_refused = declare.Event( "shop.refused" );
		// The combat mod's (see combat.cpp): a = the killer, b = who died.
		m_killed = declare.Event( "combat.killed" );
	}

	void Start( Context& ctx ) override
	{
		m_start = int32_t( std::clamp( ctx.Option( "shop.start", 3.0 ), 0.0, 1000.0 ) );
		for ( const Offer& offer : kOffers )
		{
			m_kinds.push_back( ctx.Schema().FindItemKind( offer.kind ) );
		}
	}

	void Tick( Context& ctx ) override
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			if ( ctx.Joining( PlayerSlot( i ) ) )
			{
				ctx.Set( SlotTarget( PlayerSlot( i ) ), m_coins, m_start );
			}
		}
		// A kill is a coin for the killer.
		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			int killer = int( e.type ) == m_killed.index && e.netIdA != 0 && e.netIdA != e.netIdB ? ctx.SlotOf( e.netIdA ) : -1;
			if ( killer >= 0 )
			{
				ctx.Set( SlotTarget( PlayerSlot( killer ) ), m_coins, ctx.Get( e.netIdA, m_coins ) + 1 );
			}
		}
		// What the players asked for this tick.
		for ( const IntentSent& asked : ctx.Intents( m_buy ) )
		{
			uint32_t player = ctx.PlayerNetId( asked.slot );
			const Character* c = ctx.PlayerCharacter( asked.slot );
			int32_t coins = player != 0 ? ctx.Get( player, m_coins ) : 0;
			bool known = asked.a < m_kinds.size() && m_kinds[asked.a] >= 0;
			if ( known == false || c == nullptr || c->dead != 0 || coins < kOffers[asked.a].price )
			{
				ctx.Emit( m_refused, SlotTarget( asked.slot ), 0, int32_t( asked.a ), {} );
				continue;
			}
			ctx.Set( SlotTarget( asked.slot ), m_coins, coins - kOffers[asked.a].price );
			ctx.GiveItem( SlotTarget( asked.slot ), ItemKindHandle{ m_kinds[asked.a] } );
			ctx.Emit( m_bought, SlotTarget( asked.slot ), 0, int32_t( asked.a ), {} );
		}
	}

private:
	IntentHandle m_buy;
	FieldHandle m_coins;
	EventHandle m_bought;
	EventHandle m_refused;
	EventHandle m_killed;
	int32_t m_start = 3;
	std::vector<int> m_kinds; // the offers' item kinds (-1: no mod declares it)
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_shop()
{
	return std::make_unique<ShopMod>();
}
