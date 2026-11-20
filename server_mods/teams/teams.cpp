// Teams (M276, chosen and spawned apart in M277): players in teams, for a mode played in teams (the
// slice's team deathmatch: cb_server --preset tdm). Off by default: it runs when --mods names it.
//
// A player joining is put in the team with the fewest players (the lowest number on a tie), so it
// can play at once, and picks its own on the team screen (its look: ui/hud_teams.tscn, on M): a
// button sends the intent "teams.join" with the team's number. A living player that changes team
// starts again at its new team's spawn; a dead one comes back there. Its team is a field, team.id
// (1, 2, ...; 0: none), on whatever body it drives, shared: every game has everyone's (the
// scoreboard shows it). teams.chosen (private) is 1 once the player picked one itself.
//
// Where a team spawns is the map's: a CbSpawn tagged "team1", "team2" (Context::Place). Each
// player of the team stands somewhere within its radius, facing where it faces; on a map without
// them, at the map's spawn as everyone does.
//
// What a team means is the other mods', by that name (they declare team.id too):
//   combat      teammates do not hurt each other (combat.friendly_fire 1: they do)
//   deathmatch  scores the teams, and a round ends at deathmatch.team_kills
//   brawl       the finisher does not toss a teammate
//
// Options: teams.count (2; 2 to 4).

#include "mod_api.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr int kMostTeams = 4;

class TeamsMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "teams";
	}

	void Declare( Declarations& declare ) override
	{
		m_team = declare.Field( "team.id", BoardType::Int, BoardScope::Entity, true );
		m_count = declare.Field( "teams.count", BoardType::Int, BoardScope::Global );
		m_chosen = declare.Field( "teams.chosen", BoardType::Bool, BoardScope::Private );
		// a = the team's number (1, 2, ...).
		m_join = declare.Intent( "teams.join" );
		// At the player: value = the team it is in now.
		m_joined = declare.Event( "teams.joined" );
		// The combat mod's (a = who is back): after dying, or with a new round.
		m_respawned = declare.Event( "combat.respawned" );
	}

	void Start( Context& ctx ) override
	{
		m_teams = std::clamp( int( ctx.Option( "teams.count", 2.0 ) ), 2, kMostTeams );
	}

	void Tick( Context& ctx ) override
	{
		if ( m_published == false )
		{
			m_published = true;
			ctx.Set( 0, m_count, m_teams );
		}
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			const PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Leaving( slot ) )
			{
				m_teamOf[i] = 0;
				m_bodies[i] = 0;
			}
			if ( ctx.Joining( slot ) )
			{
				m_teamOf[i] = Smallest();
				ctx.Set( SlotTarget( slot ), m_team, m_teamOf[i] );
				ctx.Set( SlotTarget( slot ), m_chosen, 0 );
				SpawnAtTeam( ctx, slot ); // (its commands come after the join: it stands there from its first tick)
			}
			// Another body (a possession): the team goes on its board too.
			const uint32_t body = ctx.PlayerNetId( slot );
			if ( body != 0 && m_bodies[i] != 0 && body != m_bodies[i] && ctx.Joining( slot ) == false && m_teamOf[i] != 0 )
			{
				ctx.Set( SlotTarget( slot ), m_team, m_teamOf[i] );
			}
			m_bodies[i] = body;
		}

		// A team picked on the screen.
		for ( const IntentSent& asked : ctx.Intents( m_join ) )
		{
			const int32_t team = int32_t( asked.a );
			const int i = int( asked.slot );
			if ( team < 1 || team > m_teams || ctx.PlayerNetId( asked.slot ) == 0 )
			{
				continue;
			}
			ctx.Set( SlotTarget( asked.slot ), m_chosen, 1 );
			if ( team == m_teamOf[i] )
			{
				continue;
			}
			m_teamOf[i] = team;
			ctx.Set( SlotTarget( asked.slot ), m_team, team );
			ctx.Emit( m_joined, SlotTarget( asked.slot ), 0, team );
			const Character* c = ctx.PlayerCharacter( asked.slot );
			if ( c != nullptr && c->dead == 0 )
			{
				SpawnAtTeam( ctx, asked.slot ); // (a dead one comes back there anyway)
			}
		}

		// Back from the dead, or a new round: at its team's spawn. (The respawn put it at the map's;
		// heard a tick later, so it stands there for one tick.)
		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			const int slot = int( e.type ) == m_respawned.index ? ctx.SlotOf( e.netIdA ) : -1;
			if ( slot >= 0 )
			{
				SpawnAtTeam( ctx, PlayerSlot( slot ) );
			}
		}
	}

private:
	int32_t Smallest() const
	{
		int sizes[kMostTeams + 1] = {};
		for ( int32_t team : m_teamOf )
		{
			sizes[team] += team > 0 ? 1 : 0;
		}
		int32_t best = 1;
		for ( int32_t team = 2; team <= m_teams; ++team )
		{
			best = sizes[team] < sizes[best] ? team : best;
		}
		return best;
	}

	// Somewhere within the team's place (the map's "teamN"), each slot its own spot: on a sunflower's
	// spiral, so a team's players never stand in each other.
	void SpawnAtTeam( Context& ctx, PlayerSlot slot )
	{
		const int32_t team = m_teamOf[slot];
		const LevelPlace* place = team > 0 ? ctx.Place( "team" + std::to_string( team ) ) : nullptr;
		if ( place == nullptr )
		{
			return;
		}
		const float k = float( slot % 32 );
		const float distance = place->radius * std::sqrt( ( k + 0.5f ) / 32.0f );
		const float angle = 2.39996323f * k;
		const b3Vec3 at = { place->at.x + distance * std::cos( angle ), place->at.y, place->at.z + distance * std::sin( angle ) };
		ctx.RespawnAt( SlotTarget( slot ), at, place->yaw );
	}

	FieldHandle m_team;
	FieldHandle m_count;
	FieldHandle m_chosen;
	IntentHandle m_join;
	EventHandle m_joined;
	EventHandle m_respawned;
	int m_teams = 2;
	bool m_published = false;
	int32_t m_teamOf[kMaxPlayers] = {};
	uint32_t m_bodies[kMaxPlayers] = {};
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_teams()
{
	return std::make_unique<TeamsMod>();
}
