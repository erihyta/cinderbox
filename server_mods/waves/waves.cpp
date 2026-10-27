// Waves: NPCs come in waves that grow (ROADMAP.md, step 10). Off by default:
//
//   cb_server --mods <the others>,brawl,waves
//
// Each wave's NPCs stand up on a ring around the map's spawn, are given a weapon (the brawl fist),
// find their way to the nearest living player on the server's navigation mesh and hit it once they
// are near. When the whole wave is down, the next one comes after a pause, bigger. An NPC is a body
// this mod drives (Context::Drive): every simulation runs its input, so it moves, swings and is hit
// as a player is. The combat mod hurts it, kills it and credits the kill; this mod clears its dead.
//
// Options (cb_server --mod-option NAME=VALUE):
//   waves.first          NPCs in the first wave (3)
//   waves.grow           more in each wave after it (2)
//   waves.most           the most in one wave (24)
//   waves.pause_seconds  between a wave's end and the next (4)
//   waves.ring           how far from the spawn they stand up, in metres (14)
//   waves.reach          how near they come before they swing, in metres (1.6)
//   waves.slot           the hotbar slot their weapon goes in: the one its key selects (1)
//
// Fields: waves.wave (global: the wave on), waves.left (global: its NPCs still standing).
// Events: waves.started, waves.cleared (value: the wave).

#include "mod_api.h"
#include "detmath.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{

using namespace cb;
using namespace cb::mods;

constexpr float kTwoPi = 6.2831853f;

class WavesMod final : public ServerMod
{
public:
	const char* Name() const override
	{
		return "waves";
	}

	void Declare( Declarations& declare ) override
	{
		m_wave = declare.Field( "waves.wave", BoardType::Int, BoardScope::Global );
		m_left = declare.Field( "waves.left", BoardType::Int, BoardScope::Global );
		m_started = declare.Event( "waves.started" );
		m_cleared = declare.Event( "waves.cleared" );
	}

	void Start( Context& ctx ) override
	{
		m_rate = ctx.Config().tickRate;
		m_first = std::max( 1, int( ctx.Option( "waves.first", 3.0 ) ) );
		m_grow = std::max( 0, int( ctx.Option( "waves.grow", 2.0 ) ) );
		m_most = std::max( 1, int( ctx.Option( "waves.most", 24.0 ) ) );
		m_pauseTicks = uint32_t( std::max( 0.0, ctx.Option( "waves.pause_seconds", 4.0 ) ) * m_rate );
		m_ring = float( std::max( 1.0, ctx.Option( "waves.ring", 14.0 ) ) );
		m_reach = float( std::max( 0.5, ctx.Option( "waves.reach", 1.6 ) ) );
		m_slotKey = uint16_t( 1u << std::clamp( int( ctx.Option( "waves.slot", 1.0 ) ) - 1, 0, 15 ) );
		m_weapon = ItemKindHandle{ ctx.Schema().FindItemKind( "brawl.fist" ) };
		m_nextWave = ctx.Tick() + m_pauseTicks;
	}

	void Tick( Context& ctx ) override
	{
		const uint32_t now = ctx.Tick();
		Claim( ctx, now );
		// Gone (a round's clear-out, say): forgotten. Dead a while: cleared away.
		std::erase_if( m_npcs, [&]( Npc& npc ) {
			// (Its Drive takes a tick or so to reach the simulation; until then it is a body nobody
			// drives, with no Character.)
			const Character* c = ctx.BodyCharacter( npc.netId );
			if ( c == nullptr || ctx.DrivenByMod( npc.netId ) == false )
			{
				return now > npc.since + 10;
			}
			if ( c->dead == 0 )
			{
				npc.deadSince = 0; // (brought back by someone)
				return false;
			}
			npc.deadSince = npc.deadSince == 0 ? now : npc.deadSince;
			if ( now - npc.deadSince > 2 * m_rate )
			{
				ctx.StopDriving( npc.netId ); // (dead, it goes)
				return true;
			}
			return false;
		} );
		int standing = int( m_spots.size() );
		for ( Npc& npc : m_npcs )
		{
			if ( npc.deadSince == 0 )
			{
				++standing;
				if ( ctx.DrivenByMod( npc.netId ) )
				{
					Think( ctx, npc, now );
				}
			}
		}
		ctx.Set( 0, m_left, standing );

		if ( standing > 0 )
		{
			m_fighting = true;
			m_nextWave = now + m_pauseTicks;
			return;
		}
		if ( m_fighting )
		{
			m_fighting = false;
			ctx.Emit( m_cleared, 0, 0, m_number );
		}
		if ( now >= m_nextWave && AnyPlayer( ctx ) )
		{
			StartWave( ctx, now );
		}
	}

private:
	struct Npc
	{
		uint32_t netId = 0;
		uint32_t since = 0;		// the tick it was claimed
		uint32_t deadSince = 0; // 0: standing
		uint32_t repath = 0;	// the tick its way is found again
		uint32_t target = 0;
		std::vector<b3Vec3> corners;
		size_t next = 0;
	};
	// Where a body was asked for, until it stands there.
	struct Spot
	{
		b3Vec3 feet;
		uint32_t until = 0;
	};

	bool AnyPlayer( Context& ctx ) const
	{
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			if ( ctx.InWorld( PlayerSlot( i ) ) )
			{
				return true;
			}
		}
		return false;
	}

	void StartWave( Context& ctx, uint32_t now )
	{
		++m_number;
		const int count = std::min( m_most, m_first + m_grow * ( m_number - 1 ) );
		// The bodies nobody drives already: none of them is this wave's (a body a player left near
		// a spot is not taken for an NPC).
		m_before = ctx.IdleBodies();
		const b3Vec3 center = GetLevelLayout().spawnCenter;
		for ( int i = 0; i < count; ++i )
		{
			// Around the ring, each wave turned a little from the last.
			const float angle = kTwoPi * ( float( i ) / float( count ) + 0.13f * float( m_number ) );
			b3Vec3 feet = { center.x + m_ring * std::sin( angle ), center.y, center.z + m_ring * std::cos( angle ) };
			b3Vec3 walkable;
			if ( ctx.NearestWalkable( feet, walkable ) )
			{
				feet = walkable;
			}
			feet.y += 0.05f;
			ctx.SpawnCharacter( CharacterHandle{ 0 }, feet, angle + 3.1416f ); // (facing in)
			m_spots.push_back( { feet, now + m_rate } );
		}
		ctx.Set( 0, m_wave, m_number );
		ctx.Emit( m_started, 0, 0, m_number );
	}

	// The bodies its waves stood up (nobody's, where it asked for one) become its NPCs.
	void Claim( Context& ctx, uint32_t now )
	{
		if ( m_spots.empty() )
		{
			return;
		}
		for ( uint32_t body : ctx.IdleBodies() )
		{
			if ( std::find( m_before.begin(), m_before.end(), body ) != m_before.end() )
			{
				continue;
			}
			const Transform* t = ctx.EntityTransform( body );
			for ( size_t i = 0; t != nullptr && i < m_spots.size(); ++i )
			{
				const float dx = t->position.x - m_spots[i].feet.x, dz = t->position.z - m_spots[i].feet.z;
				if ( dx * dx + dz * dz < 0.25f )
				{
					m_spots.erase( m_spots.begin() + std::ptrdiff_t( i ) );
					ctx.Drive( body, PlayerInput{} );
					if ( m_weapon.Valid() )
					{
						ctx.GiveItem( body, m_weapon );
					}
					Npc npc;
					npc.netId = body;
					npc.since = now;
					m_npcs.push_back( std::move( npc ) );
					break;
				}
			}
		}
		std::erase_if( m_spots, [&]( const Spot& s ) { return now > s.until; } );
	}

	// The nearest living player.
	uint32_t Nearest( Context& ctx, b3Vec3 from, b3Vec3& at ) const
	{
		uint32_t best = 0;
		float bestDistance = 1e30f;
		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			const PlayerSlot slot = PlayerSlot( i );
			const Character* c = ctx.InWorld( slot ) ? ctx.PlayerCharacter( slot ) : nullptr;
			const uint32_t netId = c != nullptr && c->dead == 0 ? ctx.PlayerNetId( slot ) : 0;
			const Transform* t = netId != 0 ? ctx.EntityTransform( netId ) : nullptr;
			if ( t == nullptr )
			{
				continue;
			}
			const float d = b3Length( t->position - from );
			if ( d < bestDistance )
			{
				bestDistance = d;
				best = netId;
				at = t->position;
			}
		}
		return best;
	}

	void Think( Context& ctx, Npc& npc, uint32_t now )
	{
		const Transform* me = ctx.EntityTransform( npc.netId );
		// Its weapon out: its slot's key, held (one press) once the weapon is surely there.
		const uint16_t keys = now > npc.since + 5 ? m_slotKey : 0;
		b3Vec3 target{};
		const uint32_t who = me != nullptr ? Nearest( ctx, me->position, target ) : 0;
		PlayerInput in;
		in.slotKeys = keys;
		if ( who == 0 )
		{
			ctx.Drive( npc.netId, in );
			return;
		}
		const float dx = target.x - me->position.x, dz = target.z - me->position.z;
		if ( dx * dx + dz * dz <= m_reach * m_reach )
		{
			// Near: it stands, faces it and swings, twice a second.
			in.cameraYaw = detmath::RadiansToYaw( detmath::Atan2( dx, dz ) );
			in.buttons = ( now - npc.since ) % std::max( 1u, m_rate / 2 ) < 2 ? uint8_t( BtnUse ) : uint8_t( 0 );
			ctx.Drive( npc.netId, in );
			return;
		}
		// Its way there, found again twice a second, or at once for another player.
		if ( now >= npc.repath || who != npc.target )
		{
			npc.target = who;
			npc.repath = now + m_rate / 2;
			npc.next = 0;
			if ( ctx.FindPath( me->position, target, npc.corners ) == false )
			{
				npc.corners.clear();
			}
		}
		while ( npc.next < npc.corners.size() &&
				b3Length( b3Vec3{ npc.corners[npc.next].x - me->position.x, 0.0f, npc.corners[npc.next].z - me->position.z } ) < 0.4f )
		{
			++npc.next;
		}
		// Past the way's end (or no way on the mesh): straight at it.
		const b3Vec3 toward = npc.next < npc.corners.size() ? npc.corners[npc.next] : target;
		in = ctx.WalkToward( npc.netId, toward, 0.05f );
		in.slotKeys = keys;
		ctx.Drive( npc.netId, in );
	}

	FieldHandle m_wave;
	FieldHandle m_left;
	EventHandle m_started;
	EventHandle m_cleared;
	ItemKindHandle m_weapon;
	std::vector<Npc> m_npcs;
	std::vector<Spot> m_spots;
	std::vector<uint32_t> m_before; // the bodies nobody drove when the last wave was asked for
	uint32_t m_rate = 60;
	uint32_t m_nextWave = 0;
	uint32_t m_pauseTicks = 0;
	int m_number = 0;
	int m_first = 3;
	int m_grow = 2;
	int m_most = 24;
	float m_ring = 14.0f;
	float m_reach = 1.6f;
	uint16_t m_slotKey = 1;
	bool m_fighting = false;
};

} // namespace

std::unique_ptr<cb::mods::ServerMod> CreateMod_waves()
{
	return std::make_unique<WavesMod>();
}
