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
//   - the slam's landing: no node finds who is near a point, and no motion starts on landing, so
//     the mod watches the diver and throws who is near when it lands (not predicted)
//
// Switched off (the "disabled" file): it is a game of its own. Start it by name, with the mods it
// needs and without the other weapons:
//   cb_server --mods inventory,combat,brawl
// Server options: brawl.jab, brawl.finisher, brawl.dash, brawl.uppercut, brawl.slam (damage).

#include "mod_api.h"

#include <array>
#include <cmath>

using namespace cb;
using namespace cb::mods;

namespace
{

constexpr float kSlamRadius = 5.0f;
constexpr float kSlamPush = 9.0f; // m/s away, and half of it up
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
			int diver = int( e.type ) == m_slamStarted.index ? ctx.SlotOf( e.netIdA ) : -1;
			if ( diver >= 0 )
			{
				m_diving[size_t( diver )] = true;
			}
		}
		// A diver that lands: everyone near is hurt and thrown away from where it landed.
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			const Character* c = ctx.PlayerCharacter( PlayerSlot( i ) );
			if ( m_diving[size_t( i )] == false || c == nullptr || c->dead != 0 )
			{
				m_diving[size_t( i )] = c != nullptr && c->dead == 0 && m_diving[size_t( i )];
				continue;
			}
			if ( c->grounded == 0 )
			{
				continue;
			}
			m_diving[size_t( i )] = false;
			uint32_t slammer = ctx.PlayerNetId( PlayerSlot( i ) );
			const Transform* landed = ctx.EntityTransform( slammer );
			if ( landed == nullptr )
			{
				continue;
			}
			b3Vec3 at = landed->position;
			ctx.Emit( m_slamLanded, slammer, 0, 0, at );
			for ( int j = 0; j < kMaxPlayers; ++j )
			{
				uint32_t other = ctx.PlayerNetId( PlayerSlot( j ) );
				const Transform* there = other != 0 ? ctx.EntityTransform( other ) : nullptr;
				if ( j == i || there == nullptr || ctx.PlayerCharacter( PlayerSlot( j ) ) == nullptr )
				{
					continue;
				}
				b3Vec3 to = b3Sub( there->position, at );
				float distance = b3Length( to );
				if ( distance > kSlamRadius )
				{
					continue;
				}
				b3Vec3 away = distance > 0.01f ? b3MulSV( 1.0f / distance, b3Vec3{ to.x, 0.0f, to.z } ) : b3Vec3{ 0.0f, 0.0f, 0.0f };
				b3Vec3 push = b3Add( b3MulSV( kSlamPush, away ), b3Vec3{ 0.0f, 0.5f * kSlamPush, 0.0f } );
				if ( Hurt( ctx, slammer, other, m_slamDamage, at, push ) )
				{
					ctx.Push( other, at, push, ImpulseVelocity );
				}
			}
		}
	}

private:
	// The kit's damage on a player: a quarter while it guards. False: not a player, or oneself.
	bool Hurt( Context& ctx, uint32_t attacker, uint32_t victim, int32_t amount, b3Vec3 point, b3Vec3 push )
	{
		int slot = victim != 0 && victim != attacker && ctx.IsPlayer( victim ) ? ctx.SlotOf( victim ) : -1;
		if ( slot < 0 )
		{
			return false;
		}
		if ( m_guard >= 0 && ctx.Using( PlayerSlot( slot ), ItemKindHandle{ m_guard } ) )
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
	EventHandle m_damage;
	int32_t m_slamDamage = 25;
	int m_guard = -1;
	std::array<bool, kMaxPlayers> m_diving{};
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_brawl()
{
	return std::make_unique<BrawlMod>();
}
