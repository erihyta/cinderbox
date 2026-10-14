// A grenade: the example of an item thrown from the hand (a CbLaunch of the one carried,
// sim/motions.h).
//
// What throwing is (a key, how hard, what leaves the hand, when) is not in this file: the grenade
// is an item (prefabs/shell.tscn, a CbItem) carried in slot 6, whose slot's key (H) plays its
// throw on the left arm (its animation pack); the throw clip's marker, grenade.release, lets go of
// the grenade the player carries (a CbLaunch under its Release motion). Every simulation runs that
// from the player's own input, so the grenade leaves on the same tick on the thrower's own screen,
// flies and bounces as its body says, and this mod never sees the key.
//
// This file is the rules: a new grenade a little after a player has none (grenade.refill_seconds),
// and what a grenade does when it hits something.
// When it hits something it goes off: everyone near
// is hurt (the combat mod's "combat.damage", like any other weapon's: less with distance),
// everything loose near it is thrown and sent tumbling (props, items, ragdolls: so those it kills
// fly, and those it only hurts keep their feet), and the grenade is gone. One that hits nothing hard enough is removed when its time
// is up (the launch's seconds).
//
// Server options: grenade.damage (at the centre, default 80), grenade.radius (metres, default 4),
// grenade.arm_speed (how hard it must hit to go off, m/s, default 3), grenade.push (how fast what is
// loose is thrown at the centre, m/s, default 12), grenade.refill_seconds (how long a player is
// without one before the next, default 1.2; 0: never: one a life).

#include "mod_api.h"

#include <algorithm>
#include <vector>

using namespace cb;
using namespace cb::mods;

namespace
{

class GrenadeMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "grenade";
	}

	void Declare( Declarations& declare ) override
	{
		// The grenade, its throw and its pose: its scene's. Its slot is the sixth.
		declare.Slots( 6 );
		declare.ItemsOfLook();
		m_shell = declare.ItemKind( "grenade.shell" );
		// Said by the grenade's throw: its clip's marker (the hand lets go), and the throw.
		m_release = declare.Event( "grenade.release" );
		m_thrown = declare.Event( "grenade.thrown" );
		m_blast = declare.Event( "grenade.blast" );
		// The combat mod's (see combat.cpp): what a hit on a player is said with.
		m_damage = declare.Event( "combat.damage" );
		// Not for picking up, and not for a slot: it is on its way.
		declare.ItemProperty( m_shell, "pickup.never", 1.0f );
	}

	void Start( Context& ctx ) override
	{
		m_centreDamage = float( std::clamp( ctx.Option( "grenade.damage", 80.0 ), 0.0, 10000.0 ) );
		m_radius = float( std::clamp( ctx.Option( "grenade.radius", 4.0 ), 0.5, 50.0 ) );
		m_armSpeed = float( std::clamp( ctx.Option( "grenade.arm_speed", 3.0 ), 0.0, 100.0 ) );
		m_push = float( std::clamp( ctx.Option( "grenade.push", 12.0 ), 0.0, 100.0 ) );
		m_refillTicks = uint32_t( std::clamp( ctx.Option( "grenade.refill_seconds", 1.2 ), 0.0, 600.0 ) * double( ctx.Config().tickRate ) + 0.5 );
	}

	void Tick( Context& ctx ) override
	{
		Refill( ctx );
		// The blasts of the last few ticks throw those they killed, now that they have fallen.
		for ( size_t i = 0; i < m_recent.size(); )
		{
			if ( ctx.Tick() > m_recent[i].tick + kFallTicks )
			{
				m_recent.erase( m_recent.begin() + std::ptrdiff_t( i ) );
				continue;
			}
			Throw( ctx, m_recent[i].point, 0, m_recent[i].tick );
			++i;
		}
		if ( m_recent.empty() )
		{
			m_flung.clear();
		}
		// What the grenades in the world ran into on the tick before this one.
		m_gone.clear();
		for ( const Context::ItemHit& hit : ctx.Hits( m_shell ) )
		{
			bool hitPlayer = ctx.IsPlayer( hit.other );
			if ( ( hit.speed < m_armSpeed && hitPlayer == false ) || std::find( m_gone.begin(), m_gone.end(), hit.item ) != m_gone.end() )
			{
				continue; // a roll, a nudge: not yet. Or it went off already this tick.
			}
			m_gone.push_back( hit.item );
			GoOff( ctx, hit.item, hit.point );
		}
	}

private:
	// A player in the world without a grenade gets one, a while after it had none (it threw it).
	void Refill( Context& ctx )
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			const Character* c = ctx.InWorld( slot ) ? ctx.PlayerCharacter( slot ) : nullptr;
			uint32_t& since = m_without[i];
			if ( m_refillTicks == 0 || c == nullptr || c->dead != 0 )
			{
				since = 0;
				continue;
			}
			std::vector<CarriedItem> carried = ctx.CarriedItems( slot );
			bool has = std::any_of( carried.begin(), carried.end(), [&]( const CarriedItem& it ) { return it.kind.index == m_shell.index; } );
			if ( has )
			{
				since = 0;
				continue;
			}
			since = since == 0 ? ctx.Tick() : since;
			if ( ctx.Tick() >= since + m_refillTicks )
			{
				ctx.GiveItem( SlotTarget( slot ), m_shell );
				since = ctx.Tick() + 2; // (not again before it has it)
			}
		}
	}

	void GoOff( Context& ctx, uint32_t item, b3Vec3 point )
	{
		// Whose it is (for the kill): the thrower, as long as it is still here.
		uint32_t thrower = ctx.ThrownBy( item );
		int throwerSlot = thrower != 0 ? ctx.SlotOf( thrower ) : -1;
		uint32_t throwerTarget = throwerSlot >= 0 ? SlotTarget( PlayerSlot( throwerSlot ) ) : 0;

		ctx.Emit( m_blast, throwerTarget, 0, int32_t( m_radius * 100.0f ), point );
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			const Character* c = ctx.InWorld( slot ) ? ctx.PlayerCharacter( slot ) : nullptr;
			const Transform* at = c != nullptr && c->dead == 0 ? ctx.EntityTransform( ctx.PlayerNetId( slot ) ) : nullptr;
			if ( at == nullptr )
			{
				continue;
			}
			// (From the blast to the middle of the body.)
			b3Vec3 to = b3Sub( b3Add( at->position, b3Vec3{ 0.0f, 0.9f, 0.0f } ), point );
			float distance = b3Length( to );
			if ( distance > m_radius )
			{
				continue;
			}
			float share = 1.0f - distance / m_radius;
			int32_t damage = int32_t( m_centreDamage * share + 0.5f );
			b3Vec3 away = distance > 0.01f ? b3MulSV( 1.0f / distance, to ) : b3Vec3{ 0.0f, 1.0f, 0.0f };
			b3Vec3 push = b3Add( b3MulSV( 6.0f * share + 2.0f, away ), b3Vec3{ 0.0f, 2.5f, 0.0f } );
			if ( damage > 0 )
			{
				ctx.Emit( m_damage, throwerTarget, ctx.PlayerNetId( slot ), damage, point, push );
			}
			// (The living keep their feet: `push` is what the combat mod throws the body with if this
			// kills. The fallen are loose, and are thrown below with everything else.)
		}
		// Everything loose is thrown too: crates, balls, what lies on the floor, the fallen. The
		// same change of speed whatever it weighs (a blast is not a shove), more the nearer it is.
		Throw( ctx, point, item, 0 );
		// Those it kills fall a tick or two from now (the combat mod hears of the damage, then the
		// body is made): the blast is remembered that long, and throws them as they fall.
		m_recent.push_back( { point, ctx.Tick() } );
		ctx.Destroy( item );
	}

	// Everything loose within the blast is thrown; with `fallenSince`, only the bodies that fell
	// at or after that tick (the ones this blast killed).
	void Throw( Context& ctx, b3Vec3 point, uint32_t item, uint32_t fallenSince )
	{
		for ( const NearBody& body : ctx.BodiesNear( point, m_radius ) )
		{
			if ( body.netId == item || ( fallenSince != 0 && body.ragdollSince < fallenSince ) ||
				 std::find( m_flung.begin(), m_flung.end(), body.netId ) != m_flung.end() )
			{
				continue;
			}
			b3Vec3 to = b3Sub( body.position, point );
			float share = 1.0f - body.distance / m_radius;
			b3Vec3 away = body.distance > 0.01f ? b3MulSV( 1.0f / body.distance, to ) : b3Vec3{ 0.0f, 1.0f, 0.0f };
			if ( body.ragdollSince != 0 )
			{
				// A body: the whole of it, up and away. Once per blast.
				m_flung.push_back( body.netId );
				ctx.Push( body.netId, body.position, b3Add( b3MulSV( m_push * ( 0.3f + 0.5f * share ), away ), b3Vec3{ 0.0f, 0.45f * m_push * ( 0.3f + 0.5f * share ), 0.0f } ),
						  ImpulseThrow );
				continue;
			}
			// Hit on the side that faces the blast, low: off its centre, so it tumbles as it goes. How
			// far off is a share of its size, less for small things (which would spin like tops).
			float off = 0.3f * body.radius * std::clamp( body.radius / 0.5f, 0.15f, 1.0f );
			b3Vec3 where = b3Sub( body.position, b3Add( b3MulSV( off, away ), b3Vec3{ 0.0f, 0.6f * off, 0.0f } ) );
			ctx.Push( body.netId, where, b3Add( b3MulSV( m_push * share, away ), b3Vec3{ 0.0f, 0.35f * m_push * share, 0.0f } ), ImpulseVelocity );
		}
	}

	ItemKindHandle m_shell;
	EventHandle m_release;
	EventHandle m_thrown;
	EventHandle m_blast;
	EventHandle m_damage;
	uint32_t m_refillTicks = 72;
	uint32_t m_without[kMaxPlayers] = {}; // the tick from which a player has had none (0: it has one)
	float m_centreDamage = 80.0f;
	float m_radius = 4.0f;
	float m_armSpeed = 3.0f;
	float m_push = 12.0f;
	std::vector<uint32_t> m_gone;
	// How long after a blast a body it killed may still fall, in ticks.
	static constexpr uint32_t kFallTicks = 6;
	struct Blast
	{
		b3Vec3 point;
		uint32_t tick;
	};
	std::vector<Blast> m_recent;
	std::vector<uint32_t> m_flung; // the bodies the recent blasts threw already
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_grenade()
{
	return std::make_unique<GrenadeMod>();
}
