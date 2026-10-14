// Gun: the rules of anything that shoots. Which guns there are is not written here.
//
// A gun is an item whose scene says so: a CbItem in this mod's look (client/prefabs/*.tscn) with
// the numbers below among its properties. Adding a gun is adding a scene; nothing is compiled.
//
//   gun.damage           what a hit does (the head doubles it: --mod-option gun.zone.<zone>=<x>)
//   gun.magazine         rounds
//   gun.fire_seconds     between two shots
//   gun.automatic        1: it fires for as long as the use button is held; 0: once a press
//   gun.reload_seconds   how long a reload takes
//   gun.range            metres
//   gun.kill_push, gun.prop_push   how hard a hit shoves the body it kills, and what is loose
//
// The scene also says what the gun is like to hold (its animation pack), where it hangs (its
// holster), its slot, and what a shot looks and sounds like: its CbReaction nodes on the item
// mod's cues, which do not name the gun (see item.cpp):
//
//   item.use          a shot: a = shooter, b = what the ray hit (0: nothing), point = where it
//                     came from, vector = where it ended
//   item.use_failed   the trigger on an empty magazine
//   item.hit          a = shooter, b = what was hit, value = damage done, point, vector = normal
//   item.reload       a reload began
//
// A magazine is the gun's own: put away half empty, it is half empty when it comes out, in whose
// hand ever. Its rounds are the gun's own field, "gun.rounds", which the gun's scene declares with
// the full count (CbItem.fields): simulation state, in every copy, read by looks as
// "hand.gun.rounds". What the holder's HUD and the look's predictions read is also on the player:
// the gun in the hand's "gun.ammo", "gun.magazine" and "gun.reloading" (all 0 with no gun out).
//
// Health and death are the combat mod's: a hit on a player goes out as "combat.damage", and a new
// life or a new round fills every gun the player carries.

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
	float fireSeconds = 0.2f;
	bool automatic = false;
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
	uint32_t nextShotTick = 0;
	uint32_t reloadDoneTick = 0; // 0: not reloading
	bool wentDry = false;		 // the trigger is held on an empty magazine: one click, not ten a second
};

// What each player was last told, and whether its body is in a shooter's stance.
struct Shooter
{
	bool aiming = false;
	int32_t ammo = -1;
	int32_t magazine = -1;
	int32_t reloading = -1;
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
		m_reload = declare.Action( "reload", "R" );
		m_ammo = declare.Field( "gun.ammo", BoardType::Int );
		m_magazine = declare.Field( "gun.magazine", BoardType::Int );
		m_reloading = declare.Field( "gun.reloading", BoardType::Bool );
		// The item mod's (item.cpp): what a use is called, whatever the item.
		m_use = declare.Event( "item.use" );
		m_useFailed = declare.Event( "item.use_failed" );
		m_hit = declare.Event( "item.hit" );
		m_reloadEvent = declare.Event( "item.reload" );
		// The combat mod's (combat.cpp), and a game-mode mod's round start.
		m_damage = declare.Event( "combat.damage" );
		m_respawned = declare.Event( "combat.respawned" );
		m_roundStart = declare.Event( "game.round_start" );
		m_upper = declare.Layer( "upper" );
		m_stance = declare.Stance( "gun" );
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
			kind.fireSeconds = std::max( number( "gun.fire_seconds", 0.2f ), 0.02f );
			kind.automatic = number( "gun.automatic", 0.0f ) != 0.0f;
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
			if ( int( e.type ) == m_respawned.index && ctx.SlotOf( e.netIdA ) >= 0 )
			{
				Refill( ctx, PlayerSlot( ctx.SlotOf( e.netIdA ) ) );
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

	// The player's fields say what the gun in its hand has (0 with none): written when they change.
	void Publish( Context& ctx, PlayerSlot slot, int32_t ammo, int32_t magazine, bool reloading )
	{
		Shooter& s = m_shooters[size_t( slot )];
		uint32_t target = SlotTarget( slot );
		if ( s.ammo != ammo )
		{
			s.ammo = ammo;
			ctx.Set( target, m_ammo, ammo );
		}
		if ( s.magazine != magazine )
		{
			s.magazine = magazine;
			ctx.Set( target, m_magazine, magazine );
		}
		if ( s.reloading != int32_t( reloading ) )
		{
			s.reloading = int32_t( reloading );
			ctx.Set( target, m_reloading, reloading ? 1 : 0 );
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
		bool lost = holding && s.aiming && anim != nullptr && m_upper.Valid() &&
					( anim->aiming == 0 || int( anim->stances[m_upper.index] ) != m_stance.index + 1 );
		if ( holding != s.aiming || lost )
		{
			s.aiming = holding;
			ctx.Aim( target, holding );
			ctx.SetStance( target, m_upper, holding ? m_stance : StanceHandle{} );
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
		bool reloading = gun.reloadDoneTick != 0;
		Publish( ctx, slot, gun.ammo, kind->magazine, reloading );

		ItemKindHandle handle = ctx.ItemKindOf( inHand );
		bool trigger = kind->automatic ? ctx.Using( slot, handle ) : ctx.Used( slot, handle );
		if ( ( kind->automatic ? ctx.Using( slot, handle ) : true ) == false )
		{
			gun.wentDry = false;
		}
		if ( c->frozen )
		{
			return;
		}
		if ( ctx.Pressed( slot, m_reload ) && reloading == false && gun.ammo < kind->magazine )
		{
			StartReload( ctx, slot, gun, *kind, tick );
			return;
		}
		if ( trigger == false || reloading || tick < gun.nextShotTick )
		{
			return;
		}
		if ( gun.ammo <= 0 )
		{
			// Empty: one click and a reload for each pull of the trigger, however long it is held.
			if ( gun.wentDry == false )
			{
				gun.wentDry = kind->automatic;
				ctx.Emit( m_useFailed, target );
				StartReload( ctx, slot, gun, *kind, tick );
			}
			return;
		}
		SetRounds( ctx, inHand, gun, gun.ammo - 1 );
		gun.wentDry = false; // (it fires again: the next empty magazine clicks again)
		gun.nextShotTick = tick + Ticks( ctx, kind->fireSeconds );
		Publish( ctx, slot, gun.ammo, kind->magazine, false );
		Fire( ctx, slot, *kind );
	}

	void StartReload( Context& ctx, PlayerSlot slot, Gun& gun, const GunKind& kind, uint32_t tick )
	{
		gun.reloadDoneTick = tick + std::max( Ticks( ctx, kind.reloadSeconds ), 1u );
		Publish( ctx, slot, gun.ammo, kind.magazine, true );
		ctx.Emit( m_reloadEvent, SlotTarget( slot ) );
	}

	bool IsLivingPlayer( Context& ctx, uint32_t netId ) const
	{
		int slot = ctx.SlotOf( netId );
		const Character* c = slot >= 0 ? ctx.PlayerCharacter( PlayerSlot( slot ) ) : nullptr;
		return c != nullptr && c->dead == 0;
	}

	void Fire( Context& ctx, PlayerSlot slot, const GunKind& kind )
	{
		uint32_t shooter = SlotTarget( slot );
		// At what is under the crosshair, from the eye: whatever camera the player looks through.
		b3Vec3 eye, dir;
		RayHit hit;
		bool found = ctx.CastAim( slot, kind.range, hit, eye, dir );
		b3Vec3 end = found ? hit.point : b3MulAdd( eye, kind.range, dir );
		ctx.Emit( m_use, shooter, found ? hit.netId : 0, 0, eye, end );
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
	ActionHandle m_reload;
	FieldHandle m_ammo;
	FieldHandle m_magazine;
	FieldHandle m_reloading;
	FieldHandle m_rounds;
	EventHandle m_use;
	EventHandle m_useFailed;
	EventHandle m_hit;
	EventHandle m_reloadEvent;
	EventHandle m_damage;
	EventHandle m_respawned;
	EventHandle m_roundStart;
	LayerHandle m_upper;
	StanceHandle m_stance;
	SocketHandle m_hand;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_gun()
{
	return std::make_unique<GunMod>();
}
