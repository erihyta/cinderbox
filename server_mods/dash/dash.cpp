// Dashing, and a second jump in the air: the example of motions (sim/motions.h).
//
// What a dash is (a burst of speed along where you walk, a moment without friction) and what a
// double jump is are not in this file: they are CbMotion nodes in the mod's client project
// (server_mods/dash/client/motion_sets/dash_moves.tscn), baked to motions/dash.moves.cfg and run by
// every simulation from the player's own input. So a dash starts on the tick of the press on the
// player's own screen, with no round trip, and this mod never sees the key.
//
// This file is the rules: the names the motions use, and how many dashes a player has. The dash
// motion asks "dash.charges > 0" and takes one; the mod gives them back, one at a time.
//
// Server options: dash.charges (how many a player can hold, default 2), dash.recharge_seconds
// (how long one takes to come back, default 2).

#include "mod_api.h"

#include <algorithm>
#include <array>

using namespace cb;
using namespace cb::mods;

namespace
{

class DashMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "dash";
	}

	void Declare( Declarations& declare ) override
	{
		// The motions read and write these by name.
		m_dash = declare.Action( "dash", "V" );
		m_charges = declare.Field( "dash.charges", BoardType::Int );
		m_max = declare.Field( "dash.max", BoardType::Int, BoardScope::Global );
		m_started = declare.Event( "dash.started" );
		m_doubleJump = declare.Event( "dash.double_jump" );
		m_moves = declare.Motions( "dash.moves" );
	}

	void Start( Context& ctx ) override
	{
		m_capacity = std::clamp( int( ctx.Option( "dash.charges", 2.0 ) ), 0, 99 );
		double seconds = std::clamp( ctx.Option( "dash.recharge_seconds", 2.0 ), 0.05, 3600.0 );
		m_rechargeTicks = std::max<uint32_t>( 1, uint32_t( seconds * double( ctx.Config().tickRate ) + 0.5 ) );
	}

	void Tick( Context& ctx ) override
	{
		if ( m_published == false )
		{
			// For the HUD: "DASH 1 / 2".
			ctx.Set( 0, m_max, m_capacity );
			m_published = true;
		}
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Joining( slot ) )
			{
				// A new player starts full.
				ctx.Set( SlotTarget( slot ), m_charges, m_capacity );
				m_waited[size_t( i )] = 0;
				continue;
			}
			if ( ctx.InWorld( slot ) == false || ctx.Leaving( slot ) )
			{
				continue;
			}
			// The world before this tick: a dash this very tick takes its charge after this Set has
			// been applied (commands come first), so neither is lost.
			int charges = ctx.Get( ctx.PlayerNetId( slot ), m_charges );
			if ( charges >= m_capacity )
			{
				m_waited[size_t( i )] = 0;
				continue;
			}
			if ( ++m_waited[size_t( i )] >= m_rechargeTicks )
			{
				m_waited[size_t( i )] = 0;
				ctx.Set( SlotTarget( slot ), m_charges, charges + 1 );
			}
		}
	}

private:
	ActionHandle m_dash;
	FieldHandle m_charges;
	FieldHandle m_max;
	EventHandle m_started;
	EventHandle m_doubleJump;
	MotionsHandle m_moves;
	int m_capacity = 2;
	bool m_published = false;
	uint32_t m_rechargeTicks = 120;
	std::array<uint32_t, kMaxPlayers> m_waited{};
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_dash()
{
	return std::make_unique<DashMod>();
}
