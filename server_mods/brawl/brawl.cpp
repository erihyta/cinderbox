// A brawler's kit: the measure of step 5 (ROADMAP.md), made with today's nodes. Five skills on the
// hotbar, as a Roblox fighting game has them, each an item of its own (client/prefabs). The skills
// are used by their slots' keys where they are (CbItem.slot_key_uses): nothing changes hands.
//
//   slot 1  Fist         selected, click: a jab; three in a row, the third a finisher that flings
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
//
// It runs by default, beside the weapons; its skills then go to whatever slots are free (their
// slots' keys use them wherever they are). Alone, with only what it needs:
//   cb_server --mods inventory,combat,brawl
// Server options: brawl.jab, brawl.finisher, brawl.dash, brawl.uppercut, brawl.slam (damage).

#include "mod_api.h"

#include <array>
#include <cmath>

using namespace cb;
using namespace cb::mods;

namespace
{

constexpr float kSlamPush = 9.0f; // m/s away, and half of it up: what the sheet throws (for a ragdoll)
constexpr float kGuardShare = 0.25f;

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
		m_damage = declare.Event( "combat.damage" );
		declare.Motions( "brawl.moves" );
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
		m_guard = ctx.Schema().FindItemKind( "brawl.guard" );
	}

	void Tick( Context& ctx ) override
	{
		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			for ( size_t i = 0; i < m_hits.size(); ++i )
			{
				if ( int( e.type ) == m_hits[i].index )
				{
					Hurt( ctx, e.netIdA, e.netIdB, m_amounts[i], e.point, {} );
				}
			}
			// Someone the slam's shockwave found (its sheet throws them): hurt, by the slam's damage.
			if ( int( e.type ) == m_slamHit.index )
			{
				const Transform* from = ctx.EntityTransform( e.netIdA );
				b3Vec3 away = from != nullptr ? b3Sub( e.point, from->position ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
				away.y = 0.0f;
				float length = b3Length( away );
				away = length > 0.01f ? b3MulSV( 1.0f / length, away ) : away;
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
		if ( slot >= 0 && m_guard >= 0 && ctx.Using( PlayerSlot( slot ), ItemKindHandle{ m_guard } ) )
		{
			amount = int32_t( std::lround( float( amount ) * kGuardShare ) );
		}
		ctx.Emit( m_damage, attacker, victim, amount, point, push );
		return true;
	}

	std::array<EventHandle, 4> m_hits;
	std::array<int32_t, 4> m_amounts{};
	EventHandle m_slamStarted;
	EventHandle m_slamLanded;
	EventHandle m_slamHit;
	EventHandle m_damage;
	int32_t m_slamDamage = 25;
	int m_guard = -1;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_brawl()
{
	return std::make_unique<BrawlMod>();
}
