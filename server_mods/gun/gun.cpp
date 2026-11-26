// Gun: the rules of anything that shoots. Which guns there are is not written here.
//
// A gun is an item whose scene says so: a CbItem in this mod's look (client/prefabs/*.tscn) with
// the numbers below among its properties. Adding a gun is adding a scene; nothing is compiled.
//
//   gun.damage           what a hit does (the head doubles it: --mod-option gun.zone.<zone>=<x>)
//   gun.capacity         rounds (its sheet's param: the fields gun.rounds and gun.magazine start at it)
//   gun.reload_seconds   how long a reload takes
//   gun.range            metres
//   gun.kill_push, gun.prop_push   how hard a hit shoves the body it kills, and what is loose
//
// Since M287 a gun's rounds and its reload are its sheet's (prefabs/pistol.cbsheet), which every
// simulation runs, so a shot, a click and a reload are predicted, not guessed:
//   field gun.rounds int on item = gun.capacity, gun.magazine, gun.reloading, gun.reload_left (a timer): the
//   gun's own fields, so a magazine put away half empty comes out half empty in whose hand ever
//   Fire     on use, while it has rounds and is not reloading: a round off the gun, and item.use
//   Dry      on use, empty: item.use_failed (the click), item.reload, and the reload begins
//   Reload   on R, not full: item.reload, and the reload begins
//   Reloaded when its reload_left has run out: full again
// This file is what a sheet cannot say:
//   - the shot: hearing item.use from a gun's holder, it aims along the camera the player had when
//     it fired (the input of the tick before: Context::CastAim's `pressed`), hit-tests posed bodies
//     (hit zones), and says item.reach (a = shooter, b = what it reached, 0: nothing; point = where it
//     came from, vector = where it ended: the tracer) and item.hit (a = shooter, b = what was hit,
//     value = damage done, point, vector = normal); the damage goes out as combat.damage
//   - out, the body faces where the camera looks and the arm points the gun there (Aim)
//   - a new life or a new round fills every gun the player carries
// Health and death are the combat mod's.
//
// Until M282 this file pulled the trigger itself (gun.fire_seconds, gun.automatic), and the look
// guessed the shot; until M287 it kept the rounds and the reload's end, and told the holder's fields.

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
	float range = 80.0f;
	float killPush = 6.0f;
	float propPush = 4.0f;
};

// Whether a player's body is in a shooter's stance.
struct Shooter
{
	bool aiming = false;
};

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
		// The item mod's (item.cpp): what a use is called, whatever the item. item.use, item.use_failed
		// and item.reload are the gun's sheet's; item.reach and item.hit this file's.
		m_use = declare.Event( "item.use" );
		declare.Event( "item.use_failed" );
		declare.Event( "item.reload" );
		m_reach = declare.Event( "item.reach" );
		m_hit = declare.Event( "item.hit" );
		// The combat mod's (combat.cpp), and a game-mode mod's round start.
		m_damage = declare.Event( "combat.damage" );
		m_respawned = declare.Event( "combat.respawned" );
		m_roundStart = declare.Event( "game.round_start" );
		m_hand = declare.Socket( "RightHand" );

		// Every item of the look that says gun.damage is a gun. (Its pack, its holster, its own
		// fields are taken from its scene and its sheet with it.)
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
			kind.magazine = std::max( int32_t( number( "gun.capacity", 10.0f ) ), 1 ); // (its sheet's param, M296)
			kind.range = number( "gun.range", 80.0f );
			kind.killPush = number( "gun.kill_push", 6.0f );
			kind.propPush = number( "gun.prop_push", 4.0f );
			m_kinds[own.kind.index] = kind;
			// (The item mod leaves its cues to the gun's sheet, which has motions: M296.)
		}
		// A gun's own fields (its sheet declares them; the same fields, for a refill).
		m_rounds = declare.Field( "gun.rounds", BoardType::Int );
		m_magazine = declare.Field( "gun.magazine", BoardType::Int );
		m_reloading = declare.Field( "gun.reloading", BoardType::Bool );
		m_reloadLeft = declare.Field( "gun.reload_left", BoardType::Float );
	}

	void Tick( Context& ctx ) override
	{
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
			// A gun's sheet fired (its own simulation's: a round is off the gun already).
			if ( int( e.type ) == m_use.index )
			{
				Shoot( ctx, PlayerSlot( slot ) );
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
			Update( ctx, slot );
		}
	}

private:
	const GunKind* KindOf( Context& ctx, uint32_t item ) const
	{
		auto found = item != 0 ? m_kinds.find( ctx.ItemKindOf( item ).index ) : m_kinds.end();
		return found != m_kinds.end() ? &found->second : nullptr;
	}

	// Full again, not reloading: its fields (the sheet's), as a new life or a round has them.
	void Refill( Context& ctx, PlayerSlot slot )
	{
		for ( const CarriedItem& carried : ctx.CarriedItems( slot ) )
		{
			if ( const GunKind* kind = KindOf( ctx, carried.netId ) )
			{
				const int32_t magazine = ctx.Get( carried.netId, m_magazine );
				ctx.Set( carried.netId, m_rounds, magazine > 0 ? magazine : kind->magazine );
				ctx.Set( carried.netId, m_reloading, 0 );
				ctx.SetFloat( carried.netId, m_reloadLeft, 0.0f );
			}
		}
	}

	// The gun in the player's hand, and its kind (null: none, or the player is dead).
	const GunKind* Holding( Context& ctx, PlayerSlot slot, uint32_t& item ) const
	{
		const Character* c = ctx.PlayerCharacter( slot );
		item = c != nullptr && c->dead == 0 ? ctx.HeldItem( slot, m_hand ) : 0;
		return KindOf( ctx, item );
	}

	// The sheet fired (item.use): the shot, from the gun in the hand.
	void Shoot( Context& ctx, PlayerSlot slot )
	{
		uint32_t item = 0;
		if ( const GunKind* kind = Holding( ctx, slot, item ) )
		{
			Fire( ctx, slot, *kind );
		}
	}

	void Update( Context& ctx, PlayerSlot slot )
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
	Shooter m_shooters[kMaxPlayers];
	FieldHandle m_rounds;
	FieldHandle m_magazine;
	FieldHandle m_reloading;
	FieldHandle m_reloadLeft;
	EventHandle m_use;
	EventHandle m_reach;
	EventHandle m_hit;
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
