// A frag grenade: thrown from the hand, it goes off when its fuse is out. The example of an item
// whose time is up (ItemShape::expiredEvent, sim/mod_schema.h).
//
// What throwing is (a key, how hard, what leaves the hand, when, how long the fuse) is not in this
// file: the grenade is an item (client/prefabs/frag.tscn, a CbItem) that goes to slot 6, whose own
// key (H), or the use button with it selected, plays its throw on the left arm (its animation pack); the throw clip's marker,
// frag.release, lets go of the grenade the player carries (a CbLaunch of the one carried), with
// 2.5 s to live. It bounces and rolls as its body says. When its time is up the simulation sets it
// off where it is, as its sheet says (client/prefabs/frag.rules: a motion the grenade owns): it says
// frag.blast, blows a hole in a wall near it and throws what is loose; and it says
// "frag.grenade.expired" there, with whoever threw it, and removes it: on every machine, on the same
// tick, so the blast is never a lead late on anyone's screen.
//
// This file is the rules: grenades come one at a time, a little apart (frag.refill_seconds), until a
// player carries frag.carry of them (a stack: the scene's stack), how big a blast is (its options,
// published as fields the sheet reads: frag.radius, frag.push, frag.break_radius,
// frag.rubble_seconds), and who it hurts: everyone near (the combat mod's "combat.damage", like any
// other weapon's: less with distance); the bodies it kills, and those lying near, are flung. It does
// not go off when it hits something: that was the impact grenade (the grenade mod, switched off).
//
// Server options: frag.damage (at the centre, default 90), frag.radius (metres, default 5),
// frag.push (how fast what is loose is thrown at the centre, m/s, default 12), frag.refill_seconds
// (how long a player waits for the next while it has fewer than it carries, default 1.2; 0: never:
// one a life), frag.carry (how many, default 3: the scene's stack holds that many),
// frag.break_radius (metres: a breakable's pieces whose centre is this near go, default half of
// frag.radius; 0: nothing breaks), frag.rubble_seconds (how long its pieces stay, default 0: for
// good).

#include "mod_api.h"

#include <algorithm>
#include <vector>

using namespace cb;
using namespace cb::mods;

namespace
{

class FragMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "frag";
	}

	void Declare( Declarations& declare ) override
	{
		// The grenade, its throw and its pose: its scene's. Its slot is the sixth.
		declare.Slots( 6 );
		declare.ItemsOfLook();
		m_grenade = declare.ItemKind( "frag.grenade" );
		// Said by the grenade's throw: its clip's marker (the hand lets go), and the throw.
		m_release = declare.Event( "frag.release" );
		m_thrown = declare.Event( "frag.thrown" );
		// Said by the simulation when a grenade's time is up ("<kind>.expired"): the fuse.
		m_expired = declare.Event( "frag.grenade.expired" );
		m_blast = declare.Event( "frag.blast" );
		// The blast's numbers, for its sheet (frag.rules): the server's options.
		m_radiusField = declare.Field( "frag.radius", BoardType::Float, BoardScope::Global );
		m_pushField = declare.Field( "frag.push", BoardType::Float, BoardScope::Global );
		m_breakRadiusField = declare.Field( "frag.break_radius", BoardType::Float, BoardScope::Global );
		m_rubbleField = declare.Field( "frag.rubble_seconds", BoardType::Float, BoardScope::Global );
		// The combat mod's (see combat.cpp): what a hit on a player is said with.
		m_damage = declare.Event( "combat.damage" );
		// Not for picking up, and not for a slot: it is on its way.
		declare.ItemProperty( m_grenade, "pickup.never", 1.0f );
	}

	void Start( Context& ctx ) override
	{
		m_centreDamage = float( std::clamp( ctx.Option( "frag.damage", 90.0 ), 0.0, 10000.0 ) );
		m_radius = float( std::clamp( ctx.Option( "frag.radius", 5.0 ), 0.5, 50.0 ) );
		m_breakRadius = float( std::clamp( ctx.Option( "frag.break_radius", double( m_radius ) * 0.5 ), 0.0, 50.0 ) );
		m_rubbleSeconds = float( std::clamp( ctx.Option( "frag.rubble_seconds", 0.0 ), 0.0, 3600.0 ) );
		m_push = float( std::clamp( ctx.Option( "frag.push", 12.0 ), 0.0, 100.0 ) );
		m_refillTicks = uint32_t( std::clamp( ctx.Option( "frag.refill_seconds", 1.2 ), 0.0, 600.0 ) * double( ctx.Config().tickRate ) + 0.5 );
		m_carry = int( std::clamp( ctx.Option( "frag.carry", 3.0 ), 1.0, 99.0 ) );
	}

	void Tick( Context& ctx ) override
	{
		if ( m_published == false )
		{
			ctx.SetFloat( 0, m_radiusField, m_radius );
			ctx.SetFloat( 0, m_pushField, m_push );
			ctx.SetFloat( 0, m_breakRadiusField, m_breakRadius );
			ctx.SetFloat( 0, m_rubbleField, m_rubbleSeconds );
			m_published = true;
		}
		Refill( ctx );
		// The blasts of the last few ticks throw those they killed, now that they have fallen.
		for ( size_t i = 0; i < m_recent.size(); )
		{
			if ( ctx.Tick() > m_recent[i].tick + kFallTicks )
			{
				m_recent.erase( m_recent.begin() + std::ptrdiff_t( i ) );
				continue;
			}
			Throw( ctx, m_recent[i].point, m_recent[i].tick );
			++i;
		}
		if ( m_recent.empty() )
		{
			m_flung.clear();
		}
		// The fuses that ran out on the tick before this one: the grenade is gone, where it was.
		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			if ( m_expired.Valid() && int( e.type ) == m_expired.index )
			{
				GoOff( ctx, e.netIdB, e.point );
			}
		}
	}

private:
	// A player in the world with fewer grenades than it carries gets one, a while after it had fewer
	// (it threw one); then the next, as long as it is short.
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
			int has = 0;
			for ( const CarriedItem& it : carried )
			{
				has += it.kind.index == m_grenade.index ? it.count : 0;
			}
			if ( has >= m_carry )
			{
				since = 0;
				continue;
			}
			since = since == 0 ? ctx.Tick() : since;
			if ( ctx.Tick() >= since + m_refillTicks )
			{
				ctx.GiveItem( SlotTarget( slot ), m_grenade );
				since = ctx.Tick(); // (the next one as long after; it is on the stack by then)
			}
		}
	}

	void GoOff( Context& ctx, uint32_t thrower, b3Vec3 point )
	{
		// Whose it was (for the kill): the thrower, as long as it is still here.
		int throwerSlot = thrower != 0 ? ctx.SlotOf( thrower ) : -1;
		uint32_t throwerTarget = throwerSlot >= 0 ? SlotTarget( PlayerSlot( throwerSlot ) ) : 0;

		// (The blast itself, its hole and what is loose thrown, are its sheet's: frag.rules.)
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
		}
		// The bodies lying near are flung (what is loose its sheet threw already); those it kills fall
		// a tick or two from now, and are flung as they fall (the blast is remembered that long).
		Throw( ctx, point, 0 );
		m_recent.push_back( { point, ctx.Tick() } );
	}

	// The bodies (ragdolls) within the blast are flung; with `fallenSince`, only those that fell at or
	// after that tick (the ones this blast killed).
	void Throw( Context& ctx, b3Vec3 point, uint32_t fallenSince )
	{
		for ( const NearBody& body : ctx.BodiesNear( point, m_radius ) )
		{
			if ( ( fallenSince != 0 && body.ragdollSince < fallenSince ) || std::find( m_flung.begin(), m_flung.end(), body.netId ) != m_flung.end() )
			{
				continue;
			}
			if ( body.ragdollSince == 0 )
			{
				continue; // (what is loose: its sheet's)
			}
			b3Vec3 to = b3Sub( body.position, point );
			float share = 1.0f - body.distance / m_radius;
			b3Vec3 away = body.distance > 0.01f ? b3MulSV( 1.0f / body.distance, to ) : b3Vec3{ 0.0f, 1.0f, 0.0f };
			// A body: the whole of it, up and away. Once per blast.
			m_flung.push_back( body.netId );
			ctx.Push( body.netId, body.position,
					  b3Add( b3MulSV( m_push * ( 0.3f + 0.5f * share ), away ), b3Vec3{ 0.0f, 0.45f * m_push * ( 0.3f + 0.5f * share ), 0.0f } ),
					  ImpulseThrow );
		}
	}

	ItemKindHandle m_grenade;
	EventHandle m_release;
	EventHandle m_thrown;
	EventHandle m_expired;
	EventHandle m_blast;
	EventHandle m_damage;
	FieldHandle m_radiusField;
	FieldHandle m_pushField;
	FieldHandle m_breakRadiusField;
	FieldHandle m_rubbleField;
	bool m_published = false;
	float m_centreDamage = 90.0f;
	float m_radius = 5.0f;
	float m_breakRadius = 2.5f;
	float m_rubbleSeconds = 0.0f;
	float m_push = 12.0f;
	uint32_t m_refillTicks = 72;
	int m_carry = 3;
	uint32_t m_without[kMaxPlayers] = {}; // the tick from which a player has been short (0: it has them all)
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

std::unique_ptr<cb::mods::ServerMod> CreateMod_frag()
{
	return std::make_unique<FragMod>();
}
