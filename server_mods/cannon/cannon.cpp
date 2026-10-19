// Cannon: walls broken by what hits them, not by a blast. The example of ctx.Hits and ctx.Break
// together: no explosion anywhere.
//
// The cannon is not in this file. It is an item (client/prefabs/cannon.tscn, a CbItem used once a
// press, its own key N, wherever it is carried) whose use launches a new cannon ball
// (client/prefabs/ball.tscn: 8 kg, 15 cm across, fast enough that the simulation traces it so it
// never passes through a thin wall) along the look, gone again after 10 s. All of that is its
// scene's, and every simulation does it from the player's own input.
//
// This file is the rules of a hit. A ball that meets a wall that breaks (a map's CbBreakable, or a
// piece of it a hole left standing) faster than cannon.break_speed knocks a hole in it: as wide as
// it was fast, its pieces flung the way the ball was going. The ball goes on through, slower (it
// has bounced off already when the wall breaks: it is given back a share of the speed it had). A
// ball that hits a player hurts it (the combat mod's "combat.damage"), by how fast it was going.
//
// Server options: cannon.break_speed (m/s, default 12: slower, it only bounces), cannon.hole (the
// hole's radius at 30 m/s, metres, default 0.9; faster, wider), cannon.through (the share of its
// speed a ball keeps going through, default 0.6; 0: it bounces off), cannon.damage (to a player at
// 30 m/s, default 70), cannon.rubble_seconds (how long the pieces stay, default 0: for good).
//
// Off by default: a server runs it by name (cb_server --mods ...,cannon).

#include "mod_api.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace cb;
using namespace cb::mods;

namespace
{

// The speed the hole and the damage are measured at (the cannon's own: its scene launches at 36).
constexpr float kReferenceSpeed = 30.0f;

class CannonMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "cannon";
	}

	void Declare( Declarations& declare ) override
	{
		// The cannon and its ball, from its look.
		declare.ItemsOfLook();
		m_ball = declare.ItemKind( "cannon.ball" );
		// Said by the cannon's use, and where a ball breaks a wall (the look's dust and crash).
		m_fired = declare.Event( "cannon.fired" );
		m_smash = declare.Event( "cannon.smash" );
		// The combat mod's (see combat.cpp): what a hit on a player is said with.
		m_damage = declare.Event( "combat.damage" );
		// A ball lying about is not for picking up.
		declare.ItemProperty( m_ball, "pickup.never", 1.0f );
	}

	void Start( Context& ctx ) override
	{
		m_breakSpeed = float( std::clamp( ctx.Option( "cannon.break_speed", 12.0 ), 0.0, 1000.0 ) );
		m_hole = float( std::clamp( ctx.Option( "cannon.hole", 0.9 ), 0.0, 20.0 ) );
		m_through = float( std::clamp( ctx.Option( "cannon.through", 0.6 ), 0.0, 1.0 ) );
		m_hurt = float( std::clamp( ctx.Option( "cannon.damage", 70.0 ), 0.0, 10000.0 ) );
		m_rubbleSeconds = float( std::clamp( ctx.Option( "cannon.rubble_seconds", 0.0 ), 0.0, 3600.0 ) );
	}

	void Tick( Context& ctx ) override
	{
		// What the balls ran into on the tick before this one (each ball once).
		std::vector<uint32_t> done;
		for ( const Context::ItemHit& hit : ctx.Hits( m_ball ) )
		{
			if ( hit.speed < m_breakSpeed || std::find( done.begin(), done.end(), hit.item ) != done.end() )
			{
				continue; // a roll, a nudge: nothing
			}
			if ( ctx.IsBreakable( hit.other ) )
			{
				done.push_back( hit.item );
				Smash( ctx, hit );
			}
			else if ( ctx.IsPlayer( hit.other ) )
			{
				done.push_back( hit.item );
				Hurt( ctx, hit );
			}
		}
		// How each ball in the world moves now: what it had before whatever it hits next.
		m_before.clear();
		for ( uint32_t item : ctx.Items() )
		{
			if ( ctx.ItemKindOf( item ).index == m_ball.index && ctx.ItemHolder( item ) == 0 )
			{
				m_before.push_back( { item, ctx.EntityVelocity( item ) } );
			}
		}
	}

private:
	struct Moving
	{
		uint32_t item = 0;
		b3Vec3 velocity = {};
	};

	// How it was going before the hit (it has bounced since); as it is now when it was not seen.
	b3Vec3 Before( Context& ctx, uint32_t item ) const
	{
		for ( const Moving& m : m_before )
		{
			if ( m.item == item )
			{
				return m.velocity;
			}
		}
		return ctx.EntityVelocity( item );
	}

	void Smash( Context& ctx, const Context::ItemHit& hit )
	{
		const b3Vec3 before = Before( ctx, hit.item );
		float speed = b3Length( before );
		const b3Vec3 way = speed > 0.01f ? b3MulSV( 1.0f / speed, before ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
		speed = std::max( speed, hit.speed );
		// A hole as wide as it was fast; its pieces flung on the way the ball was going (the "blast"
		// is half a metre behind where it met the wall, on its side).
		const float radius = std::clamp( m_hole * speed / kReferenceSpeed, 0.25f, 4.0f );
		ctx.Break( hit.other, b3Sub( hit.point, b3MulSV( 0.5f, way ) ), 0.5f * speed, m_rubbleSeconds, radius );
		ctx.Emit( m_smash, 0, hit.item, int32_t( radius * 100.0f ), hit.point, way );
		// On through the hole, slower: the speed it had, a share of it, in place of the bounce.
		const Transform* at = ctx.EntityTransform( hit.item );
		if ( m_through > 0.0f && at != nullptr )
		{
			const b3Vec3 change = b3Sub( b3MulSV( m_through, before ), ctx.EntityVelocity( hit.item ) );
			ctx.Push( hit.item, at->position, change, ImpulseVelocity );
		}
	}

	void Hurt( Context& ctx, const Context::ItemHit& hit )
	{
		const int slot = ctx.SlotOf( hit.other );
		const Character* c = slot >= 0 ? ctx.PlayerCharacter( PlayerSlot( slot ) ) : nullptr;
		if ( c == nullptr || c->dead != 0 )
		{
			return;
		}
		const b3Vec3 before = Before( ctx, hit.item );
		const float speed = std::max( b3Length( before ), hit.speed );
		const int32_t damage = int32_t( m_hurt * std::min( speed / kReferenceSpeed, 1.5f ) + 0.5f );
		// Whose ball it was (for the kill): the one who fired it, as long as it is still here.
		const uint32_t shooter = ctx.ThrownBy( hit.item );
		const int shooterSlot = shooter != 0 ? ctx.SlotOf( shooter ) : -1;
		const uint32_t by = shooterSlot >= 0 ? SlotTarget( PlayerSlot( shooterSlot ) ) : 0;
		const b3Vec3 way = speed > 0.01f ? b3MulSV( 1.0f / std::max( b3Length( before ), 0.01f ), before ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
		const b3Vec3 push = b3Add( b3MulSV( 0.2f * speed, way ), b3Vec3{ 0.0f, 2.0f, 0.0f } );
		if ( damage > 0 )
		{
			ctx.Emit( m_damage, by, hit.other, damage, hit.point, push );
		}
	}

	ItemKindHandle m_ball;
	EventHandle m_fired;
	EventHandle m_smash;
	EventHandle m_damage;
	float m_breakSpeed = 12.0f;
	float m_hole = 0.9f;
	float m_through = 0.6f;
	float m_hurt = 70.0f;
	float m_rubbleSeconds = 0.0f;
	std::vector<Moving> m_before;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_cannon()
{
	return std::make_unique<CannonMod>();
}
