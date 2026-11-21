// Gun: the rules of anything that shoots. Which guns there are is not written here.
//
// A gun is an item whose scene says so: a CbItem in this mod's look (client/prefabs/*.tscn) with
// the numbers below among its properties. Adding a gun is adding a scene; nothing is compiled.
//
//   gun.damage           what a hit does (the head doubles it: --mod-option gun.zone.<zone>=<x>)
//   gun.magazine         rounds
//   gun.reload_seconds   how long a reload takes
//   gun.range            metres
//   gun.kill_push, gun.prop_push   how hard a hit shoves the body it kills, and what is loose
//
// Since M282 (step 5: every item a sheet) pulling the trigger is the gun's sheet (its scene's
// CbRules, prefabs/pistol.rules), which every simulation runs, so a shot is predicted, not guessed:
//   Fire     on use, while it has rounds and is not reloading: a round off what the holder sees
//            (change gun.ammo -= 1) and item.use (the flash, the sound, the recoil). Its cooldown is
//            the time between two shots; the item's `use` says whether it fires once a press or
//            while held (an automatic)
//   Dry      on use, empty: item.use_failed (the click) and item.reload; it starts reloading
//   Reload   on R (the action "reload"), not full: item.reload; it starts reloading
// This file is what a sheet cannot say:
//   - the shot: hearing item.use from a gun's holder, it aims along the camera the player had when
//     it fired (the input of the tick before: Context::CastAim's `pressed`), hit-tests posed bodies
//     (hit zones), and says item.reach (a = shooter, b = what it reached, 0: nothing; point = where it
//     came from, vector = where it ended: the tracer) and item.hit (a = shooter, b = what was hit,
//     value = damage done, point, vector = normal); the damage goes out as combat.damage
//   - the rounds: a gun's own field, gun.rounds (its scene declares it with the full count), so a
//     magazine put away half empty comes out half empty in whose hand ever. The language has no
//     field of an item to change (ROADMAP step 6): the sheet takes a round off the holder's
//     gun.ammo, and this file off the gun, and says the gun's count to the holder again (gun.ammo,
//     gun.magazine, gun.reloading: the HUD's, and the sheet's conditions) whenever they differ, so
//     a shot the server refused (reloading, empty) is put right
//   - the reload's end: gun.reload_seconds after it began, the gun is full again (no timer on a
//     carried item either)
//   - out, the body faces where the camera looks and the arm points the gun there (Aim)
// Health and death are the combat mod's; a new life or a new round fills every gun the player
// carries.
//
// Until M282 this file pulled the trigger itself (gun.fire_seconds, gun.automatic), and the look
// guessed the shot (predict use as item.use, change gun.ammo -= 1).

#include "mod_api.h"

#include <algorithm>
#include <map>
#include <string>

namespace
{

using namespace cb;
using namespace cb::mods;

// What a gun's scene says about it.
struct GunKind
{
	int32_t damage = 10;
	int32_t magazine = 10;
	float reloadSeconds = 1.5f;
	float range = 80.0f;
	float killPush = 6.0f;
	float propPush = 4.0f;
	bool roundsInScene = false; // its scene declares "gun.rounds" (a new copy starts full)
};

// One gun in the world.
struct Gun
{
	int32_t ammo = 0;
	uint32_t reloadDoneTick = 0; // 0: not reloading
};

// Whether a player's body is in a shooter's stance.
struct Shooter
{
	bool aiming = false;
};

uint32_t Ticks( const Context& ctx, float seconds )
{
	return uint32_t( seconds * float( ctx.Config().tickRate ) + 0.5f );
}

class GunMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "gun";
	}

	void Declare( Declarations& declare ) override
	{
		declare.Action( "reload", "R" ); // (the sheet's Reload motion presses it)
		m_ammo = declare.Field( "gun.ammo", BoardType::Int );
		m_magazine = declare.Field( "gun.magazine", BoardType::Int );
		m_reloading = declare.Field( "gun.reloading", BoardType::Bool );
		// The item mod's (item.cpp): what a use is called, whatever the item. item.use, item.use_failed
		// and item.reload are the gun's sheet's; item.reach and item.hit this file's.
		m_use = declare.Event( "item.use" );
		declare.Event( "item.use_failed" );
		m_reach = declare.Event( "item.reach" );
		m_hit = declare.Event( "item.hit" );
		m_reloadEvent = declare.Event( "item.reload" );
		// The combat mod's (combat.cpp), and a game-mode mod's round start.
		m_damage = declare.Event( "combat.damage" );
		m_respawned = declare.Event( "combat.respawned" );
		m_roundStart = declare.Event( "game.round_start" );
		m_hand = declare.Socket( "RightHand" );
		// A gun's rounds: its own field (its scene declares it with the full count, below; this is
		// the same field, for a scene that does not).

		// Every item of the look that says gun.damage is a gun. (Its pack and its holster are
		// taken from its scene with it; its slot and "inventory.start" are properties like these.)
		for ( const Declarations::OwnItem& own : declare.ItemsOfLook() )
		{
			auto number = [&]( const char* name, float fallback ) {
				auto found = own.look.properties.find( name );
				return found != own.look.properties.end() ? found->second : fallback;
			};
			if ( own.look.properties.count( "gun.damage" ) == 0 )
			{
				continue;
			}
			GunKind kind;
			kind.damage = int32_t( number( "gun.damage", 10.0f ) );
			kind.magazine = std::max( int32_t( number( "gun.magazine", 10.0f ) ), 1 );
			kind.reloadSeconds = std::max( number( "gun.reload_seconds", 1.5f ), 0.0f );
			kind.range = number( "gun.range", 80.0f );
			kind.killPush = number( "gun.kill_push", 6.0f );
			kind.propPush = number( "gun.prop_push", 4.0f );
			for ( const LookItem::Field& field : own.look.fields )
			{
				kind.roundsInScene |= field.name == "gun.rounds";
			}
			m_kinds[own.kind.index] = kind;
			// (The item mod leaves its cues to this mod.)
			declare.ItemProperty( own.kind, "item.ruled", 1.0f );
		}
		// A gun's rounds: its own field. Its scene declares it, with the full count a new copy
		// starts with; this is the same field, for a gun whose scene does not.
		m_rounds = declare.Field( "gun.rounds", BoardType::Int );
	}

	void Tick( Context& ctx ) override
	{
		uint32_t tick = ctx.Tick();
		// A new life, or a new round for everyone: every gun the player carries is full.
		bool newRound = false;
		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			newRound |= int( e.type ) == m_roundStart.index;
			const int slot = ctx.SlotOf( e.netIdA );
			if ( slot < 0 )
			{
				continue;
			}
			if ( int( e.type ) == m_respawned.index )
			{
				Refill( ctx, PlayerSlot( slot ) );
			}
			// A gun's sheet fired, or began a reload (by R, or on an empty click).
			if ( int( e.type ) == m_use.index )
			{
				Shoot( ctx, PlayerSlot( slot ), tick );
			}
			else if ( int( e.type ) == m_reloadEvent.index )
			{
				Reload( ctx, PlayerSlot( slot ), tick );
			}
		}
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			if ( ctx.Joining( slot ) || ctx.Leaving( slot ) )
			{
				m_shooters[i] = Shooter{};
			}
			if ( newRound && ctx.PlayerNetId( slot ) != 0 )
			{
				Refill( ctx, slot );
			}
			Update( ctx, slot, tick );
		}
		// (A gun that is gone is forgotten: its number may be another's some day.)
		if ( tick % 600 == 0 )
		{
			for ( auto it = m_guns.begin(); it != m_guns.end(); )
			{
				it = ctx.ItemKindOf( it->first ).Valid() ? std::next( it ) : m_guns.erase( it );
			}
		}
	}

private:
	const GunKind* KindOf( Context& ctx, uint32_t item ) const
	{
		auto found = item != 0 ? m_kinds.find( ctx.ItemKindOf( item ).index ) : m_kinds.end();
		return found != m_kinds.end() ? &found->second : nullptr;
	}

	// The gun's own state. Its rounds are the gun's field: read each time (the simulation's), made full
	// the first time it is seen if its scene does not start it so.
	Gun& GunOf( Context& ctx, uint32_t item, const GunKind& kind )
	{
		auto found = m_guns.find( item );
		if ( found == m_guns.end() )
		{
			found = m_guns.emplace( item, Gun{} ).first;
			if ( kind.roundsInScene == false )
			{
				SetRounds( ctx, item, found->second, kind.magazine );
				return found->second;
			}
		}
		found->second.ammo = ctx.Get( item, m_rounds );
		return found->second;
	}

	// Its rounds, now and in the gun's field (which the simulation has from the next tick).
	void SetRounds( Context& ctx, uint32_t item, Gun& gun, int32_t rounds )
	{
		gun.ammo = rounds;
		ctx.Set( item, m_rounds, rounds );
	}

	void Refill( Context& ctx, PlayerSlot slot )
	{
		for ( const CarriedItem& carried : ctx.CarriedItems( slot ) )
		{
			if ( const GunKind* kind = KindOf( ctx, carried.netId ) )
			{
				Gun& gun = GunOf( ctx, carried.netId, *kind );
				SetRounds( ctx, carried.netId, gun, kind->magazine );
				gun.reloadDoneTick = 0;
			}
		}
	}

	// The player's fields say what the gun in its hand has (0 with none), written where the board
	// has something else: the sheet changes gun.ammo and gun.reloading too, and a shot or a reload
	// this file refused is put right.
	void Publish( Context& ctx, PlayerSlot slot, int32_t ammo, int32_t magazine, bool reloading )
	{
		const uint32_t netId = ctx.PlayerNetId( slot );
		const uint32_t target = SlotTarget( slot );
		auto put = [&]( FieldHandle field, int32_t value ) {
			if ( ctx.Get( netId, field ) != value )
			{
				ctx.Set( target, field, value );
			}
		};
		put( m_ammo, ammo );
		put( m_magazine, magazine );
		put( m_reloading, reloading ? 1 : 0 );
	}

	// The gun in the player's hand, and its kind (null: none, or the player is dead).
	const GunKind* Holding( Context& ctx, PlayerSlot slot, uint32_t& item ) const
	{
		const Character* c = ctx.PlayerCharacter( slot );
		item = c != nullptr && c->dead == 0 ? ctx.HeldItem( slot, m_hand ) : 0;
		return KindOf( ctx, item );
	}

	// The sheet fired (item.use): a round off the gun, and the shot, unless it is reloading or empty
	// (then the board is put right: Publish).
	void Shoot( Context& ctx, PlayerSlot slot, uint32_t tick )
	{
		uint32_t item = 0;
		const GunKind* kind = Holding( ctx, slot, item );
		if ( kind == nullptr )
		{
			return; // (another item's use)
		}
		Gun& gun = GunOf( ctx, item, *kind );
		if ( gun.reloadDoneTick != 0 && tick < gun.reloadDoneTick )
		{
			return;
		}
		if ( gun.ammo <= 0 )
		{
			return;
		}
		SetRounds( ctx, item, gun, gun.ammo - 1 );
		Fire( ctx, slot, *kind );
	}

	// The sheet began a reload (item.reload: by R, or an empty click): unless it is full or reloading.
	void Reload( Context& ctx, PlayerSlot slot, uint32_t tick )
	{
		uint32_t item = 0;
		const GunKind* kind = Holding( ctx, slot, item );
		if ( kind == nullptr )
		{
			return;
		}
		Gun& gun = GunOf( ctx, item, *kind );
		if ( gun.reloadDoneTick == 0 && gun.ammo < kind->magazine )
		{
			gun.reloadDoneTick = tick + std::max( Ticks( ctx, kind->reloadSeconds ), 1u );
		}
	}

	void Update( Context& ctx, PlayerSlot slot, uint32_t tick )
	{
		uint32_t netId = ctx.PlayerNetId( slot );
		const Character* c = ctx.PlayerCharacter( slot );
		if ( netId == 0 || c == nullptr )
		{
			return; // joining this tick: the player exists from the next one
		}
		Shooter& s = m_shooters[size_t( slot )];
		uint32_t target = SlotTarget( slot );
		// What is in the right hand decides: a gun there fires, wherever it came from. Who has one,
		// and when it is out, is the inventory mod's.
		uint32_t inHand = ctx.HeldItem( slot, m_hand );
		const GunKind* kind = KindOf( ctx, inHand );
		bool holding = kind != nullptr && c->dead == 0;

		// A gun out means a shooter's stance: the body faces where the camera looks, the upper body
		// holds the gun and the arm points it there, in the pose everyone draws and hit tests use.
		// (What the body says decides, so it is set again where another mod took the layer.)
		const AnimStateCopy anim = ctx.PlayerAnim( slot );
		// (A new life's animation state has no aim: it is set again.)
		bool lost = holding && s.aiming && anim != nullptr && anim->aiming == 0;
		if ( holding != s.aiming || lost )
		{
			s.aiming = holding;
			ctx.Aim( target, holding );
			if ( holding )
			{
				ctx.FaceCamera( target, true );
			}
		}
		if ( holding == false )
		{
			// (A reload does not go on in a holster, or in a dead hand: it starts again.)
			Publish( ctx, slot, 0, 0, false );
			return;
		}
		Gun& gun = GunOf( ctx, inHand, *kind );
		if ( gun.reloadDoneTick != 0 && tick >= gun.reloadDoneTick )
		{
			gun.reloadDoneTick = 0;
			SetRounds( ctx, inHand, gun, kind->magazine );
		}
		Publish( ctx, slot, gun.ammo, kind->magazine, gun.reloadDoneTick != 0 );
	}

	bool IsLivingPlayer( Context& ctx, uint32_t netId ) const
	{
		// A player, or a body a mod drives (an NPC: the combat mod hurts it too).
		int slot = ctx.SlotOf( netId );
		const Character* c = slot >= 0 ? ctx.PlayerCharacter( PlayerSlot( slot ) )
							 : ctx.DrivenByMod( netId ) ? ctx.BodyCharacter( netId ) : nullptr;
		return c != nullptr && c->dead == 0;
	}

	void Fire( Context& ctx, PlayerSlot slot, const GunKind& kind )
	{
		uint32_t shooter = SlotTarget( slot );
		// At what is under the crosshair, from the eye: whatever camera the player looks through.
		b3Vec3 eye, dir;
		RayHit hit;
		bool found = ctx.CastAim( slot, kind.range, hit, eye, dir, true );
		b3Vec3 end = found ? hit.point : b3MulAdd( eye, kind.range, dir );
		ctx.Emit( m_reach, shooter, found ? hit.netId : 0, 0, eye, end );
		if ( found == false )
		{
			return;
		}
		if ( IsLivingPlayer( ctx, hit.netId ) )
		{
			// Where it hit scales the damage: --mod-option gun.zone.<zone>=<multiplier>, for any
			// zone the server's character defines (head x2 unless told otherwise).
			int32_t damage = kind.damage;
			if ( hit.zone != nullptr )
			{
				std::string zone = hit.zone;
				double multiplier = ctx.Option( "gun.zone." + zone, zone == "head" ? 2.0 : 1.0 );
				damage = std::max( int32_t( double( kind.damage ) * multiplier + 0.5 ), 0 );
			}
			// What the gun did (its look's: the puff, the hit marker), and what it means for the
			// one it hit, which is the combat mod's to decide.
			ctx.Emit( m_hit, shooter, hit.netId, damage, hit.point, hit.normal );
			b3Vec3 push = b3Add( b3MulSV( kind.killPush, dir ), b3Vec3{ 0.0f, 1.5f, 0.0f } );
			ctx.Emit( m_damage, shooter, hit.netId, damage, hit.point, push );
			return;
		}
		// Anything loose gets shoved, ragdolls included; walls just spark.
		if ( ctx.IsDynamic( hit.netId ) )
		{
			ctx.Push( hit.netId, hit.point, b3MulSV( kind.propPush, dir ), ImpulseVelocity );
		}
		ctx.Emit( m_hit, shooter, hit.netId, 0, hit.point, hit.normal );
	}

	std::map<int, GunKind> m_kinds; // by item kind
	std::map<uint32_t, Gun> m_guns; // by the item's NetId
	Shooter m_shooters[kMaxPlayers];
	FieldHandle m_ammo;
	FieldHandle m_magazine;
	FieldHandle m_reloading;
	FieldHandle m_rounds;
	EventHandle m_use;
	EventHandle m_reach;
	EventHandle m_hit;
	EventHandle m_reloadEvent;
	EventHandle m_damage;
	EventHandle m_respawned;
	EventHandle m_roundStart;
	SocketHandle m_hand;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_gun()
{
	return std::make_unique<GunMod>();
}
