// Inventory: what a player carries, and which of it is in the hand.
//
// A player has slots: 1 is empty hands, 2 to 4 hold one item each. The slot that is out has its
// item in the right hand; the others' items are stowed: still carried, in no hand (the engine's
// StowItem / HoldItem). Keys 1 to 4 switch. Nothing is created or destroyed by switching, so an
// item keeps its own state (a hot bat stays hot on the back) and nothing falls out of a hand
// because another slot came out.
//
// Other mods say what their items are like with item properties, and never touch the slots:
//   "inventory.slot"    which slot the kind lives in (2..4). Without it: the first free slot.
//   "inventory.start"   1: every player gets one when a life starts.
//   "inventory.holster" a socket the item hangs in while it is stowed (ItemProperty with a socket:
//                       "Back", "Hip"). Without it, or on a character without that socket, a stowed
//                       item is out of sight.
//
// Anything the player comes to carry is put into its slot (this is how the pickup mod's items
// arrive: it only makes the player carry them). If that slot already has an item, the old one
// drops: one item per slot. A new arrival comes out into the hand, except the ones a life starts
// with. An item that leaves (thrown, taken away, expired) simply empties its slot.
//
// Dying takes back what the life started with and drops the rest where the player stood.
//
// It publishes "inventory.slot" (which slot is out) and "inventory.item_2" .. "inventory.item_4"
// (the NetId of each slot's item, 0: empty) on the player's board. Its look (client/ui) is a row of
// slots at the bottom of the screen, built from those.

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

constexpr int kSlots = 4;			// slot 1 is empty hands
constexpr float kDropSpeed = 1.0f;	// m/s, when an item is pushed out of its slot

struct Bag
{
	std::array<uint32_t, kSlots + 1> item{};	  // slot -> NetId (0: empty); [0] and [1] stay 0
	std::array<int32_t, kSlots + 1> published{}; // what the board says about them
	int current = 1;
	bool full = false;				// the hand has (or is getting) an item
	bool dead = false;
	bool gave = false;				// this life's starting items were given
	std::vector<uint32_t> starting; // the ones still carried
	std::vector<int> expected;		// kinds given and not seen yet
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
		m_slot = declare.Field( "inventory.slot", BoardType::Int );
		for ( int s = 1; s <= kSlots; ++s )
		{
			m_keys[size_t( s )] = declare.Action( "slot_" + std::to_string( s ), std::to_string( s ) );
			if ( s >= 2 )
			{
				m_items[size_t( s )] = declare.Field( "inventory.item_" + std::to_string( s ), BoardType::Int );
			}
		}
		m_hand = declare.Socket( "RightHand" );
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
			if ( ctx.Joining( slot ) )
			{
				ctx.Set( SlotTarget( slot ), m_slot, 1 );
				continue;
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
						ctx.GiveItem( target, handle, ctx.ItemSocket( handle, "inventory.holster" ) );
						bag.expected.push_back( kind );
					}
				}
			}

			// What left: thrown, taken away, expired.
			auto carries = [&carried]( uint32_t netId ) {
				return std::any_of( carried.begin(), carried.end(), [netId]( const CarriedItem& it ) { return it.netId == netId; } );
			};
			for ( int s = 2; s <= kSlots; ++s )
			{
				if ( bag.item[size_t( s )] != 0 && carries( bag.item[size_t( s )] ) == false )
				{
					bag.item[size_t( s )] = 0;
				}
			}
			bag.starting.erase( std::remove_if( bag.starting.begin(), bag.starting.end(), [&]( uint32_t id ) { return carries( id ) == false; } ),
								bag.starting.end() );

			// What arrived: into its slot, pushing out what was there.
			int wanted = bag.current;
			std::vector<CarriedItem> arrivals;
			for ( const CarriedItem& it : carried )
			{
				if ( std::find( bag.item.begin(), bag.item.end(), it.netId ) == bag.item.end() )
				{
					arrivals.push_back( it );
				}
			}
			std::vector<uint32_t> leaving;
			for ( const CarriedItem& it : arrivals )
			{
				auto expected = std::find( bag.expected.begin(), bag.expected.end(), it.kind.index );
				bool start = expected != bag.expected.end();
				if ( start )
				{
					bag.expected.erase( expected );
					bag.starting.push_back( it.netId );
				}
				int s = SlotFor( ctx, bag, it.kind );
				if ( uint32_t old = bag.item[size_t( s )] )
				{
					Drop( ctx, slot, old );
					leaving.push_back( old );
					bag.starting.erase( std::remove( bag.starting.begin(), bag.starting.end(), old ), bag.starting.end() );
				}
				bag.item[size_t( s )] = it.netId;
				if ( start == false )
				{
					wanted = s; // what was just picked up comes out
				}
			}

			for ( int s = 1; s <= kSlots; ++s )
			{
				if ( ctx.Pressed( slot, m_keys[size_t( s )] ) )
				{
					wanted = s;
				}
			}

			// The slot that is out has its item in the hand; everything else in use is put away.
			uint32_t desired = bag.item[size_t( wanted )];
			bool inHand = false;
			for ( const CarriedItem& it : carried )
			{
				if ( it.stowed || std::find( leaving.begin(), leaving.end(), it.netId ) != leaving.end() )
				{
					continue;
				}
				if ( it.netId == desired )
				{
					inHand = true;
				}
				else
				{
					ctx.StowItem( it.netId, ctx.ItemSocket( it.kind, "inventory.holster" ) );
				}
			}
			if ( desired != 0 && inHand == false )
			{
				ctx.HoldItem( desired, m_hand );
			}
			// Empty hands mean freelook: a weapon turns camera-facing on when it comes out, and only
			// this turns it off, so going from one weapon to another never races two mods.
			bool full = desired != 0;
			if ( full != bag.full )
			{
				bag.full = full;
				if ( full == false )
				{
					ctx.FaceCamera( target, false );
				}
			}
			if ( wanted != bag.current )
			{
				bag.current = wanted;
				ctx.Set( target, m_slot, wanted );
			}
			Publish( ctx, target, bag );
		}
	}

private:
	// The kind's own slot, else the first free one, else the one that is out (or the last).
	int SlotFor( const Context& ctx, const Bag& bag, ItemKindHandle kind ) const
	{
		int own = int( ctx.ItemProperty( kind, "inventory.slot", 0.0f ) );
		if ( own >= 2 && own <= kSlots )
		{
			return own;
		}
		for ( int s = 2; s <= kSlots; ++s )
		{
			if ( bag.item[size_t( s )] == 0 )
			{
				return s;
			}
		}
		return bag.current >= 2 ? bag.current : kSlots;
	}

	// Out of the bag, a little in front of the chest.
	void Drop( Context& ctx, PlayerSlot slot, uint32_t item )
	{
		b3Vec3 aim = ctx.AimDirection( slot );
		b3Vec3 flat = b3Normalize( b3Vec3{ aim.x, 0.0f, aim.z } );
		b3Vec3 chest = b3Sub( ctx.EyePosition( slot ), b3Vec3{ 0.0f, 0.35f, 0.0f } );
		b3Quat facing = b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, std::atan2( -aim.x, -aim.z ) );
		ctx.DropItem( item, b3Add( chest, b3MulSV( 0.45f, flat ) ), facing, b3Add( b3MulSV( kDropSpeed, flat ), b3Vec3{ 0.0f, 1.5f, 0.0f } ) );
	}

	void Die( Context& ctx, PlayerSlot slot, Bag& bag, const std::vector<CarriedItem>& carried )
	{
		for ( const CarriedItem& it : carried )
		{
			if ( std::find( bag.starting.begin(), bag.starting.end(), it.netId ) != bag.starting.end() )
			{
				ctx.Destroy( it.netId ); // the next life gets its own
			}
			else
			{
				Drop( ctx, slot, it.netId );
			}
		}
		// The slot that was out stays the one that is out: the next life's item for it comes out.
		Bag next;
		next.current = bag.current;
		next.published = bag.published;
		next.dead = true;
		bag = next;
		ctx.FaceCamera( SlotTarget( slot ), false );
		Publish( ctx, SlotTarget( slot ), bag );
	}

	void Publish( Context& ctx, uint32_t target, Bag& bag )
	{
		for ( int s = 2; s <= kSlots; ++s )
		{
			int32_t value = int32_t( bag.item[size_t( s )] );
			if ( value != bag.published[size_t( s )] )
			{
				bag.published[size_t( s )] = value;
				ctx.Set( target, m_items[size_t( s )], value );
			}
		}
	}

	FieldHandle m_slot;
	std::array<FieldHandle, kSlots + 1> m_items;
	std::array<ActionHandle, kSlots + 1> m_keys;
	SocketHandle m_hand;
	std::array<Bag, kMaxPlayers> m_bags;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_inventory()
{
	return std::make_unique<InventoryMod>();
}
