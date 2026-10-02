// Pistol: hitscan shooting with a magazine, a reload, and a ray that marks.
//
// Only the gun lives here. Health and death are the combat mod's: a shot that hits a player goes
// out as "combat.damage", like any other weapon's, and "combat.respawned" (a new life) refills the
// magazine. Without the combat mod the pistol still fires and reports its hits; nobody is hurt.
//
// Every rule is on the server. Clients never learn what a pistol is: they see board fields
// ("pistol.ammo", "pistol.reloading") and events ("pistol.fired", "pistol.hit"), and the look
// decides what to draw and play for each.
//
// State is kept in the mods' shared flecs world: one entity per player with a Gunner, plus
// Reloading while that lasts. Nothing here is rolled back or sent anywhere; what clients need is
// published through the board.

#include "mod_api.h"

#include <algorithm>
#include <string>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr int32_t kDamage = 25;
constexpr int32_t kMagazine = 12;
constexpr float kFireSeconds = 0.2f;
constexpr float kReloadSeconds = 1.5f;
constexpr float kMarkSeconds = 1.0f; // between two marks
constexpr float kRange = 80.0f;
// How hard a hit shoves: the body it kills, and anything loose that is hit.
constexpr float kDeathPush = 6.0f;
constexpr float kPropPush = 4.0f;

struct Gunner
{
	PlayerSlot slot = 0;
	int32_t ammo = kMagazine;
	uint32_t nextShotTick = 0;
	uint32_t nextMarkTick = 0;
	bool aiming = false; // what the last Aim command said
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
		// A second use of the pistol, and the example of adding one: a ray that harms nothing and
		// marks the player it finds (see Mark below, and client/vfx/reactions_pistol.tscn).
		m_mark = declare.Action( "mark", "MouseRight" );

		m_ammo = declare.Field( "pistol.ammo", BoardType::Int );
		m_reloading = declare.Field( "pistol.reloading", BoardType::Bool );

		// a = shooter, b = what the ray hit (0: nothing), point = where the shot came from,
		// vector = where it ended.
		m_fired = declare.Event( "pistol.fired" );
		// a = shooter, b = what was hit, value = damage done, point = where, vector = surface normal.
		m_hit = declare.Event( "pistol.hit" );
		m_reloadEvent = declare.Event( "pistol.reload" );
		m_dry = declare.Event( "pistol.dry" );
		// A mark was cast: a = who cast it, b = what the ray hit (0: nothing), point = where it came
		// from, vector = where it ended.
		m_scan = declare.Event( "pistol.scan" );
		// ... and it found a living player: a = who cast it, b = the marked player, point = where.
		m_marked = declare.Event( "pistol.marked" );

		// The combat mod's (see combat.cpp): what a hit on a player is said with, and the news of a
		// new life, which comes with a full magazine. A game-mode mod's round start does too.
		m_damage = declare.Event( "combat.damage" );
		m_respawned = declare.Event( "combat.respawned" );
		m_roundStart = declare.Event( "game.round_start" );

		m_upper = declare.Layer( "upper" );
		m_stance = declare.Stance( "pistol" );
		// Its body when it lies in the world is authored in its scene (client/prefabs/pistol.tscn, the
		// CbItemBody) and baked to client/items/pistol.gun.cfg.
		m_gun = declare.ItemKind( "pistol.gun" );
		m_hand = declare.Socket( "RightHand" );
		// For the inventory mod: slot 2, one for every life, on the hip while it is put away.
		declare.ItemProperty( m_gun, "inventory.slot", 2.0f );
		declare.ItemProperty( m_gun, "inventory.start", 1.0f );
		declare.ItemProperty( m_gun, "inventory.holster", declare.Socket( "Hip" ) );
	}

	void Start( Context& ctx ) override
	{
		flecs::world& world = ctx.World();
		world.component<Gunner>();
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
				Publish( ctx, g );
			}
		}

		// A new life, or a new round for everyone: a full magazine.
		bool newRound = false;
		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			newRound |= int( e.type ) == m_roundStart.index;
			if ( int( e.type ) == m_respawned.index )
			{
				int slot = ctx.SlotOf( e.netIdA );
				if ( slot >= 0 && m_bySlot[slot].is_valid() )
				{
					Refill( ctx, m_bySlot[slot] );
				}
			}
		}

		// Collected first: reloading adds and removes a component.
		m_scratch.clear();
		m_gunners.each( [&]( flecs::entity e, Gunner& ) { m_scratch.push_back( e ); } );
		for ( flecs::entity e : m_scratch )
		{
			if ( e.is_alive() && newRound )
			{
				Refill( ctx, e );
			}
			if ( e.is_alive() )
			{
				Update( ctx, e );
			}
		}
	}

private:
	void Publish( Context& ctx, const Gunner& g )
	{
		uint32_t target = SlotTarget( g.slot );
		ctx.Set( target, m_ammo, g.ammo );
		ctx.Set( target, m_reloading, 0 );
	}

	void Refill( Context& ctx, flecs::entity e )
	{
		Gunner g = e.get<Gunner>();
		g.ammo = kMagazine;
		e.remove<Reloading>();
		e.set<Gunner>( g );
		Publish( ctx, g );
	}

	// Works on a copy that is written back at the end: adding or removing Reloading moves the
	// entity between tables, which would leave a reference into the old one dangling.
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

		// What is in the right hand decides: a "pistol.gun" there fires, wherever it came from. Who
		// has one, and when it is out, is the inventory mod's.
		uint32_t inHand = ctx.HeldItem( g.slot, m_hand );
		bool gunInHand = inHand != 0 && ctx.ItemKindOf( inHand ).index == m_gun.index;

		if ( const Reloading* r = e.try_get<Reloading>() )
		{
			if ( c->dead != 0 )
			{
				// Dying drops the reload; the next life starts with a full magazine anyway.
				e.remove<Reloading>();
				ctx.Set( target, m_reloading, 0 );
			}
			else if ( tick >= r->doneTick )
			{
				e.remove<Reloading>();
				g.ammo = kMagazine;
				ctx.Set( target, m_ammo, g.ammo );
				ctx.Set( target, m_reloading, 0 );
			}
		}

		// The pistol out means a shooter's stance: the body faces where the camera looks, the upper
		// body holds the pistol and the arm points it there, in the pose everyone draws and hit
		// tests use. Put away, the pistol clears only what is its own (the loadout decides facing).
		bool holding = gunInHand && c->dead == 0;
		if ( holding != g.aiming )
		{
			g.aiming = holding;
			ctx.Aim( target, holding );
			ctx.SetStance( target, m_upper, holding ? m_stance : StanceHandle{} );
			if ( holding )
			{
				ctx.FaceCamera( target, true );
			}
		}

		if ( holding == false || c->frozen )
		{
			return;
		}
		bool reloading = e.has<Reloading>();

		if ( ctx.Pressed( g.slot, m_reload ) && reloading == false && g.ammo < kMagazine )
		{
			StartReload( ctx, e, target );
			return;
		}
		if ( ctx.Pressed( g.slot, m_mark ) && reloading == false && tick >= g.nextMarkTick )
		{
			g.nextMarkTick = tick + Ticks( ctx, kMarkSeconds );
			Mark( ctx, g, netId );
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

	// A living player: what a shot hurts and a mark finds.
	bool IsLivingPlayer( Context& ctx, uint32_t netId ) const
	{
		int slot = ctx.SlotOf( netId );
		const Character* c = slot >= 0 ? ctx.PlayerCharacter( PlayerSlot( slot ) ) : nullptr;
		return c != nullptr && c->dead == 0;
	}

	// The same ray as a shot, with no damage: it says where it went (pistol.scan) and, when it found
	// a living player, who (pistol.marked). What a mark looks like and how long it shows is the
	// look's business; the server keeps nothing about it.
	void Mark( Context& ctx, const Gunner& caster, uint32_t casterNetId )
	{
		uint32_t casterTarget = SlotTarget( caster.slot );
		b3Vec3 eye = ctx.EyePosition( caster.slot );
		b3Vec3 dir = ctx.AimDirection( caster.slot );
		RayHit hit;
		bool found = ctx.CastRay( eye, b3MulSV( kRange, dir ), casterNetId, hit );
		b3Vec3 end = found ? hit.point : b3MulAdd( eye, kRange, dir );
		ctx.Emit( m_scan, casterTarget, found ? hit.netId : 0, 0, eye, end );
		if ( found && IsLivingPlayer( ctx, hit.netId ) )
		{
			ctx.Emit( m_marked, casterTarget, hit.netId, 0, hit.point, hit.normal );
		}
	}

	void Fire( Context& ctx, const Gunner& shooter, uint32_t shooterNetId )
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

		if ( IsLivingPlayer( ctx, hit.netId ) )
		{
			// Where it hit scales the damage: --mod-option pistol.zone.<zone>=<multiplier>, for any
			// zone the server's character defines (head x2 unless told otherwise).
			int32_t damage = kDamage;
			if ( hit.zone != nullptr )
			{
				std::string zone = hit.zone;
				double multiplier = ctx.Option( "pistol.zone." + zone, zone == "head" ? 2.0 : 1.0 );
				damage = std::max( int32_t( double( kDamage ) * multiplier + 0.5 ), 0 );
			}
			// What the pistol did (its own look: the puff, the hit marker), and what it means for
			// the one it hit, which is the combat mod's to decide.
			ctx.Emit( m_hit, shooterTarget, hit.netId, damage, hit.point, hit.normal );
			b3Vec3 push = b3Add( b3MulSV( kDeathPush, dir ), b3Vec3{ 0.0f, 1.5f, 0.0f } );
			ctx.Emit( m_damage, shooterTarget, hit.netId, damage, hit.point, push );
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
	ActionHandle m_mark;
	FieldHandle m_ammo;
	FieldHandle m_reloading;
	EventHandle m_fired;
	EventHandle m_hit;
	EventHandle m_reloadEvent;
	EventHandle m_dry;
	EventHandle m_scan;
	EventHandle m_marked;
	EventHandle m_damage;
	EventHandle m_respawned;
	EventHandle m_roundStart;
	LayerHandle m_upper;
	StanceHandle m_stance;
	ItemKindHandle m_gun;
	SocketHandle m_hand;

	flecs::query<Gunner> m_gunners;
	flecs::entity m_bySlot[kMaxPlayers];
	std::vector<flecs::entity> m_scratch;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_pistol()
{
	return std::make_unique<PistolMod>();
}
