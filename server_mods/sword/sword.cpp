// Sword (M279): a blade, the slice's first weapon made for it.
//
// What the sword is and does to bodies is its look's (client/prefabs/sword.tscn and its sheet,
// sword.rules), so it is predicted:
//   left mouse button   a swing: the pack (sword.hold, the mannequin's sword clips) plays it, and at
//                       its sword.strike marker a box where the player looks, pitch and all (the
//                       upper body bends with the camera: it can hit above and below), finds whom it
//                       hits: sword.hit (a = who swung, b = who was hit), and shoves them
//   right mouse button  held: the block (slow, no jump; the pack holds the blade up)
// This file is what a sheet cannot say:
//   - while a sword is in the hand, its holder faces where the camera looks (the bat's way), so
//     the upper body bends with the pitch and the swing goes where it is drawn
//   - damage: a hit is sword.damage (30) as combat.damage, unless the one hit blocks with a sword
//     of its own, facing the swing (within 75 degrees): then a fifth (sword.block_share) and
//     sword.blocked is said; and if its block went up within sword.parry_seconds (0.2) before the
//     hit, a parry: nothing, sword.parried is said, and the swinger reels: pushed back, slowed and
//     unable to swing for sword.stun_seconds (0.8; the field sword.stunned)
//
// Options: sword.damage, sword.block_share, sword.parry_seconds, sword.stun_seconds.

#include "mod_api.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr float kBlockCone = 0.26f; // cos(75 degrees): how far round a block covers
constexpr float kShove = 6.0f;		// m/s: what the sheet shoves with (the ragdoll's last hit)
constexpr float kRecoil = 6.0f;		// m/s back, for a swinger parried

struct Wielder
{
	bool out = false;
	uint32_t blockSince = 0; // the tick its block went up (0: down)
	uint32_t stunnedUntil = 0;
};

class SwordMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "sword";
	}

	void Declare( Declarations& declare ) override
	{
		// The sword is its look's: kind sword.blade, its pack, its holster, its sheet.
		for ( const Declarations::OwnItem& item : declare.ItemsOfLook() )
		{
			m_sword = item.look.kind == "sword.blade" ? item.kind : m_sword;
		}
		m_hand = declare.Socket( "RightHand" );
		m_block = declare.Action( "use2", "MouseRight" ); // (the item mod's: the same name, the same action)
		m_stunned = declare.Field( "sword.stunned", BoardType::Bool, BoardScope::Entity, true );
		// What the sheet says: a swing starting, its hit; the clip's markers.
		declare.Event( "sword.swing" );
		m_hit = declare.Event( "sword.hit" );
		declare.Event( "sword.strike" );
		declare.Event( "sword.done" );
		// What this file says: at the one who blocked, b = the swinger.
		m_blocked = declare.Event( "sword.blocked" );
		m_parried = declare.Event( "sword.parried" );
		m_damage = declare.Event( "combat.damage" );
		m_team = declare.Field( "team.id", BoardType::Int, BoardScope::Entity, true ); // (the teams mod's)
	}

	void Start( Context& ctx ) override
	{
		m_amount = int32_t( ctx.Option( "sword.damage", 30.0 ) );
		m_blockShare = float( std::clamp( ctx.Option( "sword.block_share", 0.2 ), 0.0, 1.0 ) );
		m_parryTicks = uint32_t( std::max( 0.0, ctx.Option( "sword.parry_seconds", 0.2 ) ) * ctx.Config().tickRate + 0.5 );
		m_stunTicks = uint32_t( std::max( 0.0, ctx.Option( "sword.stun_seconds", 0.8 ) ) * ctx.Config().tickRate + 0.5 );
		m_friendlyFire = ctx.Option( "combat.friendly_fire", 0.0 ) != 0.0;
	}

	void Tick( Context& ctx ) override
	{
		const uint32_t tick = ctx.Tick();
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			const PlayerSlot slot = PlayerSlot( i );
			Wielder& w = m_wielders[size_t( i )];
			if ( ctx.Joining( slot ) || ctx.Leaving( slot ) )
			{
				w = Wielder{};
			}
			const Character* c = ctx.PlayerCharacter( slot );
			if ( c == nullptr || ctx.PlayerNetId( slot ) == 0 )
			{
				continue;
			}
			const uint32_t target = SlotTarget( slot );
			const bool holding = c->dead == 0 && Holding( ctx, slot );
			if ( holding != w.out )
			{
				w.out = holding;
				// Out: it faces where it looks (and so bends with the pitch). Put away, the inventory
				// mod lets it go (whatever is taken out next says for itself).
				if ( holding )
				{
					ctx.FaceCamera( target, true );
				}
			}
			const bool blocking = holding && ctx.Held( slot, m_block ) && c->frozen == 0;
			w.blockSince = blocking ? ( w.blockSince != 0 ? w.blockSince : tick ) : 0;
			if ( w.stunnedUntil != 0 && tick >= w.stunnedUntil )
			{
				w.stunnedUntil = 0;
				ctx.Set( target, m_stunned, 0 );
				ctx.ResetMove( target, MoveParam::WalkSpeed );
				ctx.ResetMove( target, MoveParam::SprintSpeed );
				ctx.ResetMove( target, MoveParam::JumpSpeed );
			}
		}

		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			if ( int( e.type ) == m_hit.index )
			{
				Hit( ctx, e.netIdA, e.netIdB, e.point );
			}
		}
	}

private:
	bool Holding( Context& ctx, PlayerSlot slot ) const
	{
		const uint32_t inHand = ctx.HeldItem( slot, m_hand );
		return inHand != 0 && m_sword.Valid() && ctx.ItemKindOf( inHand ).index == m_sword.index;
	}

	void Hit( Context& ctx, uint32_t attacker, uint32_t victim, b3Vec3 point )
	{
		const int attackerSlot = ctx.SlotOf( attacker );
		if ( victim == 0 || victim == attacker || attackerSlot < 0 )
		{
			return;
		}
		const Transform* from = ctx.EntityTransform( attacker );
		const Transform* to = ctx.EntityTransform( victim );
		if ( from == nullptr || to == nullptr )
		{
			return;
		}
		b3Vec3 away = b3Sub( to->position, from->position );
		away.y = 0.0f;
		const float length = b3Length( away );
		away = length > 0.01f ? b3MulSV( 1.0f / length, away ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
		const int32_t team = ctx.Get( attacker, m_team );
		const bool teammates = m_friendlyFire == false && team != 0 && team == ctx.Get( victim, m_team );

		// A block: the one hit holds a sword up, facing the swing.
		const int victimSlot = ctx.SlotOf( victim );
		const Wielder* guard = victimSlot >= 0 ? &m_wielders[size_t( victimSlot )] : nullptr;
		bool blocks = false;
		if ( guard != nullptr && guard->blockSince != 0 )
		{
			const b3Vec3 look = ctx.AimDirection( PlayerSlot( victimSlot ) );
			const b3Vec3 facing = b3Normalize( b3Vec3{ look.x, 0.0f, look.z } );
			blocks = b3Dot( facing, b3MulSV( -1.0f, away ) ) >= kBlockCone;
		}
		if ( blocks && teammates == false && ctx.Tick() - guard->blockSince <= m_parryTicks )
		{
			// A parry: nothing lands, and the swinger reels.
			ctx.Emit( m_parried, victim, attacker, 0, point );
			ctx.Push( attacker, from->position, b3Add( b3MulSV( -kRecoil, away ), b3Vec3{ 0.0f, 2.0f, 0.0f } ), ImpulseVelocity );
			Wielder& reeling = m_wielders[size_t( attackerSlot )];
			reeling.stunnedUntil = ctx.Tick() + m_stunTicks;
			const uint32_t target = SlotTarget( PlayerSlot( attackerSlot ) );
			ctx.Set( target, m_stunned, 1 );
			ctx.SetMove( target, MoveParam::WalkSpeed, 1.0f );
			ctx.SetMove( target, MoveParam::SprintSpeed, 1.0f );
			ctx.SetMove( target, MoveParam::JumpSpeed, 0.0f );
			return;
		}
		int32_t amount = m_amount;
		if ( blocks )
		{
			amount = int32_t( std::lround( float( amount ) * m_blockShare ) );
			if ( teammates == false )
			{
				ctx.Emit( m_blocked, victim, attacker, amount, point );
			}
		}
		// (Combat spares a teammate itself.)
		ctx.Emit( m_damage, attacker, victim, amount, point, b3Add( b3MulSV( kShove, away ), b3Vec3{ 0.0f, 1.5f, 0.0f } ) );
	}

	std::array<Wielder, kMaxPlayers> m_wielders{};
	ItemKindHandle m_sword;
	SocketHandle m_hand;
	ActionHandle m_block;
	FieldHandle m_stunned;
	FieldHandle m_team;
	EventHandle m_hit;
	EventHandle m_blocked;
	EventHandle m_parried;
	EventHandle m_damage;
	int32_t m_amount = 30;
	float m_blockShare = 0.2f;
	uint32_t m_parryTicks = 12;
	uint32_t m_stunTicks = 48;
	bool m_friendlyFire = false;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_sword()
{
	return std::make_unique<SwordMod>();
}
