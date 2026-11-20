// Bots (M281; step 4's bots that fight): players the server plays, so a game is never empty and
// one person can try the slice alone. Off by default: it runs when --mods names it (the tdm preset
// does), and it adds no bot until bots.fill says how many players a game should have.
//
// A bot is a player like any other (Context::AddBot): in a slot of its own, on the names list and
// the scoreboard, in a team (the teams mod puts it in the smaller one), given the kit each life as
// a player is; it presses keys, which this file decides each tick (Context::BotInput), and every
// simulation runs them as a player's input. What it does:
//   - fills the game up to bots.fill players: one joins (a second apart) while there are fewer, and
//     one leaves when a person joins and there are more (from the bigger team)
//   - keeps the teams even: a bot in a team two or more bigger than another moves over
//   - finds the nearest player of another team (anyone, without teams) who is up, and goes there
//     along the server's navigation mesh, running while far; jumps when it is stuck
//   - fights each life with the fist and the brawl kit, or with the sword (one in three lives):
//       fist   jabs in reach (two jabs and the finisher); the dash strike from 4 to 7.5 m; now and
//              then the uppercut in reach, and the slam from the air onto someone below; guards
//              (holds the guard's key) now and then when someone close starts an attack
//       sword  swings in reach; blocks now and then when someone close starts a swing (in time,
//              a parry)
//     facing them, the camera pitched at their chest, with a reaction time and a little aim error
//     (bots.skill: 0 slow and wild, 1 quick and sure; 0.5)
//
// What it knows of the kit is by name, from the schema: the items brawl.fist, brawl.dash_strike,
// brawl.uppercut, brawl.slam, brawl.guard and sword.blade wherever the inventory put them, the
// fields team.id and brawl.cd_*, the action use2 (the sword's block), the intent teams.join, the
// events brawl.*.started and sword.swing. A kit that is not running is a key it never presses.
//
// Options: bots.fill (0: none; players a game is filled up to), bots.skill (0.5).

#include "mod_api.h"
#include "detmath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr uint32_t kThinkEvery = 10; // ticks between choosing whom to go for
constexpr uint32_t kSeatsEvery = 60; // ticks between filling and evening up
constexpr float kNear = 2.2f;		 // metres: in reach of a jab or a swing
constexpr float kRun = 8.0f;		 // farther: it runs
constexpr float kPi = 3.14159265f;

enum Kit
{
	KitFist,
	KitDash,
	KitUpper,
	KitSlam,
	KitGuard,
	KitSword,
	KitCount
};
const char* const kKitKinds[KitCount] = { "brawl.fist", "brawl.dash_strike", "brawl.uppercut", "brawl.slam", "brawl.guard", "sword.blade" };

struct Brain
{
	PlayerSlot slot = 0;
	int number = 0;		  // "Bot <number>"
	bool sword = false;	  // this life's weapon
	bool wasDead = true;  // (a life starts when it is up again)
	uint8_t intents = 0;  // PlayerInput::intentSeq
	uint8_t intent = 0;	  // the last intent sent (it stays in the input)
	uint8_t intentA = 0;
	uint32_t asked = 0;	  // the tick it last asked for its weapon
	uint32_t target = 0;  // NetId
	std::vector<b3Vec3> path;
	size_t next = 0;
	uint32_t repath = 0;
	b3Vec3 pathTo = {};
	// What it is doing about it, until when.
	uint32_t clickUntil = 0, nextClick = 0;
	uint32_t keyUntil = 0;
	uint16_t key = 0;	   // the slots' keys held meanwhile
	uint32_t guardFrom = 0, guardUntil = 0;
	uint32_t blockFrom = 0, blockUntil = 0;
	uint32_t jumpUntil = 0;
	int8_t strafe = 0;
	uint32_t strafeUntil = 0;
	// Stuck: where it was a second ago, while it meant to go somewhere.
	b3Vec3 was = {};
	uint32_t wasAt = 0;
	float aimError = 0.0f; // radians, chosen now and then
};

class BotsMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "bots";
	}

	void Declare( Declarations& ) override
	{
		// Nothing of its own: it plays with what the other mods declared, found by name (Start).
	}

	void Start( Context& ctx ) override
	{
		m_fill = std::clamp( int( ctx.Option( "bots.fill", 0.0 ) ), 0, 64 );
		m_skill = float( std::clamp( ctx.Option( "bots.skill", 0.5 ), 0.0, 1.0 ) );
		const ModSchema& schema = ctx.Schema();
		for ( int k = 0; k < KitCount; ++k )
		{
			m_kinds[size_t( k )] = schema.FindItemKind( kKitKinds[k] );
		}
		auto field = [&]( const char* name ) {
			const BoardField* f = schema.FindField( name );
			return f != nullptr ? FieldHandle{ f->slot, f->scope, f->type } : FieldHandle{};
		};
		m_team = field( "team.id" );
		m_cdDash = field( "brawl.cd_dash" );
		m_cdUpper = field( "brawl.cd_upper" );
		m_cdSlam = field( "brawl.cd_slam" );
		m_block = schema.ActionMask( "use2" );
		m_join = schema.FindIntent( "teams.join" );
		m_teams = 0;
		if ( const BoardField* count = schema.FindField( "teams.count" ) )
		{
			m_teamsCount = FieldHandle{ count->slot, count->scope, count->type };
		}
		for ( const char* name : { "brawl.dash_strike.started", "brawl.uppercut.started", "brawl.slam.started", "sword.swing" } )
		{
			const int e = schema.FindEvent( name );
			if ( e >= 0 )
			{
				m_attacks.push_back( e );
			}
		}
		m_swing = schema.FindEvent( "sword.swing" );
		m_reaction = uint32_t( 18.0f - 12.0f * m_skill ); // ticks: 0.3 s to 0.1 s
	}

	void Tick( Context& ctx ) override
	{
		if ( m_fill <= 0 && m_brains.empty() )
		{
			return;
		}
		const uint32_t now = ctx.Tick();
		m_teams = m_teamsCount.Valid() ? ctx.Get( 0, m_teamsCount ) : 0;
		// Bots the server let go (none should be): forgotten.
		m_brains.erase( std::remove_if( m_brains.begin(), m_brains.end(), [&]( const Brain& b ) { return ctx.IsBot( b.slot ) == false; } ),
						m_brains.end() );
		if ( now % kSeatsEvery == 0 )
		{
			Seats( ctx );
		}
		// Who has just started an attack, this tick (whoever is close may guard or block).
		m_started.clear();
		for ( const ModEventRecord& e : ctx.RecentEvents() )
		{
			if ( std::find( m_attacks.begin(), m_attacks.end(), int( e.type ) ) != m_attacks.end() )
			{
				m_started.push_back( { e.netIdA, int( e.type ) == m_swing } );
			}
		}
		for ( Brain& b : m_brains )
		{
			ctx.BotInput( b.slot, Think( ctx, b, now ) );
		}
	}

private:
	struct Started
	{
		uint32_t who;
		bool swing;
	};

	int32_t TeamOf( Context& ctx, uint32_t netId ) const
	{
		return m_team.Valid() ? ctx.Get( netId, m_team ) : 0;
	}

	// Filling the game up to bots.fill, and the teams even.
	void Seats( Context& ctx )
	{
		int people = 0;
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			people += ctx.InWorld( PlayerSlot( i ) ) && ctx.IsBot( PlayerSlot( i ) ) == false ? 1 : 0;
		}
		const int wanted = std::max( 0, m_fill - people );
		// Team sizes, everyone's.
		std::array<int, 5> sizes{};
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			const uint32_t id = ctx.InWorld( PlayerSlot( i ) ) ? ctx.PlayerNetId( PlayerSlot( i ) ) : 0;
			const int32_t team = id != 0 ? TeamOf( ctx, id ) : 0;
			sizes[size_t( std::clamp( team, 0, 4 ) )] += team > 0 ? 1 : 0;
		}
		int big = 0, small = 0;
		for ( int t = 1; t <= std::min( m_teams, 4 ); ++t )
		{
			big = big == 0 || sizes[size_t( t )] > sizes[size_t( big )] ? t : big;
			small = small == 0 || sizes[size_t( t )] < sizes[size_t( small )] ? t : small;
		}
		auto inBig = [&]( const Brain& b ) {
			const uint32_t id = ctx.PlayerNetId( b.slot );
			return big > 0 && id != 0 && TeamOf( ctx, id ) == big;
		};
		if ( int( m_brains.size() ) > wanted )
		{
			// One leaves: from the bigger team if it has a bot.
			auto leaving = std::find_if( m_brains.begin(), m_brains.end(), inBig );
			leaving = leaving != m_brains.end() ? leaving : m_brains.end() - 1;
			ctx.RemoveBot( leaving->slot );
			m_brains.erase( leaving );
			return;
		}
		if ( int( m_brains.size() ) < wanted )
		{
			int number = 1;
			while ( std::any_of( m_brains.begin(), m_brains.end(), [&]( const Brain& b ) { return b.number == number; } ) )
			{
				++number;
			}
			const int slot = ctx.AddBot( "Bot " + std::to_string( number ) );
			if ( slot >= 0 )
			{
				Brain b;
				b.slot = PlayerSlot( slot );
				b.number = number;
				m_brains.push_back( b );
			}
			return;
		}
		// Even: a bot of a team two bigger than another moves over.
		if ( m_join >= 0 && big > 0 && small > 0 && sizes[size_t( big )] - sizes[size_t( small )] >= 2 )
		{
			auto moving = std::find_if( m_brains.begin(), m_brains.end(), inBig );
			if ( moving != m_brains.end() )
			{
				Intend( *moving, uint8_t( kFirstModIntent + m_join ), uint8_t( small ) );
			}
		}
	}

	void Intend( Brain& b, uint8_t intent, uint8_t a )
	{
		b.intent = intent;
		b.intentA = a;
		b.intents = uint8_t( b.intents + 1 );
	}

	float Random( Context& ctx )
	{
		return ctx.RandomRange( 0.0f, 1.0f );
	}

	// The slot an item of this kit part is in (-1: it has none).
	int SlotOf( Context& ctx, PlayerSlot player, int part ) const
	{
		if ( m_kinds[size_t( part )] < 0 )
		{
			return -1;
		}
		for ( int s = 0; s < ctx.SlotCount(); ++s )
		{
			const uint32_t item = ctx.SlotItem( player, s );
			if ( item != 0 && ctx.ItemKindOf( item ).index == m_kinds[size_t( part )] )
			{
				return s;
			}
		}
		return -1;
	}

	// The nearest player of another team who is up (hysteresis: the one it goes for unless another is
	// 3 m nearer).
	uint32_t Choose( Context& ctx, const Brain& b, uint32_t me, b3Vec3 at ) const
	{
		const int32_t mine = TeamOf( ctx, me );
		uint32_t best = 0;
		float bestDistance = 1e9f;
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			const PlayerSlot slot = PlayerSlot( i );
			const uint32_t id = slot != b.slot && ctx.InWorld( slot ) ? ctx.PlayerNetId( slot ) : 0;
			const Character* c = id != 0 ? ctx.PlayerCharacter( slot ) : nullptr;
			const Transform* t = c != nullptr ? ctx.EntityTransform( id ) : nullptr;
			if ( t == nullptr || c->dead != 0 || ctx.IsDown( id ) || ( mine != 0 && TeamOf( ctx, id ) == mine ) )
			{
				continue;
			}
			const float d = b3Distance( t->position, at ) - ( id == b.target ? 3.0f : 0.0f );
			if ( d < bestDistance )
			{
				bestDistance = d;
				best = id;
			}
		}
		return best;
	}

	PlayerInput Think( Context& ctx, Brain& b, uint32_t now )
	{
		PlayerInput in;
		in.intent = b.intent;
		in.intentA = b.intentA;
		in.intentSeq = b.intents;
		const uint32_t me = ctx.PlayerNetId( b.slot );
		const Character* self = me != 0 ? ctx.PlayerCharacter( b.slot ) : nullptr;
		const Transform* at = self != nullptr ? ctx.EntityTransform( me ) : nullptr;
		if ( at == nullptr || self->dead != 0 || ctx.IsDown( me ) )
		{
			b.wasDead = true;
			b.target = 0;
			return in;
		}
		if ( b.wasDead )
		{
			// A life: its weapon for it, and a fresh start.
			b.wasDead = false;
			b.sword = m_kinds[KitSword] >= 0 && Random( ctx ) < 0.34f;
			b.path.clear();
			b.was = at->position;
			b.wasAt = now;
		}

		// Its weapon in the hand: asked for (once a second at most) while another is.
		const int weapon = SlotOf( ctx, b.slot, b.sword ? KitSword : KitFist );
		if ( weapon >= 0 && ctx.SelectedSlot( b.slot ) != weapon && now >= b.asked + 60 )
		{
			b.asked = now;
			Intend( b, uint8_t( SlotIntent::Select ), uint8_t( weapon ) );
			in.intent = b.intent;
			in.intentA = b.intentA;
			in.intentSeq = b.intents;
		}

		if ( ( now + b.slot ) % kThinkEvery == 0 || b.target == 0 )
		{
			b.target = Choose( ctx, b, me, at->position );
			b.aimError = ( Random( ctx ) - 0.5f ) * ( 1.0f - m_skill ) * 0.5f;
		}
		const Transform* there = b.target != 0 ? ctx.EntityTransform( b.target ) : nullptr;
		if ( there == nullptr )
		{
			b.target = 0;
			return in;
		}
		const b3Vec3 to = b3Sub( there->position, at->position );
		const float flat = std::sqrt( to.x * to.x + to.z * to.z );
		const float distance = b3Length( to );

		// The way: straight at it when close, along the mesh otherwise (found again twice a second, or
		// when it has gone 3 m from where the way led).
		b3Vec3 goal = there->position;
		if ( flat > 4.0f )
		{
			if ( now >= b.repath || b3Distance( b.pathTo, there->position ) > 3.0f )
			{
				b.repath = now + 30;
				b.pathTo = there->position;
				b.next = 0;
				if ( ctx.FindPath( at->position, there->position, b.path ) == false )
				{
					b.path.clear();
				}
			}
			while ( b.next < b.path.size() &&
					std::hypot( b.path[b.next].x - at->position.x, b.path[b.next].z - at->position.z ) < 0.6f )
			{
				++b.next;
			}
			goal = b.next < b.path.size() ? b.path[b.next] : there->position;
		}
		// Facing: where it goes while far, at the target (and its error) when near; pitched at its chest.
		const bool close = flat < 6.0f;
		const b3Vec3 face = close ? to : b3Sub( goal, at->position );
		const float yaw = std::atan2( face.x, face.z ) + ( close ? b.aimError : 0.0f );
		in.cameraYaw = detmath::RadiansToYaw( yaw );
		const float pitch = close ? std::atan2( to.y, std::max( flat, 0.1f ) ) : 0.0f;
		in.cameraPitch = int16_t( std::clamp( pitch * 65536.0f / ( 2.0f * kPi ), -float( kMaxCameraPitch ), float( kMaxCameraPitch ) ) );

		// Moving: on until in reach (a step back when on top of it), running while far, a sidestep now
		// and then up close.
		in.moveForward = flat > kNear * 0.8f ? int8_t( 127 ) : flat < 0.9f ? int8_t( -80 ) : int8_t( 0 );
		in.buttons = flat > kRun ? uint8_t( BtnSprint ) : uint8_t( 0 );
		if ( close && now >= b.strafeUntil )
		{
			b.strafeUntil = now + 30 + uint32_t( Random( ctx ) * 60.0f );
			b.strafe = Random( ctx ) < 0.5f ? int8_t( 0 ) : Random( ctx ) < 0.5f ? int8_t( -90 ) : int8_t( 90 );
		}
		in.moveRight = close ? b.strafe : int8_t( 0 );

		// Stuck: hardly moved in a second while meaning to go: a jump, a sidestep, a new way.
		if ( now >= b.wasAt + 60 )
		{
			const bool meant = in.moveForward > 0 && flat > kNear;
			if ( meant && b3Distance( at->position, b.was ) < 0.8f )
			{
				b.jumpUntil = now + 4;
				b.strafe = Random( ctx ) < 0.5f ? int8_t( -127 ) : int8_t( 127 );
				b.strafeUntil = now + 40;
				b.repath = now;
			}
			b.was = at->position;
			b.wasAt = now;
		}
		if ( now < b.jumpUntil )
		{
			in.buttons |= BtnJump;
		}
		if ( now < b.strafeUntil && close == false )
		{
			in.moveRight = b.strafe;
		}

		Fight( ctx, b, in, now, flat, distance, to, *self );
		return in;
	}

	void Fight( Context& ctx, Brain& b, PlayerInput& in, uint32_t now, float flat, float distance, b3Vec3 to, const Character& self )
	{
		const uint32_t me = ctx.PlayerNetId( b.slot );
		// Someone close started an attack: it may guard (the fist) or block (the sword), after its
		// reaction time.
		for ( const Started& s : m_started )
		{
			const Transform* t = s.who != me ? ctx.EntityTransform( s.who ) : nullptr;
			if ( t == nullptr || b3Distance( t->position, ctx.EntityTransform( me )->position ) > 4.5f ||
				 ( TeamOf( ctx, s.who ) == TeamOf( ctx, me ) && TeamOf( ctx, me ) != 0 ) )
			{
				continue;
			}
			const float chance = 0.25f + 0.4f * m_skill;
			if ( b.sword && s.swing && now >= b.blockUntil && Random( ctx ) < chance )
			{
				b.blockFrom = now + m_reaction / 2;
				b.blockUntil = b.blockFrom + 36;
			}
			else if ( b.sword == false && now >= b.guardUntil && Random( ctx ) < chance * 0.6f )
			{
				b.guardFrom = now + m_reaction;
				b.guardUntil = b.guardFrom + 40;
			}
		}
		const bool blocking = now >= b.blockFrom && now < b.blockUntil;
		const bool guarding = now >= b.guardFrom && now < b.guardUntil;
		if ( blocking )
		{
			in.actions |= m_block;
		}
		const int guard = SlotOf( ctx, b.slot, KitGuard );
		if ( guarding && guard >= 0 )
		{
			in.slotKeys |= uint16_t( 1u << guard );
		}

		// A key held for a few ticks (a skill), the button held for three (a click).
		if ( now < b.keyUntil )
		{
			in.slotKeys |= b.key;
		}
		if ( now < b.clickUntil )
		{
			in.buttons |= BtnUse;
		}
		if ( blocking || guarding || now < b.keyUntil || now < b.clickUntil )
		{
			return;
		}
		auto press = [&]( int part ) {
			const int slot = SlotOf( ctx, b.slot, part );
			if ( slot < 0 )
			{
				return false;
			}
			b.key = uint16_t( 1u << slot );
			b.keyUntil = now + 4;
			in.slotKeys |= b.key;
			return true;
		};
		auto ready = [&]( FieldHandle cooldown ) { return cooldown.Valid() && ctx.GetFloat( me, cooldown ) <= 0.0f; };
		const bool decide = ( now + b.slot ) % 6 == 0;
		const float eager = 0.3f + 0.5f * m_skill;

		if ( b.sword )
		{
			if ( distance < kNear + 0.4f && now >= b.nextClick )
			{
				b.clickUntil = now + 3;
				b.nextClick = now + 66 + uint32_t( ( 1.0f - m_skill ) * 30.0f * Random( ctx ) );
				in.buttons |= BtnUse;
			}
			return;
		}
		// The fist and its kit.
		if ( decide && self.grounded == 0 && to.y < -0.8f && flat < 3.5f && ready( m_cdSlam ) && Random( ctx ) < eager )
		{
			press( KitSlam ); // from the air, onto it
			return;
		}
		if ( decide && flat > 4.0f && flat < 7.5f && std::fabs( to.y ) < 1.5f && ready( m_cdDash ) && Random( ctx ) < eager * 0.5f )
		{
			press( KitDash );
			return;
		}
		if ( decide && distance < kNear && self.grounded != 0 && ready( m_cdUpper ) && Random( ctx ) < eager * 0.15f )
		{
			press( KitUpper );
			return;
		}
		if ( distance < kNear + 0.3f && now >= b.nextClick )
		{
			// A jab every 0.4 s or so (the third of a run the finisher).
			b.clickUntil = now + 3;
			b.nextClick = now + 22 + uint32_t( ( 1.0f - m_skill ) * 20.0f * Random( ctx ) );
			in.buttons |= BtnUse;
		}
	}

	std::vector<Brain> m_brains;
	std::vector<Started> m_started;
	std::array<int, KitCount> m_kinds{};
	std::vector<int> m_attacks;
	FieldHandle m_team;
	FieldHandle m_teamsCount;
	FieldHandle m_cdDash, m_cdUpper, m_cdSlam;
	ActionBits m_block = 0;
	int m_join = -1;
	int m_swing = -1;
	int m_teams = 0;
	int m_fill = 0;
	float m_skill = 0.5f;
	uint32_t m_reaction = 12;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_bots()
{
	return std::make_unique<BotsMod>();
}
