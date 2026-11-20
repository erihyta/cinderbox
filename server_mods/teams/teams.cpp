// Teams (M276): players in teams, for a mode played in teams (the slice's team deathmatch:
// cb_server --preset tdm). Off by default: it runs when --mods names it.
//
// A player joining goes to the team with the fewest players (the lowest number on a tie) and stays
// in it until it leaves. Its team is a field, team.id (1, 2, ...; 0: none), on whatever body it
// drives, shared: every game has its own and the others' (a look colours by it).
//
// What a team means is the other mods', by that name (they declare team.id too):
//   combat      teammates do not hurt each other (combat.friendly_fire 1: they do)
//   deathmatch  scores the teams, and a round ends at deathmatch.team_kills
//   brawl       the finisher does not toss a teammate
//
// Options: teams.count (2; 2 to 4).

#include "mod_api.h"

#include <algorithm>

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
			}
			// Another body (a possession): the team goes on its board too.
			const uint32_t body = ctx.PlayerNetId( slot );
			if ( body != 0 && m_bodies[i] != 0 && body != m_bodies[i] && ctx.Joining( slot ) == false && m_teamOf[i] != 0 )
			{
				ctx.Set( SlotTarget( slot ), m_team, m_teamOf[i] );
			}
			m_bodies[i] = body;
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

	FieldHandle m_team;
	FieldHandle m_count;
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
