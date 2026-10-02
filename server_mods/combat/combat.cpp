// Combat: health, death and respawning, for every mod that hurts.
//
// The one place a player's health lives. Weapons do not own it and do not know each other: a mod
// that hurts someone says so with an event, and this mod decides what it means. Declare the names
// you use in your own mod (the same name is the same event or field) and that is the whole link:
//
//   you emit     combat.damage     a = attacker (0: nobody), b = who is hurt, value = damage,
//                                  point = where, vector = the push the body gets if it dies
//   you emit     combat.heal       a = who gave it (or 0), b = who is healed, value = health
//   you emit     game.round_start  everyone is alive again, at full health
//
//   you hear     combat.hurt       damage was taken: a = attacker, b = victim, value = health lost,
//                                  point = where
//   you hear     combat.killed     a = killer (0: the world, a fall), b = who died
//   you hear     combat.respawned  a = who is back: after dying, or with a new round
//
//   you read     combat.health, combat.max_health, combat.dead, combat.kills, combat.deaths
//                (a player's Character::dead says the same as combat.dead, a tick sooner)
//
// Options (cb_server --mod-option combat.max_health=150):
//   combat.max_health       health at the start of a life (100)
//   combat.respawn_seconds  from death to the next life (3)
//   combat.ragdoll_seconds  how long a body lies (10)
//   combat.ragdoll_cap      how many bodies lie at once (16)
//   combat.fall_counts      falling out of the world counts as a death (1; 0: it does not)
//
// Without this mod a server has no health: weapons fire and report their hits, and nobody dies.
// Events are heard a tick after they are sent, so damage lands the tick after the hit.
//
// State is kept in the mods' shared flecs world: one entity per player with a Fighter, plus Dead
// while that lasts. Nothing here is rolled back or sent anywhere; what clients need is published
// through the board.

#include "mod_api.h"

#include <algorithm>

namespace
{

using namespace cb;
using namespace cb::mods;

struct Fighter
{
	PlayerSlot slot = 0;
	int32_t health = 0;
	int32_t kills = 0;
	int32_t deaths = 0;
	uint32_t falls = 0; // Character::fallCount last seen
};

struct Dead
{
	uint32_t respawnTick = 0;
	uint32_t diedTick = 0; // the Kill command is applied in this tick's frame
};

uint32_t Ticks( const Context& ctx, double seconds )
{
	return uint32_t( seconds * double( ctx.Config().tickRate ) + 0.5 );
}

class CombatMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "combat";
	}

	void Declare( Declarations& declare ) override
	{
		m_health = declare.Field( "combat.health", BoardType::Int );
		m_maxHealthField = declare.Field( "combat.max_health", BoardType::Int );
		m_dead = declare.Field( "combat.dead", BoardType::Bool );
		m_kills = declare.Field( "combat.kills", BoardType::Int );
		m_deaths = declare.Field( "combat.deaths", BoardType::Int );

		m_damage = declare.Event( "combat.damage" );
		m_heal = declare.Event( "combat.heal" );
		m_roundStart = declare.Event( "game.round_start" );
		m_hurt = declare.Event( "combat.hurt" );
		m_killed = declare.Event( "combat.killed" );
		m_respawned = declare.Event( "combat.respawned" );
	}

	void Start( Context& ctx ) override
	{
		flecs::world& world = ctx.World();
		world.component<Fighter>();
		world.component<Dead>();
		m_fighters = world.query<Fighter>();

		m_maxHealth = std::max( 1, int32_t( ctx.Option( "combat.max_health", 100.0 ) ) );
		m_respawnTicks = Ticks( ctx, std::max( 0.0, ctx.Option( "combat.respawn_seconds", 3.0 ) ) );
		m_ragdollTicks = Ticks( ctx, std::max( 0.0, ctx.Option( "combat.ragdoll_seconds", 10.0 ) ) );
		m_ragdollCap = uint32_t( std::max( 0.0, ctx.Option( "combat.ragdoll_cap", 16.0 ) ) );
		m_fallCounts = ctx.Option( "combat.fall_counts", 1.0 ) != 0.0;
	}

	void Tick( Context& ctx ) override
	{
		flecs::world& world = ctx.World();

		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Leaving( slot ) && m_bySlot[i].is_valid() )
			{
				m_bySlot[i].destruct();
				m_bySlot[i] = flecs::entity();
			}
			if ( ctx.Joining( slot ) )
			{
				if ( m_bySlot[i].is_valid() )
				{
					m_bySlot[i].destruct();
				}
				Fighter f;
				f.slot = slot;
				f.health = m_maxHealth;
				m_bySlot[i] = world.entity().set<Fighter>( f );
				Publish( ctx, f, false );
			}
		}

		bool newRound = false;
		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			if ( int( e.type ) == m_roundStart.index )
			{
				newRound = true;
			}
			else if ( int( e.type ) == m_damage.index )
			{
				Hurt( ctx, e.netIdA, e.netIdB, e.value, e.point, e.vector );
			}
			else if ( int( e.type ) == m_heal.index )
			{
				Heal( ctx, e.netIdB, e.value );
			}
		}

		// Collected first: a death adds Dead, which moves the entity between tables.
		m_scratch.clear();
		m_fighters.each( [&]( flecs::entity e, Fighter& ) { m_scratch.push_back( e ); } );
		for ( flecs::entity e : m_scratch )
		{
			if ( e.is_alive() == false )
			{
				continue;
			}
			// Works on a copy that is written back: see above.
			Fighter f = e.get<Fighter>();
			if ( newRound )
			{
				NewLife( ctx, e, f );
			}
			else
			{
				Update( ctx, e, f );
			}
			e.set<Fighter>( f );
		}
	}

private:
	void Publish( Context& ctx, const Fighter& f, bool dead )
	{
		uint32_t target = SlotTarget( f.slot );
		ctx.Set( target, m_health, f.health );
		ctx.Set( target, m_maxHealthField, m_maxHealth );
		ctx.Set( target, m_kills, f.kills );
		ctx.Set( target, m_deaths, f.deaths );
		ctx.Set( target, m_dead, dead ? 1 : 0 );
	}

	// Full health, alive on the board, and the news of it. Bringing the body back is the caller's
	// (or whoever started the round's).
	void NewLife( Context& ctx, flecs::entity e, Fighter& f )
	{
		e.remove<Dead>();
		f.health = m_maxHealth;
		Publish( ctx, f, false );
		ctx.Emit( m_respawned, SlotTarget( f.slot ) );
	}

	void Update( Context& ctx, flecs::entity e, Fighter& f )
	{
		const Character* c = ctx.PlayerCharacter( f.slot );
		if ( ctx.PlayerNetId( f.slot ) == 0 || c == nullptr )
		{
			return; // joining this tick: the player exists from the next one
		}
		uint32_t target = SlotTarget( f.slot );
		uint32_t tick = ctx.Tick();

		if ( const Dead* dead = e.try_get<Dead>() )
		{
			// Alive in the world after the kill was applied: someone else brought the player back,
			// so stop waiting. In the tick of the kill itself the world has not applied it yet and
			// still shows the player alive.
			if ( c->dead == 0 && tick > dead->diedTick )
			{
				NewLife( ctx, e, f );
			}
			else if ( tick >= dead->respawnTick )
			{
				f.falls = c->fallCount;
				ctx.Respawn( target );
				NewLife( ctx, e, f );
			}
			return;
		}

		// Falling out of the world: the engine already put the player back; here it counts.
		if ( c->fallCount != f.falls )
		{
			f.falls = c->fallCount;
			if ( m_fallCounts )
			{
				f.deaths += 1;
				f.health = m_maxHealth;
				ctx.Set( target, m_deaths, f.deaths );
				ctx.Set( target, m_health, f.health );
				ctx.Emit( m_killed, 0, target );
			}
		}
	}

	flecs::entity FighterOf( Context& ctx, uint32_t netId )
	{
		int slot = ctx.SlotOf( netId );
		return slot >= 0 ? m_bySlot[slot] : flecs::entity();
	}

	void Hurt( Context& ctx, uint32_t attackerNetId, uint32_t victimNetId, int32_t damage, b3Vec3 point, b3Vec3 push )
	{
		flecs::entity victim = FighterOf( ctx, victimNetId );
		if ( victim.is_valid() == false || victim.has<Dead>() || damage <= 0 )
		{
			return;
		}
		Fighter v = victim.get<Fighter>();
		uint32_t victimTarget = SlotTarget( v.slot );
		flecs::entity attacker = FighterOf( ctx, attackerNetId );
		uint32_t attackerTarget = attacker.is_valid() ? SlotTarget( attacker.get<Fighter>().slot ) : 0;

		int32_t lost = std::min( damage, v.health );
		v.health -= lost;
		ctx.Set( victimTarget, m_health, v.health );
		ctx.Emit( m_hurt, attackerTarget, victimTarget, lost, point );
		if ( v.health > 0 )
		{
			victim.set<Fighter>( v );
			return;
		}

		ctx.Kill( victimTarget, true, point, push, m_ragdollTicks, m_ragdollCap );
		v.deaths += 1;
		victim.set<Fighter>( v );
		victim.set<Dead>( { ctx.Tick() + m_respawnTicks, ctx.Tick() } );
		ctx.Set( victimTarget, m_dead, 1 );
		ctx.Set( victimTarget, m_deaths, v.deaths );
		// Killing yourself is a death, not a kill.
		if ( attacker.is_valid() && attacker != victim )
		{
			Fighter a = attacker.get<Fighter>();
			a.kills += 1;
			attacker.set<Fighter>( a );
			ctx.Set( attackerTarget, m_kills, a.kills );
		}
		ctx.Emit( m_killed, attacker != victim ? attackerTarget : 0, victimTarget );
	}

	void Heal( Context& ctx, uint32_t netId, int32_t amount )
	{
		flecs::entity who = FighterOf( ctx, netId );
		if ( who.is_valid() == false || who.has<Dead>() || amount <= 0 )
		{
			return;
		}
		Fighter f = who.get<Fighter>();
		f.health = std::min( f.health + amount, m_maxHealth );
		who.set<Fighter>( f );
		ctx.Set( SlotTarget( f.slot ), m_health, f.health );
	}

	FieldHandle m_health;
	FieldHandle m_maxHealthField;
	FieldHandle m_dead;
	FieldHandle m_kills;
	FieldHandle m_deaths;
	EventHandle m_damage;
	EventHandle m_heal;
	EventHandle m_roundStart;
	EventHandle m_hurt;
	EventHandle m_killed;
	EventHandle m_respawned;

	int32_t m_maxHealth = 100;
	uint32_t m_respawnTicks = 0;
	uint32_t m_ragdollTicks = 0;
	uint32_t m_ragdollCap = 0;
	bool m_fallCounts = true;

	flecs::query<Fighter> m_fighters;
	flecs::entity m_bySlot[kMaxPlayers];
	std::vector<flecs::entity> m_scratch;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_combat()
{
	return std::make_unique<CombatMod>();
}
