// Item: what using an item is called, whatever the item is.
//
// The cues an item's look answers do not name the item:
//
//   item.use          the item in the hand did what it does (a shot, a swing, a sip)
//   item.use_failed   it was tried and could not (an empty magazine)
//   item.use2         its second use (the right mouse button)
//   item.hit          a use landed on something: value = what it did (damage), b = what was hit
//   item.reload       it is being made ready again
//
// Each is at the player (a = who holds the item). What it looks and sounds like is the item's own
// scene's business: its CbReaction nodes on these cues, which only the item in the hand plays (an
// item put away is silent), and its animation pack's states. So items differ by their scenes, and
// a new one needs no code.
//
// What a use *does* is another mod's: one that rules a kind of item (the gun mod: anything whose
// scene says gun.damage) says the cues itself, when its rule says so, and marks the kinds it rules
// with the item property "item.ruled". For every other item this mod says them on the press: an
// item with no rules at all (a prop to wave about) still has its look.

#include "mod_api.h"

namespace
{

using namespace cb;
using namespace cb::mods;

class ItemMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "item";
	}

	void Declare( Declarations& declare ) override
	{
		// a = who holds the item. For a use that reaches somewhere (a shot): b = what it reached
		// (0: nothing), point = where it started, vector = where it ended.
		m_use = declare.Event( "item.use" );
		declare.Event( "item.use_failed" );
		m_use2 = declare.Event( "item.use2" );
		// a = who holds the item, b = what was hit, value = what it did, point = where, vector = normal.
		declare.Event( "item.hit" );
		declare.Event( "item.reload" );
		m_second = declare.Action( "use2", "MouseRight" );
		declare.Action( "reload", "R" );
		m_hand = declare.Socket( "RightHand" );
	}

	void Tick( Context& ctx ) override
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			const Character* c = ctx.PlayerCharacter( slot );
			uint32_t item = c != nullptr && c->dead == 0 && c->frozen == 0 ? ctx.HeldItem( slot, m_hand ) : 0;
			if ( item == 0 )
			{
				continue;
			}
			ItemKindHandle kind = ctx.ItemKindOf( item );
			// (A kind some mod rules: that mod says when.)
			if ( kind.Valid() == false || ctx.ItemProperty( kind, "item.ruled", 0.0f ) != 0.0f )
			{
				continue;
			}
			if ( ctx.Used( slot, kind ) )
			{
				ctx.Emit( m_use, SlotTarget( slot ) );
			}
			if ( ctx.Pressed( slot, m_second ) )
			{
				ctx.Emit( m_use2, SlotTarget( slot ) );
			}
		}
	}

private:
	EventHandle m_use;
	EventHandle m_use2;
	ActionHandle m_second;
	SocketHandle m_hand;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_item()
{
	return std::make_unique<ItemMod>();
}
