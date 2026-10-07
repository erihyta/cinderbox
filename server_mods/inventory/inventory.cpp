// Inventory: the rules of what a player carries. The slots themselves are the engine's.
//
// The engine has the mechanism (sim/types.h, Slots): a player has numbered slots, the selected one
// has its item in the hand and the others' items are stowed, and selecting, moving and dropping are
// in the player's own input, so every simulation runs them and the player's own screen does not
// wait. Nothing is created or destroyed by switching: an item keeps its own state (a hot bat stays
// hot on the back).
//
// This mod says how many slots there are and what a life is given and loses:
//   declare.Slots( 3 )   three slots; keys 1 to 3 select them, and a second press empties the hands
//   "inventory.start"    an item property: 1 gives every player one when a life starts
//   dying                takes back what the life started with and drops the rest
//
// What an item is like in a slot is said by its own mod, as item properties the engine reads:
//   "slot"      the slot the kind goes to (1 is the first), pushing out what is there. Without it:
//               the first free slot
//   "holster"   the socket it hangs in while another slot is selected ("Back", "Hip"). Without it,
//               or on a character without that socket, it is out of sight
//
// It publishes "inventory.slot" (the selected slot, from 1; 0: empty hands) and "inventory.item_1"
// .. "inventory.item_3" (the NetId of each slot's item, 0: empty) on the player's board, for its
// look (client/ui): a row of slots at the bottom of the screen. Those follow the simulation a tick
// behind; the hands do not.

#include "mod_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr int kSlots = 3;

struct Bag
{
	std::array<int32_t, kSlots> published{}; // what the board says is in each slot
	int32_t selected = -1;					 // what the board says is selected (+1), -1: nothing said yet
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
		declare.Slots( kSlots );
		m_slot = declare.Field( "inventory.slot", BoardType::Int );
		for ( int s = 0; s < kSlots; ++s )
		{
			m_items[size_t( s )] = declare.Field( "inventory.item_" + std::to_string( s + 1 ), BoardType::Int );
		}
	}

	void Tick( Context& ctx ) override
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			Bag& bag = m_bags[size_t( i )];
			if ( ctx.Joining( slot ) || ctx.Leaving( slot ) )
			{
				bag = Bag{};
			}
			const Character* c = ctx.PlayerCharacter( slot );
			if ( ctx.InWorld( slot ) == false || c == nullptr )
			{
				continue;
			}
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
			Publish( ctx, slot, bag );
		}
	}

private:
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
		next.published = bag.published;
		next.selected = bag.selected;
		next.dead = true;
		bag = next;
		ctx.FaceCamera( SlotTarget( slot ), false );
	}

	void Publish( Context& ctx, PlayerSlot slot, Bag& bag )
	{
		uint32_t target = SlotTarget( slot );
		int32_t selected = ctx.SelectedSlot( slot ) + 1;
		if ( selected != bag.selected )
		{
			bag.selected = selected;
			ctx.Set( target, m_slot, selected );
		}
		for ( int s = 0; s < kSlots; ++s )
		{
			int32_t value = int32_t( ctx.SlotItem( slot, s ) );
			if ( value != bag.published[size_t( s )] )
			{
				bag.published[size_t( s )] = value;
				ctx.Set( target, m_items[size_t( s )], value );
			}
		}
	}

	FieldHandle m_slot;
	std::array<FieldHandle, kSlots> m_items;
	std::array<Bag, kMaxPlayers> m_bags;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_inventory()
{
	return std::make_unique<InventoryMod>();
}
