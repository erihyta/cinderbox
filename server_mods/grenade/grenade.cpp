// A grenade: the example of a launch (a CbLaunch, sim/motions.h).
//
// What throwing is (a key, how hard, how often, what leaves the hand) is not in this file: it is a
// CbMotion with a CbLaunch in the mod's client project
// (server_mods/grenade/client/motion_sets/grenade_moves.tscn), baked to motions/grenade.moves.cfg
// and run by every simulation from the player's own input. So the grenade leaves on the tick of
// the press on the thrower's own screen, flies and bounces as its body says (prefabs/shell.tscn: a
// CbItem), and this mod never sees the key.
//
// This file is the rules: what a grenade does when it hits something. It goes off: everyone near
// is hurt (the combat mod's "combat.damage", like any other weapon's: less with distance) and
// thrown back, and the grenade is gone. One that hits nothing hard enough is removed when its time
// is up (the launch's seconds).
//
// Server options: grenade.damage (at the centre, default 80), grenade.radius (metres, default 4),
// grenade.arm_speed (how hard it must hit to go off, m/s, default 3).

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
		// The motion reads these by name.
		m_throw = declare.Action( "throw", "H" );
		m_shell = declare.ItemKind( "grenade.shell" );
		m_thrown = declare.Event( "grenade.thrown" );
		m_blast = declare.Event( "grenade.blast" );
		m_moves = declare.Motions( "grenade.moves" );
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
	}

	void Tick( Context& ctx ) override
	{
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
			// Thrown back, hurt or not (the thrower too: a grenade at your feet is a jump).
			ctx.Push( ctx.PlayerNetId( slot ), at->position, push, ImpulseVelocity );
		}
		ctx.Destroy( item );
	}

	ActionHandle m_throw;
	ItemKindHandle m_shell;
	EventHandle m_thrown;
	EventHandle m_blast;
	EventHandle m_damage;
	MotionsHandle m_moves;
	float m_centreDamage = 80.0f;
	float m_radius = 4.0f;
	float m_armSpeed = 3.0f;
	std::vector<uint32_t> m_gone;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_grenade()
{
	return std::make_unique<GrenadeMod>();
}
