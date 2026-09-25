// Melee: a bat on slot 3.
//
// Out, it is a full-body stance ("melee": the character's own idle, walk and run with the bat) and
// the body faces where the camera looks. The left mouse button swings: the full-body layer plays the
// "melee_swing" stance, and at the strike a fan of short rays in front of the chest looks for
// someone to hit. Hits are posed like everything else, so a swing lands where it is drawn.
//
// Health is not kept here. A hit goes out as "combat.damage", and whichever mod keeps health (the
// pistol mod today) applies it and credits the kill: mods cooperate by event, not by call.

#include "mod_api.h"

#include <array>
#include <vector>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr int32_t kMeleeSlot = 3; // loadout.slot value while the bat is out
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
	uint32_t hotUntil = 0; // 0: the bat is cold
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
		m_fire = declare.Action( "fire", "MouseLeft" ); // shared with the pistol: whichever is out
		m_loadout = declare.Field( "loadout.slot", BoardType::Int );
		// The bat is an item of its own in the right hand: the swing animation plays its "slash",
		// and it has its own state (hot for a while after it hits someone).
		m_bat = declare.ItemKind( "melee.bat" );
		m_hand = declare.Socket( "RightHand" );
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
		// Characters whose swing carries a strike marker hit on it; the rest on the timer.
		bool marked = ctx.AnimationEmits( m_strike );
		std::vector<ModEventRecord> recent = marked ? ctx.RecentEvents() : std::vector<ModEventRecord>();
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

			bool holding = ctx.Get( netId, m_loadout ) == kMeleeSlot && c->dead == 0;
			if ( holding != s.out )
			{
				s.out = holding;
				s.swinging = false;
				s.hotUntil = 0;
				ctx.SetStance( target, m_full, holding ? m_ready : StanceHandle{} );
				// Taking the bat out puts one in the hand; putting it away (or dying) takes it.
				if ( holding )
				{
					ctx.SpawnItem( target, m_bat, m_hand );
				}
				else if ( ctx.HeldItem( slot, m_hand ) != 0 )
				{
					ctx.Destroy( ItemTarget( slot, m_hand ) );
				}
				if ( holding )
				{
					ctx.FaceCamera( target, true );
				}
			}
			if ( holding == false )
			{
				continue;
			}
			if ( s.hotUntil != 0 && tick >= s.hotUntil )
			{
				s.hotUntil = 0;
				ctx.Set( ItemTarget( slot, m_hand ), m_hot, 0 );
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
						// A hit heats the bat: one field on the bat itself.
						ctx.Set( ItemTarget( slot, m_hand ), m_hot, 1 );
						s.hotUntil = tick + Ticks( ctx, kHotSeconds );
					}
				}
				if ( elapsed >= Ticks( ctx, kSwingSeconds ) )
				{
					s.swinging = false;
					ctx.SetStance( target, m_full, m_ready );
				}
				continue;
			}
			if ( ctx.Pressed( slot, m_fire ) && c->frozen == 0 && tick >= s.nextSwing )
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
		// From the chest, level, fanned out across where the player looks.
		b3Vec3 chest = b3Sub( ctx.EyePosition( slot ), b3Vec3{ 0.0f, 0.45f, 0.0f } );
		b3Vec3 aim = ctx.AimDirection( slot );
		b3Vec3 flat = b3Normalize( b3Vec3{ aim.x, 0.0f, aim.z } );
		for ( float angle : { 0.0f, -kFan, kFan, -0.5f * kFan, 0.5f * kFan } )
		{
			b3Quat turn = b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, angle );
			b3Vec3 dir = b3RotateVector( turn, flat );
			RayHit hit;
			if ( ctx.CastRay( chest, b3MulSV( kReach, dir ), netId, hit ) == false )
			{
				continue;
			}
			if ( ctx.IsPlayer( hit.netId ) )
			{
				b3Vec3 push = b3Add( b3MulSV( kPush, dir ), b3Vec3{ 0.0f, 2.0f, 0.0f } );
				ctx.Emit( m_hit, target, hit.netId, kDamage, hit.point, hit.normal );
				ctx.Emit( m_damage, target, hit.netId, kDamage, hit.point, push );
				return true; // one player per swing
			}
			if ( ctx.IsDynamic( hit.netId ) )
			{
				ctx.Emit( m_hit, target, hit.netId, 0, hit.point, hit.normal );
				ctx.Push( hit.netId, hit.point, b3MulSV( kPush, dir ), ImpulseVelocity );
				return false;
			}
		}
		return false;
	}

	std::array<Swinger, kMaxPlayers> m_swingers{};
	ActionHandle m_fire;
	FieldHandle m_loadout;
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
