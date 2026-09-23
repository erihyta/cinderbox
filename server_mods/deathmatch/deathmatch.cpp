// Deathmatch: free-for-all rounds.
//
// A round lasts until someone reaches the kill limit or the time runs out. The best score wins, then
// everyone stands frozen through a short intermission while the winner is shown, and the next round
// starts clean: player-spawned props and bodies cleared, everyone respawned, scores back to zero.
//
// It scores off "combat.killed", which the pistol mod announces; it does not know what a pistol is,
// and would score any weapon mod that announces the same event. It announces "game.round_start" in
// turn, which the pistol uses to refill health and ammo. Mods cooperate through names.
//
// Options (cb_server --mod-option NAME=VALUE):
//   deathmatch.kills          kill limit (10)
//   deathmatch.round_seconds  round length (300)
//   deathmatch.pause_seconds  intermission between rounds (6)
//   deathmatch.fall_penalty   points lost for falling out of the world (1)

#include "mod_api.h"

#include <algorithm>
#include <cmath>

namespace
{

using namespace cb;
using namespace cb::mods;

enum Phase : int32_t
{
	Playing = 0,
	Intermission = 1,
};

class DeathmatchMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "deathmatch";
	}

	void Declare( Declarations& declare ) override
	{
		m_score = declare.Field( "deathmatch.score", BoardType::Int );
		m_phase = declare.Field( "deathmatch.phase", BoardType::Int, BoardScope::Global );
		m_seconds = declare.Field( "deathmatch.seconds", BoardType::Int, BoardScope::Global );
		m_winner = declare.Field( "deathmatch.winner", BoardType::Int, BoardScope::Global );
		m_round = declare.Field( "deathmatch.round", BoardType::Int, BoardScope::Global );
		m_limit = declare.Field( "deathmatch.kill_limit", BoardType::Int, BoardScope::Global );

		m_killed = declare.Event( "combat.killed" );
		// a = the winner (0: nobody scored), value = the winning score.
		m_roundEnd = declare.Event( "deathmatch.round_end" );
		m_roundStart = declare.Event( "game.round_start" );
	}

	void Start( Context& ctx ) override
	{
		uint32_t rate = ctx.Config().tickRate;
		m_killLimit = std::max( 1, int32_t( ctx.Option( "deathmatch.kills", 10.0 ) ) );
		m_roundTicks = uint32_t( std::max( 10.0, ctx.Option( "deathmatch.round_seconds", 300.0 ) ) * rate );
		m_pauseTicks = uint32_t( std::max( 1.0, ctx.Option( "deathmatch.pause_seconds", 6.0 ) ) * rate );
		m_fallPenalty = int32_t( ctx.Option( "deathmatch.fall_penalty", 1.0 ) );
		m_phaseEnd = ctx.Tick() + m_roundTicks;
	}

	void Tick( Context& ctx ) override
	{
		if ( m_published == false )
		{
			m_published = true;
			ctx.Set( 0, m_phase, Playing );
			ctx.Set( 0, m_round, m_roundNumber );
			ctx.Set( 0, m_limit, m_killLimit );
			ctx.Set( 0, m_winner, 0 );
		}

		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Joining( slot ) || ctx.Leaving( slot ) )
			{
				m_scores[i] = 0;
			}
			if ( ctx.Joining( slot ) )
			{
				ctx.Set( SlotTarget( slot ), m_score, 0 );
				if ( m_state == Intermission )
				{
					ctx.Freeze( SlotTarget( slot ), true );
				}
			}
		}

		if ( m_state == Playing )
		{
			Score( ctx );
			bool limitReached = std::any_of( m_scores, m_scores + kMaxPlayers, [&]( int32_t s ) { return s >= m_killLimit; } );
			if ( limitReached || ctx.Tick() >= m_phaseEnd )
			{
				EndRound( ctx );
			}
		}
		else if ( ctx.Tick() >= m_phaseEnd )
		{
			StartRound( ctx );
		}

		int32_t seconds = int32_t( std::ceil( double( m_phaseEnd - std::min( m_phaseEnd, ctx.Tick() ) ) / ctx.Config().tickRate ) );
		if ( seconds != m_lastSeconds )
		{
			m_lastSeconds = seconds;
			ctx.Set( 0, m_seconds, seconds );
		}
	}

private:
	void Score( Context& ctx )
	{
		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			if ( int( e.type ) != m_killed.index )
			{
				continue;
			}
			int killer = ctx.SlotOf( e.netIdA );
			int victim = ctx.SlotOf( e.netIdB );
			if ( killer >= 0 && killer != victim )
			{
				Add( ctx, killer, 1 );
			}
			else if ( e.netIdA == 0 && victim >= 0 )
			{
				Add( ctx, victim, -m_fallPenalty );
			}
		}
	}

	void Add( Context& ctx, int slot, int32_t points )
	{
		m_scores[slot] += points;
		ctx.Set( SlotTarget( PlayerSlot( slot ) ), m_score, m_scores[slot] );
	}

	void EndRound( Context& ctx )
	{
		int best = -1;
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			if ( ctx.InWorld( PlayerSlot( i ) ) && ( best < 0 || m_scores[i] > m_scores[best] ) )
			{
				best = i;
			}
		}
		uint32_t winner = best >= 0 && m_scores[best] > 0 ? ctx.PlayerNetId( PlayerSlot( best ) ) : 0;
		ctx.Set( 0, m_winner, int32_t( winner ) );
		ctx.Set( 0, m_phase, Intermission );
		ctx.Emit( m_roundEnd, winner, 0, best >= 0 ? m_scores[best] : 0 );
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			if ( ctx.InWorld( PlayerSlot( i ) ) )
			{
				ctx.Freeze( SlotTarget( PlayerSlot( i ) ), true );
			}
		}
		m_state = Intermission;
		m_phaseEnd = ctx.Tick() + m_pauseTicks;
	}

	void StartRound( Context& ctx )
	{
		// A clean arena: what players threw and what fell stays with the old round.
		for ( uint32_t id : ctx.SpawnedProps() )
		{
			ctx.Destroy( id );
		}
		for ( uint32_t id : ctx.Ragdolls() )
		{
			ctx.Destroy( id );
		}
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			m_scores[i] = 0;
			if ( ctx.InWorld( slot ) )
			{
				ctx.Respawn( SlotTarget( slot ) );
				ctx.Freeze( SlotTarget( slot ), false );
				ctx.Set( SlotTarget( slot ), m_score, 0 );
			}
		}
		m_roundNumber += 1;
		ctx.Set( 0, m_round, m_roundNumber );
		ctx.Set( 0, m_winner, 0 );
		ctx.Set( 0, m_phase, Playing );
		ctx.Emit( m_roundStart, 0 );
		m_state = Playing;
		m_phaseEnd = ctx.Tick() + m_roundTicks;
	}

	FieldHandle m_score;
	FieldHandle m_phase;
	FieldHandle m_seconds;
	FieldHandle m_winner;
	FieldHandle m_round;
	FieldHandle m_limit;
	EventHandle m_killed;
	EventHandle m_roundEnd;
	EventHandle m_roundStart;

	int32_t m_killLimit = 10;
	uint32_t m_roundTicks = 0;
	uint32_t m_pauseTicks = 0;
	int32_t m_fallPenalty = 1;

	Phase m_state = Playing;
	uint32_t m_phaseEnd = 0;
	int32_t m_roundNumber = 1;
	int32_t m_lastSeconds = -1;
	bool m_published = false;
	int32_t m_scores[kMaxPlayers] = {};
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_deathmatch()
{
	return std::make_unique<DeathmatchMod>();
}
