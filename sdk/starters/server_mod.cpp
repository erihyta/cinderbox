// MODNAME: a server mod, made by tools\sdk.ps1 -New MODNAME.
//
// This file is the rules: what the server decides. It runs on the server only. What things look
// like, and what a key does to how a player moves, is the mod's client project
// (server_mods/MODNAME/client): scenes, no code.
//
// It declares exactly the names the starter scenes use, so the project works as it is made:
//
//   MODNAME.item       an item kind      prefabs/MODNAME.tscn (a CbItem: its body, grips, look)
//   MODNAME.used       an event          sent when a player uses the item: its reactions play
//   MODNAME_use        an action (Q)     the starter motion's key (motion_sets/MODNAME_moves.tscn)
//   MODNAME.charges    a field           how many of that motion a player has left
//   MODNAME.moved      an event          the motion sends it
//   MODNAME.moves      a motion set      baked from motion_sets/MODNAME_moves.tscn
//   MODNAME.animations an animation pack baked from animation_packs/MODNAME_animations.tscn
//
// Rename, add and remove freely: the editor knows these names after the next build of the server
// (it offers them, and warns about one nobody declares), and publishing the look checks them.
//
// After a change here: build (cmake --build --preset clang-release) and restart the server.
// After a change in the client project: tools\publish_mod.ps1 -Mod MODNAME, and restart both.
// docs/server-mods.md has the whole API.

#include "mod_api.h"

#include <algorithm>
#include <array>

using namespace cb;
using namespace cb::mods;

namespace
{

class MODCLASSMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "MODNAME";
	}

	// What the mod adds to the game's vocabulary. Clients get these names when they join.
	void Declare( Declarations& declare ) override
	{
		// The item: every life starts with one (the inventory mod asks "inventory.start"), and the
		// starter animation pack plays on whoever holds it.
		m_item = declare.ItemKind( "MODNAME.item" );
		declare.ItemProperty( m_item, "inventory.start", 1.0f );
		// A slot for it: the stock mods fill four, and the item's scene asks for the fifth (its CbItem's
		// slot). The server has as many slots as the mod that asks for the most.
		declare.Slots( 5 );
		declare.ItemLayers( m_item, declare.AnimPack( "MODNAME.animations" ) );
		m_used = declare.Event( "MODNAME.used" );

		// The motion (a short burst along where the player walks) reads and writes these.
		m_use = declare.Action( "MODNAME_use", "Q" );
		m_charges = declare.Field( "MODNAME.charges", BoardType::Int );
		m_moved = declare.Event( "MODNAME.moved" );
		m_moves = declare.Motions( "MODNAME.moves" );
	}

	// Once, when the server starts: options (cb_server --mod-option MODNAME.charges=5).
	void Start( Context& ctx ) override
	{
		m_capacity = std::clamp( int( ctx.Option( "MODNAME.charges", 3.0 ) ), 0, 99 );
		m_rechargeTicks = std::max<uint32_t>( 1, uint32_t( 2.0 * double( ctx.Config().tickRate ) + 0.5 ) );
	}

	// Every tick, before the simulation steps: read the world as it is, and say what should change.
	void Tick( Context& ctx ) override
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Joining( slot ) )
			{
				// A new player starts full.
				ctx.Set( SlotTarget( slot ), m_charges, m_capacity );
				m_waited[size_t( i )] = 0;
			}
			if ( ctx.InWorld( slot ) == false )
			{
				continue;
			}

			// Using the item (it is in the hand and the use button went down): the rule goes here.
			// For now it only says so, and the look plays its reactions on the event.
			if ( ctx.Used( slot, m_item ) )
			{
				ctx.Emit( m_used, SlotTarget( slot ) );
			}

			// The motion takes a charge when it happens (in every simulation, on the tick of the
			// key: this mod never sees the key). The server gives them back, one every two seconds.
			int charges = ctx.Get( ctx.PlayerNetId( slot ), m_charges );
			if ( charges >= m_capacity )
			{
				m_waited[size_t( i )] = 0;
			}
			else if ( ++m_waited[size_t( i )] >= m_rechargeTicks )
			{
				m_waited[size_t( i )] = 0;
				ctx.Set( SlotTarget( slot ), m_charges, charges + 1 );
			}
		}
	}

private:
	ItemKindHandle m_item;
	EventHandle m_used;
	ActionHandle m_use;
	FieldHandle m_charges;
	EventHandle m_moved;
	MotionsHandle m_moves;
	int m_capacity = 3;
	uint32_t m_rechargeTicks = 120;
	std::array<uint32_t, kMaxPlayers> m_waited{};
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_MODNAME()
{
	return std::make_unique<MODCLASSMod>();
}
