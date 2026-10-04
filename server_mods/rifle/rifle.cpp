// Rifle: automatic hitscan fire. Hold the trigger and it keeps shooting.
//
// The pistol's sibling (see pistol.cpp), and what differs is the trigger: the pistol fires on a
// press, the rifle for as long as "fire" is held, one shot every kFireSeconds. Smaller damage, a
// bigger magazine, a longer reload, carried on the back.
//
// Health and death are the combat mod's: a shot that hits a player goes out as "combat.damage",
// and "combat.respawned" (a new life) refills the magazine. Without the combat mod the rifle still
// fires and reports its hits; nobody is hurt.
//
// Every rule is on the server. Clients see board fields ("rifle.ammo", "rifle.reloading") and
// events ("rifle.fired", "rifle.hit"), and the look decides what to draw and play for each. The
// look predicts held fire too (CbPrediction.while_held, at this file's rate).
//
// While it is out the upper body has the "rifle" stance. What that looks like is the character's:
// one with rifle animations has states of its own for it (entered on "rifle", kicking on
// "rifle.fired"); one without holds it as it holds a pistol ("pistol or rifle").

#include "mod_api.h"

#include <algorithm>
#include <string>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr int32_t kDamage = 14;
constexpr int32_t kMagazine = 30;
constexpr float kFireSeconds = 0.1f; // ten shots a second; the look's prediction repeats at the same rate
constexpr float kReloadSeconds = 2.0f;
constexpr float kRange = 120.0f;
// How hard a hit shoves: the body it kills, and anything loose that is hit.
constexpr float kDeathPush = 5.0f;
constexpr float kPropPush = 2.5f;

struct Rifleman
{
	PlayerSlot slot = 0;
	int32_t ammo = kMagazine;
	uint32_t nextShotTick = 0;
	bool aiming = false;  // what the last Aim command said
	bool wentDry = false; // the trigger is held on an empty magazine: one click, not ten a second
};

struct RifleReload
{
	uint32_t doneTick = 0;
};

uint32_t Ticks( const Context& ctx, float seconds )
{
	return uint32_t( seconds * float( ctx.Config().tickRate ) + 0.5f );
}

class RifleMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "rifle";
	}

	void Declare( Declarations& declare ) override
	{
		// Shared with the pistol and the bat: whichever is out answers.
		m_fire = declare.Action( "fire", "MouseLeft" );
		m_reload = declare.Action( "reload", "R" );

		m_ammo = declare.Field( "rifle.ammo", BoardType::Int );
		m_reloading = declare.Field( "rifle.reloading", BoardType::Bool );

		// a = shooter, b = what the ray hit (0: nothing), point = where the shot came from,
		// vector = where it ended.
		m_fired = declare.Event( "rifle.fired" );
		// a = shooter, b = what was hit, value = damage done, point = where, vector = surface normal.
		m_hit = declare.Event( "rifle.hit" );
		m_reloadEvent = declare.Event( "rifle.reload" );
		m_dry = declare.Event( "rifle.dry" );

		// The combat mod's (see combat.cpp), and a game-mode mod's round start.
		m_damage = declare.Event( "combat.damage" );
		m_respawned = declare.Event( "combat.respawned" );
		m_roundStart = declare.Event( "game.round_start" );

		m_upper = declare.Layer( "upper" );
		m_stance = declare.Stance( "rifle" );
		// Its body when it lies in the world is authored in its scene (client/prefabs/rifle.tscn, the
		// CbItemBody) and baked to client/items/rifle.gun.cfg.
		m_gun = declare.ItemKind( "rifle.gun" );
		m_hand = declare.Socket( "RightHand" );
		// For the inventory mod: slot 4, one for every life, on the back while it is put away.
		declare.ItemProperty( m_gun, "inventory.slot", 4.0f );
		declare.ItemProperty( m_gun, "inventory.start", 1.0f );
		declare.ItemProperty( m_gun, "inventory.holster", declare.Socket( "Back" ) );
	}

	void Start( Context& ctx ) override
	{
		flecs::world& world = ctx.World();
		world.component<Rifleman>();
		world.component<RifleReload>();
		m_riflemen = world.query<Rifleman>();
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
				Rifleman g;
				g.slot = slot;
				m_bySlot[i] = world.entity().set<Rifleman>( g );
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
		m_riflemen.each( [&]( flecs::entity e, Rifleman& ) { m_scratch.push_back( e ); } );
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
	void Publish( Context& ctx, const Rifleman& g )
	{
		uint32_t target = SlotTarget( g.slot );
		ctx.Set( target, m_ammo, g.ammo );
		ctx.Set( target, m_reloading, 0 );
	}

	void Refill( Context& ctx, flecs::entity e )
	{
		Rifleman g = e.get<Rifleman>();
		g.ammo = kMagazine;
		e.remove<RifleReload>();
		e.set<Rifleman>( g );
		Publish( ctx, g );
	}

	// Works on a copy that is written back at the end: adding or removing RifleReload moves the
	// entity between tables, which would leave a reference into the old one dangling.
	void Update( Context& ctx, flecs::entity e )
	{
		Rifleman g = e.get<Rifleman>();
		UpdateRifleman( ctx, e, g );
		if ( e.is_alive() )
		{
			e.set<Rifleman>( g );
		}
	}

	void UpdateRifleman( Context& ctx, flecs::entity e, Rifleman& g )
	{
		uint32_t netId = ctx.PlayerNetId( g.slot );
		const Character* c = ctx.PlayerCharacter( g.slot );
		if ( netId == 0 || c == nullptr )
		{
			return; // joining this tick: the player exists from the next one
		}
		uint32_t target = SlotTarget( g.slot );
		uint32_t tick = ctx.Tick();

		// What is in the right hand decides: a "rifle.gun" there fires, wherever it came from. Who
		// has one, and when it is out, is the inventory mod's.
		uint32_t inHand = ctx.HeldItem( g.slot, m_hand );
		bool gunInHand = inHand != 0 && ctx.ItemKindOf( inHand ).index == m_gun.index;

		if ( const RifleReload* r = e.try_get<RifleReload>() )
		{
			if ( c->dead != 0 )
			{
				// Dying drops the reload; the next life starts with a full magazine anyway.
				e.remove<RifleReload>();
				ctx.Set( target, m_reloading, 0 );
			}
			else if ( tick >= r->doneTick )
			{
				e.remove<RifleReload>();
				g.ammo = kMagazine;
				ctx.Set( target, m_ammo, g.ammo );
				ctx.Set( target, m_reloading, 0 );
			}
		}

		// The rifle out means a shooter's stance: the body faces where the camera looks and the arms
		// point the rifle there, in the pose everyone draws and hit tests use. Put away, the rifle
		// clears only what is its own (the loadout decides facing).
		bool holding = gunInHand && c->dead == 0;
		// The layer is shared with the other gun: swapped for it in one tick, that one's "put away"
		// may land after this one's "out". What the body says decides, so it is set again.
		const AnimState* anim = ctx.PlayerAnim( g.slot );
		bool lost = holding && g.aiming && anim != nullptr && m_upper.Valid() &&
					( anim->aiming == 0 || int( anim->stances[m_upper.index] ) != m_stance.index + 1 );
		if ( holding != g.aiming || lost )
		{
			g.aiming = holding;
			ctx.Aim( target, holding );
			ctx.SetStance( target, m_upper, holding ? m_stance : StanceHandle{} );
			if ( holding )
			{
				ctx.FaceCamera( target, true );
			}
		}

		bool trigger = ctx.Held( g.slot, m_fire );
		if ( trigger == false )
		{
			g.wentDry = false;
		}
		if ( holding == false || c->frozen )
		{
			return;
		}
		bool reloading = e.has<RifleReload>();

		if ( ctx.Pressed( g.slot, m_reload ) && reloading == false && g.ammo < kMagazine )
		{
			StartReload( ctx, e, target );
			return;
		}
		// Automatic: the trigger held is a shot whenever the rifle is ready for the next one.
		if ( trigger == false || reloading || tick < g.nextShotTick )
		{
			return;
		}
		if ( g.ammo <= 0 )
		{
			// Empty: one click and a reload for each pull of the trigger, however long it is held.
			if ( g.wentDry == false )
			{
				g.wentDry = true;
				ctx.Emit( m_dry, target );
				StartReload( ctx, e, target );
			}
			return;
		}

		g.ammo -= 1;
		g.wentDry = false;
		g.nextShotTick = tick + Ticks( ctx, kFireSeconds );
		ctx.Set( target, m_ammo, g.ammo );
		Fire( ctx, g );
	}

	void StartReload( Context& ctx, flecs::entity e, uint32_t target )
	{
		e.set<RifleReload>( { ctx.Tick() + Ticks( ctx, kReloadSeconds ) } );
		ctx.Set( target, m_reloading, 1 );
		ctx.Emit( m_reloadEvent, target );
	}

	bool IsLivingPlayer( Context& ctx, uint32_t netId ) const
	{
		int slot = ctx.SlotOf( netId );
		const Character* c = slot >= 0 ? ctx.PlayerCharacter( PlayerSlot( slot ) ) : nullptr;
		return c != nullptr && c->dead == 0;
	}

	void Fire( Context& ctx, const Rifleman& shooter )
	{
		uint32_t shooterTarget = SlotTarget( shooter.slot );
		// At what is under the crosshair, from the eye: whatever camera the player looks through.
		b3Vec3 eye, dir;
		RayHit hit;
		bool found = ctx.CastAim( shooter.slot, kRange, hit, eye, dir );
		b3Vec3 end = found ? hit.point : b3MulAdd( eye, kRange, dir );
		ctx.Emit( m_fired, shooterTarget, found ? hit.netId : 0, 0, eye, end );
		if ( found == false )
		{
			return;
		}

		if ( IsLivingPlayer( ctx, hit.netId ) )
		{
			// Where it hit scales the damage: --mod-option rifle.zone.<zone>=<multiplier>, for any
			// zone the server's character defines (head x2 unless told otherwise).
			int32_t damage = kDamage;
			if ( hit.zone != nullptr )
			{
				std::string zone = hit.zone;
				double multiplier = ctx.Option( "rifle.zone." + zone, zone == "head" ? 2.0 : 1.0 );
				damage = std::max( int32_t( double( kDamage ) * multiplier + 0.5 ), 0 );
			}
			// What the rifle did (its own look: the puff, the hit marker), and what it means for the
			// one it hit, which is the combat mod's to decide.
			ctx.Emit( m_hit, shooterTarget, hit.netId, damage, hit.point, hit.normal );
			b3Vec3 push = b3Add( b3MulSV( kDeathPush, dir ), b3Vec3{ 0.0f, 1.2f, 0.0f } );
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
	FieldHandle m_ammo;
	FieldHandle m_reloading;
	EventHandle m_fired;
	EventHandle m_hit;
	EventHandle m_reloadEvent;
	EventHandle m_dry;
	EventHandle m_damage;
	EventHandle m_respawned;
	EventHandle m_roundStart;
	LayerHandle m_upper;
	StanceHandle m_stance;
	ItemKindHandle m_gun;
	SocketHandle m_hand;

	flecs::query<Rifleman> m_riflemen;
	flecs::entity m_bySlot[kMaxPlayers];
	std::vector<flecs::entity> m_scratch;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_rifle()
{
	return std::make_unique<RifleMod>();
}
