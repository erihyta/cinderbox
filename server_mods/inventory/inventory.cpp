// Inventory: the rules of what a player carries. The slots themselves are the engine's.
//
// The engine has the mechanism (sim/types.h, Slots): a player has numbered slots, the selected one
// has its item in the hand and the others' items are stowed, and selecting, moving and dropping are
// in the player's own input, so every simulation runs them and the player's own screen does not
// wait. Nothing is created or destroyed by switching: an item keeps its own state (a hot bat stays
// hot on the back).
//
// This mod says how many slots there are and what a life is given and loses:
//   declare.Slots( columns x rows )   a grid, 8 x 4 = 32 by default (inventory.columns,
//                        inventory.rows); the first row is the hotbar, whose keys (1 to 8) select
//                        them, a second press empties the hands; the rest is the bag (I)
//   "inventory.start"    an item property: 1 gives every player one when a life starts
//   dying                takes back what the life started with and drops the rest
//
// A life is a body's: what it started with stays the body's. A player who takes over another body
// (a possession) finds it with what it carries and is given nothing; back in its own body, that
// body's starting items are still known as such, and dying takes them back, as before.
//
// What an item is like in a slot is said by its own mod, as item properties the engine reads:
//   "slot"      the slot the kind goes to (1 is the first) if it is free; else, or without it, the
//               first free slot. A slot is a place, not a kind's: two of a kind are two items, each
//               in a slot of its own, each taken out by that slot's key and used from the hand
//   "holster"   the socket it hangs in while another slot is selected ("Back", "Hip"). Without it,
//               or on a character without that socket, it is out of sight
//
// It publishes how many columns the grid has (inventory.columns, a field of the game's); its look
// (client/ui: the hotbar, the first row, at the bottom of the screen, and the inventory screen on I,
// all of it in rows of that many) reads the engine's slots themselves, so it follows the hands at
// once. Server options: inventory.columns (default 8), inventory.rows (default 4); at most 36.

#include "mod_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace
{

using namespace cb;
using namespace cb::mods;


struct Bag
{
	bool dead = false;
	bool gave = false;				// this life's starting items were given
	bool full = false;				// the hand has an item
	std::vector<int> expected;		// kinds given and not seen yet
	std::vector<uint32_t> starting; // the ones still carried
};

class InventoryMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "inventory";
	}

	void Declare( Declarations& declare ) override
	{
		// A grid: the first row is the hotbar.
		m_columns = int( std::clamp( declare.Option( "inventory.columns", 8.0 ), 1.0, double( kMaxSlots ) ) );
		int rows = int( std::clamp( declare.Option( "inventory.rows", 4.0 ), 1.0, double( kMaxSlots ) ) );
		declare.Slots( std::min( m_columns * rows, kMaxSlots ) );
		m_columnsField = declare.Field( "inventory.columns", BoardType::Int, BoardScope::Global );
	}

	void Tick( Context& ctx ) override
	{
		if ( m_said == false )
		{
			ctx.Set( 0, m_columnsField, m_columns );
			m_said = true;
		}
		// Bags of bodies that are gone.
		for ( auto it = m_bags.begin(); it != m_bags.end(); )
		{
			it = ctx.EntityTransform( it->first ) == nullptr ? m_bags.erase( it ) : std::next( it );
		}
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			uint32_t& last = m_bodies[size_t( i )];
			if ( ctx.Joining( slot ) || ctx.Leaving( slot ) )
			{
				// A player new to the slot, or gone from it: its body's life starts again.
				m_bags.erase( last );
				m_bags.erase( ctx.PlayerNetId( slot ) );
				last = 0;
			}
			const Character* c = ctx.PlayerCharacter( slot );
			if ( ctx.InWorld( slot ) == false || c == nullptr )
			{
				continue;
			}
			// The body's bag. A body the slot took over (it drove another before) starts as it is:
			// nothing is given to it.
			const uint32_t body = ctx.PlayerNetId( slot );
			const bool tookOver = last != 0 && last != body;
			last = body;
			auto found = m_bags.find( body );
			if ( found == m_bags.end() )
			{
				Bag fresh;
				fresh.gave = tookOver;
				found = m_bags.emplace( body, fresh ).first;
			}
			Bag& bag = found->second;
			uint32_t target = SlotTarget( slot );
			std::vector<CarriedItem> carried = ctx.CarriedItems( slot );

			if ( c->dead != 0 )
			{
				if ( bag.dead == false )
				{
					Die( ctx, slot, bag, carried );
				}
				continue;
			}
			bag.dead = false;

			// A life starts with what the mods' items say.
			if ( bag.gave == false )
			{
				bag.gave = true;
				for ( int kind = 0; kind < int( ctx.Schema().itemKinds.size() ); ++kind )
				{
					ItemKindHandle handle{ kind };
					if ( ctx.ItemProperty( handle, "inventory.start", 0.0f ) != 0.0f )
					{
						ctx.GiveItem( target, handle );
						bag.expected.push_back( kind );
					}
				}
			}

			// Which of what is carried this life started with: taken back when it ends.
			auto carries = [&carried]( uint32_t netId ) {
				return std::any_of( carried.begin(), carried.end(), [netId]( const CarriedItem& it ) { return it.netId == netId; } );
			};
			bag.starting.erase( std::remove_if( bag.starting.begin(), bag.starting.end(), [&]( uint32_t id ) { return carries( id ) == false; } ),
								bag.starting.end() );
			for ( const CarriedItem& it : carried )
			{
				auto expected = std::find( bag.expected.begin(), bag.expected.end(), it.kind.index );
				if ( expected != bag.expected.end() && std::find( bag.starting.begin(), bag.starting.end(), it.netId ) == bag.starting.end() )
				{
					bag.expected.erase( expected );
					bag.starting.push_back( it.netId );
				}
			}

			// Empty hands mean freelook: a weapon turns camera-facing on when it comes out, and only
			// this turns it off, so going from one weapon to another never races two mods.
			int selected = ctx.SelectedSlot( slot );
			bool full = selected >= 0 && ctx.SlotItem( slot, selected ) != 0;
			if ( full != bag.full )
			{
				bag.full = full;
				if ( full == false )
				{
					ctx.FaceCamera( target, false );
				}
			}
		}
	}

private:
	int m_columns = 8;
	FieldHandle m_columnsField;
	bool m_said = false;
	void Die( Context& ctx, PlayerSlot slot, Bag& bag, const std::vector<CarriedItem>& carried )
	{
		b3Vec3 aim = ctx.AimDirection( slot );
		b3Vec3 flat = b3Normalize( b3Vec3{ aim.x, 0.0f, aim.z } );
		b3Vec3 chest = b3Sub( ctx.EyePosition( slot ), b3Vec3{ 0.0f, 0.35f, 0.0f } );
		b3Quat facing = b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, std::atan2( -aim.x, -aim.z ) );
		for ( const CarriedItem& it : carried )
		{
			if ( std::find( bag.starting.begin(), bag.starting.end(), it.netId ) != bag.starting.end() )
			{
				ctx.Destroy( it.netId ); // the next life gets its own
			}
			else
			{
				ctx.DropItem( it.netId, b3Add( chest, b3MulSV( 0.45f, flat ) ), facing, b3Add( flat, b3Vec3{ 0.0f, 1.5f, 0.0f } ) );
			}
		}
		// The slot that was selected stays selected: the next life's item for it comes out.
		Bag next;
		next.dead = true;
		bag = next;
		ctx.FaceCamera( SlotTarget( slot ), false );
	}

	std::map<uint32_t, Bag> m_bags; // by body
	std::array<uint32_t, kMaxPlayers> m_bodies{}; // the body each slot drove last tick
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_inventory()
{
	return std::make_unique<InventoryMod>();
}
