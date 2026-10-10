// Melee: a bat.
//
// What is in the right hand decides: a "melee.bat" there is out, wherever it came from. Who has
// one, and when it is out, is the inventory mod's: this mod only says a bat lives in slot 3, that a
// life starts with one, and that it hangs on the back while it is put away.
//
// How it is held and swung is the bat's own: its look brings an animation pack for the upper body
// (client/animation_packs/bat_hold.tscn, "melee.hold"), which plays instead of the holder's own
// upper body while a bat is in the hand. The legs keep the character's walk.
//
// Out, the "melee" stance is set and the body faces where the camera looks. The left mouse button
// swings: the "melee_swing" stance for kSwingSeconds, which the pack's Swing state plays on, and at
// the strike a fan of short rays in front of the chest looks for someone to hit. Hits are posed
// like everything else, so a swing lands where it is drawn.
//
// Health is not kept here. A hit goes out as "combat.damage", and the combat mod applies it and
// credits the kill: mods cooperate by event, not by call.

#include "mod_api.h"

#include <algorithm>
#include <array>
#include <vector>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr int32_t kDamage = 40;
constexpr float kSwingSeconds = 0.45f;	// the swing stance, then back to the ready stance
constexpr float kStrikeSeconds = 0.2f;	// when in the swing the hit is tested, unless the animation says
constexpr float kCooldownSeconds = 0.6f;
constexpr float kHotSeconds = 2.0f; // how long the bat glows after it hits someone
constexpr float kReach = 1.8f;			// metres from the chest
constexpr float kFan = 0.45f;			// radians either side of straight ahead
constexpr float kPush = 7.0f;			// m/s given to whatever it knocks over

uint32_t Ticks( const Context& ctx, float seconds )
{
	return uint32_t( seconds * float( ctx.Config().tickRate ) + 0.5f );
}

struct Swinger
{
	bool out = false;
	bool swinging = false;
	bool struck = false;
	uint32_t swingStart = 0;
	uint32_t nextSwing = 0;
};

// A bat that hit someone, and when it cools. By the item, not by who held it: it cools the same
// on a back, on the ground or in someone else's hand.
struct HotBat
{
	uint32_t netId = 0;
	uint32_t until = 0;
};

class MeleeMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "melee";
	}

	void Declare( Declarations& declare ) override
	{
		// The bat is an item of its own in the right hand: the swing animation plays its "slash",
		// and it has its own state (hot for a while after it hits someone).
		// Its body when it lies in the world is authored in its scene (client/prefabs/bat.tscn, the
		// CbItem) and baked to client/items/melee.bat.cfg.
		m_bat = declare.ItemKind( "melee.bat" );
		// The bat brings its own upper body: while it is held (from the slot or picked up), the
		// pack's "UpperBody" layer plays instead of the holder's own: ready, and the swing.
		declare.ItemLayers( m_bat, declare.AnimPack( "melee.hold" ) );
		// (That a bat takes half a second to pick up is authored with the item: the CbItem's
		// properties, "pickup.hold_seconds".)
		m_hand = declare.Socket( "RightHand" );
		declare.ItemProperty( m_bat, "inventory.start", 1.0f );
		declare.ItemProperty( m_bat, "holster", declare.Socket( "Back" ) );
		m_hot = declare.Field( "melee.hot", BoardType::Bool );
		m_full = declare.Layer( "full" );
		m_ready = declare.Stance( "melee" );
		m_swingStance = declare.Stance( "melee_swing" );
		// a = who swings.
		m_swing = declare.Event( "melee.swing" );
		// a = who swung, b = who or what was hit, value = damage, point = where, vector = normal.
		m_hit = declare.Event( "melee.hit" );
		m_damage = declare.Event( "combat.damage" );
		// A marker in the character's swing animation: the frame it connects (a = who swings).
		m_strike = declare.Event( "melee.strike" );
	}

	void Tick( Context& ctx ) override
	{
		uint32_t tick = ctx.Tick();
		// A swing that carries a strike marker (the pack's, or a character's own) hits on it; one
		// without, on the timer.
		bool marked = ctx.AnimationEmits( m_strike );
		std::vector<ModEventRecord> recent = marked ? ctx.RecentEvents() : std::vector<ModEventRecord>();
		for ( size_t i = 0; i < m_hotBats.size(); )
		{
			if ( tick >= m_hotBats[i].until )
			{
				ctx.Set( m_hotBats[i].netId, m_hot, 0 ); // nothing happens if the bat is gone
				m_hotBats.erase( m_hotBats.begin() + std::ptrdiff_t( i ) );
			}
			else
			{
				++i;
			}
		}
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			PlayerSlot slot = PlayerSlot( i );
			Swinger& s = m_swingers[size_t( i )];
			if ( ctx.Joining( slot ) || ctx.Leaving( slot ) )
			{
				s = Swinger{};
			}
			uint32_t netId = ctx.PlayerNetId( slot );
			const Character* c = ctx.PlayerCharacter( slot );
			if ( netId == 0 || c == nullptr )
			{
				continue;
			}
			uint32_t target = SlotTarget( slot );

			uint32_t inHand = ctx.HeldItem( slot, m_hand );
			bool batInHand = inHand != 0 && ctx.ItemKindOf( inHand ).index == m_bat.index;

			bool holding = batInHand && c->dead == 0;
			if ( holding != s.out )
			{
				s.out = holding;
				s.swinging = false;
				ctx.SetStance( target, m_full, holding ? m_ready : StanceHandle{} );
				if ( holding )
				{
					ctx.FaceCamera( target, true );
				}
			}
			if ( holding == false )
			{
				continue;
			}
			if ( s.swinging )
			{
				uint32_t elapsed = tick - s.swingStart;
				bool strike = marked == false && elapsed >= Ticks( ctx, kStrikeSeconds );
				for ( const ModEventRecord& e : recent )
				{
					strike |= e.type == uint16_t( m_strike.index ) && e.netIdA == netId;
				}
				if ( s.struck == false && strike )
				{
					s.struck = true;
					if ( Strike( ctx, slot, netId ) )
					{
						// A hit heats the bat: one field on the bat itself, until it cools.
						ctx.Set( inHand, m_hot, 1 );
						uint32_t until = tick + Ticks( ctx, kHotSeconds );
						auto hot = std::find_if( m_hotBats.begin(), m_hotBats.end(), [&]( const HotBat& b ) { return b.netId == inHand; } );
						if ( hot != m_hotBats.end() )
						{
							hot->until = until;
						}
						else
						{
							m_hotBats.push_back( { inHand, until } );
						}
					}
				}
				if ( elapsed >= Ticks( ctx, kSwingSeconds ) )
				{
					s.swinging = false;
					ctx.SetStance( target, m_full, m_ready );
				}
				continue;
			}
			if ( ctx.Used( slot, m_bat ) && c->frozen == 0 && tick >= s.nextSwing )
			{
				s.swinging = true;
				s.struck = false;
				s.swingStart = tick;
				s.nextSwing = tick + Ticks( ctx, kCooldownSeconds );
				ctx.SetStance( target, m_full, m_swingStance );
				ctx.Emit( m_swing, target );
			}
		}
	}

private:
	// True when it hit a player.
	bool Strike( Context& ctx, PlayerSlot slot, uint32_t netId )
	{
		uint32_t target = SlotTarget( slot );
		// From the chest, fanned out sideways across where the player looks, and as far up or down
		// as it looks: the upper body bows with the camera, and the bat goes where the body does.
		b3Vec3 chest = b3Sub( ctx.EyePosition( slot ), b3Vec3{ 0.0f, 0.45f, 0.0f } );
		b3Vec3 aim = ctx.AimDirection( slot );
		float level = b3Length( b3Vec3{ aim.x, 0.0f, aim.z } ); // the cosine of the pitch
		if ( level < 1e-4f )
		{
			return false;
		}
		b3Vec3 flat = b3MulSV( 1.0f / level, b3Vec3{ aim.x, 0.0f, aim.z } );
		for ( float angle : { 0.0f, -kFan, kFan, -0.5f * kFan, 0.5f * kFan } )
		{
			b3Quat turn = b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, angle );
			b3Vec3 side = b3RotateVector( turn, flat );
			// What it knocks away still goes along the ground (`side`), not into it.
			b3Vec3 dir = { side.x * level, aim.y, side.z * level };
			RayHit hit;
			if ( ctx.CastRay( chest, b3MulSV( kReach, dir ), netId, hit ) == false )
			{
				continue;
			}
			if ( ctx.IsPlayer( hit.netId ) )
			{
				b3Vec3 push = b3Add( b3MulSV( kPush, side ), b3Vec3{ 0.0f, 2.0f, 0.0f } );
				ctx.Emit( m_hit, target, hit.netId, kDamage, hit.point, hit.normal );
				ctx.Emit( m_damage, target, hit.netId, kDamage, hit.point, push );
				return true; // one player per swing
			}
			if ( ctx.IsDynamic( hit.netId ) )
			{
				ctx.Emit( m_hit, target, hit.netId, 0, hit.point, hit.normal );
				ctx.Push( hit.netId, hit.point, b3MulSV( kPush, side ), ImpulseVelocity );
				return false;
			}
		}
		return false;
	}

	std::array<Swinger, kMaxPlayers> m_swingers{};
	std::vector<HotBat> m_hotBats;
	ItemKindHandle m_bat;
	SocketHandle m_hand;
	FieldHandle m_hot;
	LayerHandle m_full;
	StanceHandle m_ready;
	StanceHandle m_swingStance;
	EventHandle m_swing;
	EventHandle m_hit;
	EventHandle m_damage;
	EventHandle m_strike;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_melee()
{
	return std::make_unique<MeleeMod>();
}
