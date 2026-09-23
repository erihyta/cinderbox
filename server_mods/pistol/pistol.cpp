// Pistol: hitscan shooting, health, death with a ragdoll, and respawning.
//
// Every rule lives here, on the server. Clients never learn what a pistol is: they see board fields
// ("pistol.ammo", "combat.health"), events ("pistol.fired", "combat.killed") and the ragdoll the
// simulation creates, and their data bindings decide what to draw and play for each.
//
// State is kept in the mods' shared flecs world: one entity per player with a Gunner, plus Dead and
// Reloading while those last. Nothing here is rolled back or sent anywhere; what clients need is
// published through the board.

#include "mod_api.h"

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr int32_t kMaxHealth = 100;
constexpr int32_t kDamage = 25;
constexpr int32_t kMagazine = 12;
constexpr float kFireSeconds = 0.2f;
constexpr float kReloadSeconds = 1.5f;
constexpr float kRespawnSeconds = 3.0f;
constexpr float kRange = 80.0f;
// How hard a hit shoves: a ragdoll at death, and anything loose that is hit.
constexpr float kDeathPush = 6.0f;
constexpr float kPropPush = 4.0f;
constexpr float kRagdollSeconds = 10.0f;
constexpr uint32_t kRagdollCap = 16;
constexpr int32_t kPistolSlot = 2; // loadout.slot value while the pistol is out

struct Gunner
{
	PlayerSlot slot = 0;
	int32_t health = kMaxHealth;
	int32_t ammo = kMagazine;
	uint32_t nextShotTick = 0;
	int32_t kills = 0;
	int32_t deaths = 0;
	uint32_t falls = 0; // Character::fallCount last seen
};

struct Dead
{
	uint32_t respawnTick = 0;
};

struct Reloading
{
	uint32_t doneTick = 0;
};

uint32_t Ticks( const Context& ctx, float seconds )
{
	return uint32_t( seconds * float( ctx.Config().tickRate ) + 0.5f );
}

class PistolMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "pistol";
	}

	void Declare( Declarations& declare ) override
	{
		m_fire = declare.Action( "fire", "MouseLeft" );
		m_reload = declare.Action( "reload", "R" );
		m_loadout = declare.Field( "loadout.slot", BoardType::Int );

		m_health = declare.Field( "combat.health", BoardType::Int );
		m_maxHealth = declare.Field( "combat.max_health", BoardType::Int );
		m_dead = declare.Field( "combat.dead", BoardType::Bool );
		m_kills = declare.Field( "combat.kills", BoardType::Int );
		m_deaths = declare.Field( "combat.deaths", BoardType::Int );
		m_ammo = declare.Field( "pistol.ammo", BoardType::Int );
		m_reloading = declare.Field( "pistol.reloading", BoardType::Bool );

		// a = shooter, b = what the ray hit (0: nothing), point = where the shot came from,
		// vector = where it ended.
		m_fired = declare.Event( "pistol.fired" );
		// a = shooter, b = what was hit, value = damage done, point = where, vector = surface normal.
		m_hit = declare.Event( "pistol.hit" );
		m_reloadEvent = declare.Event( "pistol.reload" );
		m_dry = declare.Event( "pistol.dry" );
		// a = killer (0: the world, e.g. a fall), b = who died.
		m_killed = declare.Event( "combat.killed" );
	}

	void Start( Context& ctx ) override
	{
		flecs::world& world = ctx.World();
		world.component<Gunner>();
		world.component<Dead>();
		world.component<Reloading>();
		m_gunners = world.query<Gunner>();
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
				Gunner g;
				g.slot = slot;
				m_bySlot[i] = world.entity().set<Gunner>( g );
				Publish( ctx, g, false, false );
			}
		}

		// Collected first: firing changes other players' components (a kill adds Dead).
		m_scratch.clear();
		m_gunners.each( [&]( flecs::entity e, Gunner& ) { m_scratch.push_back( e ); } );
		for ( flecs::entity e : m_scratch )
		{
			if ( e.is_alive() )
			{
				Update( ctx, e );
			}
		}
	}

private:
	void Publish( Context& ctx, const Gunner& g, bool dead, bool reloading )
	{
		uint32_t target = SlotTarget( g.slot );
		ctx.Set( target, m_health, g.health );
		ctx.Set( target, m_maxHealth, kMaxHealth );
		ctx.Set( target, m_ammo, g.ammo );
		ctx.Set( target, m_kills, g.kills );
		ctx.Set( target, m_deaths, g.deaths );
		ctx.Set( target, m_dead, dead ? 1 : 0 );
		ctx.Set( target, m_reloading, reloading ? 1 : 0 );
	}

	// Works on a copy that is written back at the end: adding or removing Dead and Reloading moves
	// entities between tables, which would leave a reference into the old one dangling.
	void Update( Context& ctx, flecs::entity e )
	{
		Gunner g = e.get<Gunner>();
		UpdateGunner( ctx, e, g );
		if ( e.is_alive() )
		{
			e.set<Gunner>( g );
		}
	}

	void UpdateGunner( Context& ctx, flecs::entity e, Gunner& g )
	{
		uint32_t netId = ctx.PlayerNetId( g.slot );
		const Character* c = ctx.PlayerCharacter( g.slot );
		if ( netId == 0 || c == nullptr )
		{
			return; // joining this tick: the player exists from the next one
		}
		uint32_t target = SlotTarget( g.slot );
		uint32_t tick = ctx.Tick();

		if ( const Dead* dead = e.try_get<Dead>() )
		{
			if ( tick >= dead->respawnTick )
			{
				e.remove<Dead>();
				g.health = kMaxHealth;
				g.ammo = kMagazine;
				g.falls = c->fallCount;
				ctx.Respawn( target );
				Publish( ctx, g, false, false );
			}
			return;
		}

		// Falling out of the world: the engine already put the player back; here it counts.
		if ( c->fallCount != g.falls )
		{
			g.falls = c->fallCount;
			g.deaths += 1;
			g.health = kMaxHealth;
			ctx.Set( target, m_deaths, g.deaths );
			ctx.Set( target, m_health, g.health );
			ctx.Emit( m_killed, 0, target );
		}

		if ( const Reloading* r = e.try_get<Reloading>() )
		{
			if ( tick >= r->doneTick )
			{
				e.remove<Reloading>();
				g.ammo = kMagazine;
				ctx.Set( target, m_ammo, g.ammo );
				ctx.Set( target, m_reloading, 0 );
			}
		}

		if ( ctx.Get( netId, m_loadout ) != kPistolSlot )
		{
			return;
		}
		bool reloading = e.has<Reloading>();

		if ( ctx.Pressed( g.slot, m_reload ) && reloading == false && g.ammo < kMagazine )
		{
			StartReload( ctx, e, target );
			return;
		}
		if ( ctx.Pressed( g.slot, m_fire ) == false || reloading || tick < g.nextShotTick )
		{
			return;
		}
		if ( g.ammo <= 0 )
		{
			ctx.Emit( m_dry, target );
			StartReload( ctx, e, target );
			return;
		}

		g.ammo -= 1;
		g.nextShotTick = tick + Ticks( ctx, kFireSeconds );
		ctx.Set( target, m_ammo, g.ammo );
		Fire( ctx, g, netId );
	}

	void StartReload( Context& ctx, flecs::entity e, uint32_t target )
	{
		e.set<Reloading>( { ctx.Tick() + Ticks( ctx, kReloadSeconds ) } );
		ctx.Set( target, m_reloading, 1 );
		ctx.Emit( m_reloadEvent, target );
	}

	void Fire( Context& ctx, Gunner& shooter, uint32_t shooterNetId )
	{
		uint32_t shooterTarget = SlotTarget( shooter.slot );
		b3Vec3 eye = ctx.EyePosition( shooter.slot );
		b3Vec3 dir = ctx.AimDirection( shooter.slot );
		RayHit hit;
		bool found = ctx.CastRay( eye, b3MulSV( kRange, dir ), shooterNetId, hit );
		b3Vec3 end = found ? hit.point : b3MulAdd( eye, kRange, dir );
		ctx.Emit( m_fired, shooterTarget, found ? hit.netId : 0, 0, eye, end );
		if ( found == false )
		{
			return;
		}

		int victimSlot = ctx.SlotOf( hit.netId );
		flecs::entity victim = victimSlot >= 0 ? m_bySlot[victimSlot] : flecs::entity();
		if ( victim.is_valid() && victim.has<Dead>() == false )
		{
			Gunner v = victim.get<Gunner>();
			v.health -= kDamage;
			uint32_t victimTarget = SlotTarget( v.slot );
			ctx.Emit( m_hit, shooterTarget, hit.netId, kDamage, hit.point, hit.normal );
			ctx.Set( victimTarget, m_health, std::max( v.health, 0 ) );
			if ( v.health > 0 )
			{
				victim.set<Gunner>( v );
				return;
			}

			b3Vec3 push = b3Add( b3MulSV( kDeathPush, dir ), b3Vec3{ 0.0f, 1.5f, 0.0f } );
			ctx.Kill( victimTarget, true, hit.point, push, Ticks( ctx, kRagdollSeconds ), kRagdollCap );
			v.health = 0;
			v.deaths += 1;
			shooter.kills += 1;
			victim.set<Gunner>( v );
			victim.remove<Reloading>();
			victim.set<Dead>( { ctx.Tick() + Ticks( ctx, kRespawnSeconds ) } );
			ctx.Set( victimTarget, m_dead, 1 );
			ctx.Set( victimTarget, m_deaths, v.deaths );
			ctx.Set( victimTarget, m_reloading, 0 );
			ctx.Set( shooterTarget, m_kills, shooter.kills );
			ctx.Emit( m_killed, shooterTarget, victimTarget );
			return;
		}

		// Anything loose gets shoved, ragdolls included; walls just spark.
		if ( ctx.IsDynamic( hit.netId ) )
		{
			ctx.Push( hit.netId, hit.point, b3MulSV( kPropPush, dir ), ImpulseVelocity );
		}
		ctx.Emit( m_hit, shooterTarget, hit.netId, 0, hit.point, hit.normal );
	}

	ActionHandle m_fire;
	ActionHandle m_reload;
	FieldHandle m_loadout;
	FieldHandle m_health;
	FieldHandle m_maxHealth;
	FieldHandle m_dead;
	FieldHandle m_kills;
	FieldHandle m_deaths;
	FieldHandle m_ammo;
	FieldHandle m_reloading;
	EventHandle m_fired;
	EventHandle m_hit;
	EventHandle m_reloadEvent;
	EventHandle m_dry;
	EventHandle m_killed;

	flecs::query<Gunner> m_gunners;
	flecs::entity m_bySlot[kMaxPlayers];
	std::vector<flecs::entity> m_scratch;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_pistol()
{
	return std::make_unique<PistolMod>();
}
