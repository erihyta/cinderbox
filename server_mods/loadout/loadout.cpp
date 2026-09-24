// Loadout: which slot a player has out. 1 is empty hands, 2 is the pistol, 3 the melee bat.
//
// Empty hands also mean freelook: a weapon turns camera-facing on when it comes out, and only the
// loadout turns it off, so switching from one weapon to another never races two mods.
//
// It only publishes "loadout.slot"; other mods read it to decide whether their action applies
// (props spawn with empty hands, the pistol fires when it is out). That is the whole point of the
// board: mods cooperate through published values instead of knowing about each other.

#include "mod_api.h"

namespace
{

using namespace cb;
using namespace cb::mods;

class LoadoutMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "loadout";
	}

	void Declare( Declarations& declare ) override
	{
		m_slot = declare.Field( "loadout.slot", BoardType::Int );
		m_hands = declare.Action( "slot_1", "1" );
		m_pistol = declare.Action( "slot_2", "2" );
		m_melee = declare.Action( "slot_3", "3" );
	}

	void Tick( Context& ctx ) override
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Joining( slot ) )
			{
				ctx.Set( SlotTarget( slot ), m_slot, 1 );
				continue;
			}
			if ( ctx.InWorld( slot ) == false )
			{
				continue;
			}
			int32_t current = ctx.Get( ctx.PlayerNetId( slot ), m_slot );
			if ( ctx.Pressed( slot, m_hands ) && current != 1 )
			{
				ctx.Set( SlotTarget( slot ), m_slot, 1 );
				ctx.FaceCamera( SlotTarget( slot ), false );
			}
			else if ( ctx.Pressed( slot, m_pistol ) && current != 2 )
			{
				ctx.Set( SlotTarget( slot ), m_slot, 2 );
			}
			else if ( ctx.Pressed( slot, m_melee ) && current != 3 )
			{
				ctx.Set( SlotTarget( slot ), m_slot, 3 );
			}
		}
	}

private:
	FieldHandle m_slot;
	ActionHandle m_hands;
	ActionHandle m_pistol;
	ActionHandle m_melee;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_loadout()
{
	return std::make_unique<LoadoutMod>();
}
