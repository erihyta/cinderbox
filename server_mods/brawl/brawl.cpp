// A brawler's kit (first made in M163). Five skills on the
// hotbar, as a Roblox fighting game has them, each an item of its own (client/prefabs). The skills
// are used by their slots' keys where they are (CbItem.slot_key_uses): nothing changes hands.
//
//   slot 1  Fist         selected, click: a jab; three in a row, the third a finisher that tosses
//                        whoever it hits as a ragdoll (they get up where they land; guarding, only shoved)
//   slot 2  Dash strike  2: a lunge, and whoever is ahead is knocked back (3 s)
//   slot 3  Uppercut     3, on the ground: whoever is ahead is launched up (4 s)
//   slot 4  Ground slam  4, in the air: a dive, and a shockwave where you land (5 s)
//   slot 5  Guard        hold 5: slow, and the kit's hits on you do a quarter
//
// What a skill does to bodies (the lunge, the knockback, the launch, the dive) is its scene's: every
// simulation runs it from the player's input, so it is predicted. The cooldowns and the combo are
// fields the scenes' motions write and count down. This file is what nodes cannot say:
//   - damage: a hit is the probe's event (brawl.jab.hit ..., with who was hit), and this mod turns
//     it into combat.damage, a quarter of it on someone guarding
//   - the slam's shockwave is its sheet's too (a motion block waits for the ground, then finds who
//     is in its Shockwave region and throws them, predicted); this mod hurts each one it says
//     (brawl.slam.hit, with who was found)
//   - the last hit a ragdoll takes (M275): a hit that fells someone (the finisher's toss, a killing
//     blow) strikes the ragdoll where it landed, as hard as the sheet shoves, so it folds there
//   - a tossed body breaks walls it flies into (brawl.toss_break m/s and up, a hole of
//     brawl.toss_hole m)
//   - teams (M276): the finisher does not toss a teammate (team.id the same, not 0) unless
//     combat.friendly_fire; combat does not let the hit hurt either
//   - with the fist in the hand its holder faces where the camera looks (M279, the bat's way): the
//     upper body bends with the pitch, and the fist's box turns with the look, so a punch reaches
//     up a ramp and down at someone lower
//
// It runs by default, beside the weapons; its skills then go to whatever slots are free (their
// slots' keys use them wherever they are). Alone, with only what it needs:
//   cb_server --mods inventory,combat,brawl
// Server options: brawl.jab, brawl.finisher, brawl.dash, brawl.uppercut, brawl.slam (damage);
// brawl.toss_break (8: m/s a tossed body breaks walls at; 0: never), brawl.toss_hole (1.2 m).

#include "mod_api.h"

#include <algorithm>
#include <array>
#include <cmath>

using namespace cb;
using namespace cb::mods;

namespace
{

constexpr float kSlamPush = 9.0f; // m/s away, and half of it up: what the sheet throws (for a ragdoll)
constexpr float kGuardShare = 0.25f;

// What each hit's sheet shoves whoever it hits with (jab, finisher, dash strike, uppercut: their
// impulses, m/s ahead and up): the last hit their ragdoll takes if it fells them.
struct Shove
{
	float ahead, up;
};
constexpr Shove kShoves[4] = { { 4.0f, 0.0f }, { 14.0f, 5.0f }, { 10.0f, 0.0f }, { 0.0f, 12.0f } };

class BrawlMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "brawl";
	}

	void Declare( Declarations& declare ) override
	{
		// The fields the skills' rules and the motions read and write.
		declare.Field( "brawl.combo", BoardType::Int );
		declare.Field( "brawl.combo_left", BoardType::Float );
		declare.Field( "brawl.cd_dash", BoardType::Float );
		declare.Field( "brawl.cd_upper", BoardType::Float );
		declare.Field( "brawl.cd_slam", BoardType::Float );
		// What the skills say (their use_emits, their probes' hold_emits), and the slam's landing.
		m_hits[0] = declare.Event( "brawl.jab.hit" );
		m_hits[1] = declare.Event( "brawl.finisher.hit" );
		m_hits[2] = declare.Event( "brawl.dash_strike.hit" );
		m_hits[3] = declare.Event( "brawl.uppercut.hit" );
		declare.Event( "brawl.dash_strike.started" );
		declare.Event( "brawl.uppercut.started" );
		declare.Event( "brawl.guard.up" );
		m_slamStarted = declare.Event( "brawl.slam.started" );
		m_slamLanded = declare.Event( "brawl.slam.landed" );
		m_slamHit = declare.Event( "brawl.slam.hit" );
		// A marker in the fist's punches (its pack, brawl.fists): the frame the fist lands. The
		// fist's sheet hits on it, not on the click.
		declare.Event( "brawl.fist.strike" );
		// Another: the fist is back, the next punch may start (its sheet waits for it).
		declare.Event( "brawl.fist.ready" );
		m_damage = declare.Event( "combat.damage" );
		m_team = declare.Field( "team.id", BoardType::Int, BoardScope::Entity, true ); // (the teams mod's)
		declare.Motions( "brawl.moves" );
		m_hand = declare.Socket( "RightHand" );
		// The five skills: all of them are their scenes'.
		declare.ItemsOfLook();
	}

	void Start( Context& ctx ) override
	{
		const char* names[] = { "brawl.jab", "brawl.finisher", "brawl.dash", "brawl.uppercut" };
		const double defaults[] = { 8, 20, 15, 18 };
		for ( int i = 0; i < 4; ++i )
		{
			m_amounts[size_t( i )] = int32_t( ctx.Option( names[i], defaults[i] ) );
		}
		m_slamDamage = int32_t( ctx.Option( "brawl.slam", 25.0 ) );
		m_tossBreak = float( std::max( 0.0, ctx.Option( "brawl.toss_break", 8.0 ) ) );
		m_tossHole = float( std::max( 0.1, ctx.Option( "brawl.toss_hole", 1.2 ) ) );
		m_friendlyFire = ctx.Option( "combat.friendly_fire", 0.0 ) != 0.0;
		m_guard = ctx.Schema().FindItemKind( "brawl.guard" );
		m_fist = ctx.Schema().FindItemKind( "brawl.fist" );
	}

	void Tick( Context& ctx ) override
	{
		// The fist out: its holder faces the camera (put away, the inventory mod lets it go).
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			const PlayerSlot slot = PlayerSlot( i );
			const Character* c = ctx.PlayerCharacter( slot );
			const uint32_t inHand = c != nullptr && c->dead == 0 ? ctx.HeldItem( slot, m_hand ) : 0;
			const bool fist = inHand != 0 && m_fist >= 0 && ctx.ItemKindOf( inHand ).index == m_fist;
			if ( ctx.Joining( slot ) || ctx.Leaving( slot ) )
			{
				m_fistOut[size_t( i )] = false;
			}
			if ( fist != m_fistOut[size_t( i )] )
			{
				m_fistOut[size_t( i )] = fist;
				if ( fist )
				{
					ctx.FaceCamera( SlotTarget( slot ), true );
				}
			}
		}
		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			for ( size_t i = 0; i < m_hits.size(); ++i )
			{
				if ( int( e.type ) != m_hits[i].index )
				{
					continue;
				}
				const b3Vec3 shove = b3Add( b3MulSV( kShoves[i].ahead, Away( ctx, e.netIdA, e.point ) ), b3Vec3{ 0.0f, kShoves[i].up, 0.0f } );
				if ( Hurt( ctx, e.netIdA, e.netIdB, m_amounts[i], e.point, shove ) && i == 1 && Guarding( ctx, e.netIdB ) == false &&
					 ctx.IsDown( e.netIdB ) == false && Teammates( ctx, e.netIdA, e.netIdB ) == false )
				{
					// The finisher tosses: the sheet's shove (predicted) is already its velocity, which
					// the ragdoll keeps; struck where the fist landed, it folds there.
					ctx.Toss( e.netIdB, {}, e.point, shove, m_tossBreak, m_tossHole );
				}
			}
			// Someone the slam's shockwave found (its sheet throws them): hurt, by the slam's damage.
			if ( int( e.type ) == m_slamHit.index )
			{
				const b3Vec3 away = Away( ctx, e.netIdA, e.point );
				Hurt( ctx, e.netIdA, e.netIdB, m_slamDamage, e.point, b3Add( b3MulSV( kSlamPush, away ), b3Vec3{ 0.0f, 0.5f * kSlamPush, 0.0f } ) );
			}
		}
	}

	bool Hurt( Context& ctx, uint32_t attacker, uint32_t victim, int32_t amount, b3Vec3 point, b3Vec3 push )
	{
		if ( victim == 0 || victim == attacker )
		{
			return false;
		}
		// A player, or a body a mod drives (an NPC: combat hurts it too).
		int slot = ctx.IsPlayer( victim ) ? ctx.SlotOf( victim ) : -1;
		if ( slot < 0 && ctx.DrivenByMod( victim ) == false )
		{
			return false;
		}
		if ( Guarding( ctx, victim ) )
		{
			amount = int32_t( std::lround( float( amount ) * kGuardShare ) );
		}
		ctx.Emit( m_damage, attacker, victim, amount, point, push );
		return true;
	}

	// Along the ground from the attacker out to the point (none: no direction).
	static b3Vec3 Away( Context& ctx, uint32_t attacker, b3Vec3 point )
	{
		const Transform* from = ctx.EntityTransform( attacker );
		b3Vec3 away = from != nullptr ? b3Sub( point, from->position ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
		away.y = 0.0f;
		const float length = b3Length( away );
		return length > 0.01f ? b3MulSV( 1.0f / length, away ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
	}

	bool Teammates( Context& ctx, uint32_t a, uint32_t b ) const
	{
		const int32_t team = ctx.Get( a, m_team );
		return m_friendlyFire == false && team != 0 && team == ctx.Get( b, m_team );
	}

	bool Guarding( Context& ctx, uint32_t victim ) const
	{
		const int slot = ctx.IsPlayer( victim ) ? ctx.SlotOf( victim ) : -1;
		return slot >= 0 && m_guard >= 0 && ctx.Using( PlayerSlot( slot ), ItemKindHandle{ m_guard } );
	}

	std::array<EventHandle, 4> m_hits;
	std::array<int32_t, 4> m_amounts{};
	EventHandle m_slamStarted;
	EventHandle m_slamLanded;
	EventHandle m_slamHit;
	EventHandle m_damage;
	FieldHandle m_team;
	bool m_friendlyFire = false;
	int32_t m_slamDamage = 25;
	float m_tossBreak = 8.0f;
	float m_tossHole = 1.2f;
	int m_guard = -1;
	int m_fist = -1;
	SocketHandle m_hand;
	std::array<bool, kMaxPlayers> m_fistOut{};
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_brawl()
{
	return std::make_unique<BrawlMod>();
}
