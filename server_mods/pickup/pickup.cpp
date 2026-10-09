// Pickup: items lying in the world can be picked up, dropped and thrown.
//
// Near an item (within reach of the chest, and not behind the player), its NetId is on the
// player's board as "pickup.target"; the mod's look turns that into a prompt above the item
// ("[E] Pick up Bat"). E takes it; G throws what the right hand holds.
//
// Where a taken item goes is the inventory mod's when it runs: this mod only makes the player
// carry it (stowed), and the inventory puts it in its slot and into the hand. Without an inventory
// there is only the right hand: E takes the item into it (whatever was there drops in its place),
// and dying drops it.
//
// Some items take a moment: E has to be held for the item's "pickup.hold_seconds" (an item property,
// authored on the item's CbItem or declared by its mod; --mod-option pickup.hold_seconds=N is
// the default for the rest, 0: a tap). "pickup.hold" says how long the item in reach needs, and
// while E is held on it "pickup.since" is the tick that began (0: not holding): a look draws the
// progress from those two, so the board changes when a hold starts and ends, not every tick.
// Letting go, or losing the item, starts over.
//
// --mod-option pickup.spawn_each=N drops N of every item kind the server's mods declared around
// the map's spawn point when the server starts, so there is something to pick up.
//
// The engine only offers the verbs (ItemsNear, PickUpItem, DropItem, SpawnWorldItem); who may pick
// up what, and when, is this mod's.

#include "mod_api.h"

#include <array>
#include <algorithm>
#include <cmath>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr float kReach = 1.5f;		// metres, along the ground, from the player to the item
constexpr float kBelow = 1.7f;		// how far below the body's centre it may lie (the floor at its feet)
constexpr float kAbove = 0.8f;		// and above it
constexpr float kBehind = -0.3f;	// how far behind (cosine of the angle to where it looks) still counts
constexpr float kThrowSpeed = 5.0f; // m/s along the aim, when thrown
constexpr float kDropSpeed = 1.0f;	// when it slips from a hand (picked up something else, died)

struct Picker
{
	uint32_t target = 0; // what "pickup.target" says
	bool dead = false;
	uint32_t holding = 0;	// the item E is being held on (0: none)
	uint32_t holdStart = 0; // the tick that began
	int32_t since = 0;		// what "pickup.since" says
};

// Turned so the item points (its -Z) along `aim`, flattened.
b3Quat Facing( b3Vec3 aim )
{
	float yaw = std::atan2( -aim.x, -aim.z );
	return b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, yaw );
}

class PickupMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "pickup";
	}

	void Declare( Declarations& declare ) override
	{
		m_pickup = declare.Action( "pickup", "E" );
		m_drop = declare.Action( "drop", "G" );
		// The NetId of the item the player can pick up now (0: none): what the prompt shows.
		m_target = declare.Field( "pickup.target", BoardType::Int );
		// How long E must be held for it, in seconds (0: a tap), and the tick a hold on it began (0: none).
		m_hold = declare.Field( "pickup.hold", BoardType::Float );
		m_since = declare.Field( "pickup.since", BoardType::Int );
		m_hand = declare.Socket( "RightHand" );
	}

	void Start( Context& ctx ) override
	{
		m_spawnEach = int( ctx.Option( "pickup.spawn_each", 0.0 ) );
		m_holdSeconds = float( ctx.Option( "pickup.hold_seconds", 0.0 ) );
	}

	void Tick( Context& ctx ) override
	{
		if ( m_spawnEach > 0 )
		{
			SpawnAround( ctx );
			m_spawnEach = 0;
		}
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			Picker& p = m_pickers[size_t( i )];
			if ( ctx.Joining( slot ) || ctx.Leaving( slot ) )
			{
				p = Picker{};
			}
			uint32_t netId = ctx.PlayerNetId( slot );
			const Character* c = ctx.PlayerCharacter( slot );
			const Transform* body = netId != 0 ? ctx.EntityTransform( netId ) : nullptr;
			if ( c == nullptr || body == nullptr )
			{
				continue;
			}
			uint32_t target = SlotTarget( slot );
			uint32_t inHand = ctx.HeldItem( slot, m_hand );
			// With slots (an inventory), what is picked up goes into one, and that decides what a death drops.
			bool inventory = ctx.SlotCount() > 0;

			// Dying lets go of what the hand holds (with an inventory, that decides what a death drops).
			if ( c->dead != 0 )
			{
				if ( p.dead == false && inHand != 0 && inventory == false )
				{
					Drop( ctx, slot, inHand, kDropSpeed );
				}
				p.dead = true;
				SetTarget( ctx, target, p, 0 );
				SetSince( ctx, target, p, 0 );
				p.holding = 0;
				continue;
			}
			p.dead = false;

			// What is in reach: the nearest item, unless it is behind the player.
			b3Vec3 aim = ctx.AimDirection( slot );
			b3Vec3 flatAim = b3Normalize( b3Vec3{ aim.x, 0.0f, aim.z } );
			uint32_t near = 0;
			float nearest = kReach;
			for ( const WorldItem& item : ctx.ItemsNear( body->position, kReach + kBelow ) )
			{
				b3Vec3 to = b3Sub( item.position, body->position );
				float rise = to.y;
				to.y = 0.0f;
				float length = b3Length( to );
				bool facing = length < 0.5f || b3Dot( b3MulSV( 1.0f / length, to ), flatAim ) >= kBehind;
				// (A kind may say it is not for picking up: a thrown grenade, "pickup.never".)
				if ( rise >= -kBelow && rise <= kAbove && length <= nearest && facing &&
					 ctx.ItemProperty( ctx.ItemKindOf( item.netId ), "pickup.never", 0.0f ) == 0.0f )
				{
					nearest = length;
					near = item.netId;
				}
			}
			SetTarget( ctx, target, p, near );

			// E: a press starts on the item in reach; it counts while E stays down on that same item.
			float seconds = p.target != 0 ? HoldSeconds( ctx, p.target ) : 0.0f;
			if ( c->frozen == 0 && ctx.Pressed( slot, m_pickup ) && p.target != 0 )
			{
				p.holding = p.target;
				p.holdStart = ctx.Tick();
			}
			if ( p.holding != 0 && ( p.holding != p.target || ctx.Held( slot, m_pickup ) == false || c->frozen != 0 ) )
			{
				p.holding = 0; // let go, or it is no longer the one in reach
			}
			float elapsed = p.holding != 0 ? float( ctx.Tick() - p.holdStart ) / float( ctx.Config().tickRate ) : 0.0f;
			bool take = p.holding != 0 && elapsed >= seconds;
			SetSince( ctx, target, p, p.holding != 0 && seconds > 0.0f && take == false ? int32_t( std::max( p.holdStart, 1u ) ) : 0 );
			if ( c->frozen != 0 )
			{
				continue;
			}
			if ( take && inventory )
			{
				// Carried from now on: the engine finds its slot and takes it out.
				ctx.PickUpStowed( target, p.holding );
				p.holding = 0;
			}
			else if ( take )
			{
				// Swap: what the hand held drops, then the hand takes the new one (in this order, in
				// this tick's frame).
				if ( inHand != 0 )
				{
					Drop( ctx, slot, inHand, kDropSpeed );
				}
				ctx.PickUpItem( target, p.holding, m_hand );
				p.holding = 0;
			}
			else if ( ctx.Pressed( slot, m_drop ) && inHand != 0 )
			{
				Drop( ctx, slot, inHand, kThrowSpeed );
			}
		}
	}

private:
	void SetTarget( Context& ctx, uint32_t target, Picker& p, uint32_t item )
	{
		if ( item != p.target )
		{
			p.target = item;
			ctx.Set( target, m_target, int32_t( item ) );
			ctx.SetFloat( target, m_hold, item != 0 ? HoldSeconds( ctx, item ) : 0.0f );
		}
	}

	void SetSince( Context& ctx, uint32_t target, Picker& p, int32_t since )
	{
		if ( since != p.since )
		{
			p.since = since;
			ctx.Set( target, m_since, since );
		}
	}

	// What its mod says about it, or the server's default.
	float HoldSeconds( const Context& ctx, uint32_t item ) const
	{
		return std::max( 0.0f, ctx.ItemProperty( ctx.ItemKindOf( item ), "pickup.hold_seconds", m_holdSeconds ) );
	}

	// Out of the hand, a little in front of the chest, pointing where the player looks.
	void Drop( Context& ctx, PlayerSlot slot, uint32_t item, float speed )
	{
		b3Vec3 aim = ctx.AimDirection( slot );
		b3Vec3 chest = b3Sub( ctx.EyePosition( slot ), b3Vec3{ 0.0f, 0.35f, 0.0f } );
		b3Vec3 flatAim = b3Normalize( b3Vec3{ aim.x, 0.0f, aim.z } );
		b3Vec3 grip = b3Add( chest, b3MulSV( 0.45f, flatAim ) );
		b3Vec3 velocity = b3Add( b3MulSV( speed, aim ), b3Vec3{ 0.0f, 1.5f, 0.0f } );
		ctx.DropItem( item, grip, Facing( aim ), velocity );
	}

	// N of every declared item kind in a ring around the map's spawn point.
	void SpawnAround( Context& ctx )
	{
		const auto& kinds = ctx.Schema().itemKinds;
		int total = int( kinds.size() ) * m_spawnEach;
		b3Vec3 center = ctx.Map().spawnCenter;
		for ( int i = 0; i < total; ++i )
		{
			float angle = 6.2831853f * float( i ) / float( std::max( total, 1 ) );
			b3Vec3 at = { center.x + 3.0f * std::cos( angle ), center.y + 0.5f, center.z + 3.0f * std::sin( angle ) };
			ItemKindHandle kind{ i % int( kinds.size() ) };
			ctx.SpawnWorldItem( kind, at, Facing( b3Vec3{ std::cos( angle ), 0.0f, std::sin( angle ) } ) );
		}
	}

	ActionHandle m_pickup;
	ActionHandle m_drop;
	FieldHandle m_target;
	FieldHandle m_hold;
	FieldHandle m_since;
	float m_holdSeconds = 0.0f;
	SocketHandle m_hand;
	int m_spawnEach = 0;
	std::array<Picker, kMaxPlayers> m_pickers;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_pickup()
{
	return std::make_unique<PickupMod>();
}
