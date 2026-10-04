#include "replay_player.h"

#include <algorithm>
#include <cstdio>

namespace cb
{

namespace
{
constexpr uint32_t kKeyframeInterval = 300;
} // namespace

bool ReplayPlayer::Open( const std::string& path, std::string& error )
{
	if ( m_reader.Open( path, error ) == false )
	{
		return false;
	}
	m_sim = std::make_unique<Simulation>( m_reader.Config(), m_reader.Map() );
	m_sim->SetAnimGraph( m_reader.Graph() );
	m_sim->SetAnimPacks( m_reader.Packs() );
	m_sim->SetMotions( m_reader.MotionSets() );
	m_sim->SetItemShapes( m_reader.Schema().itemShapes );
	CaptureKeyframe();
	return true;
}

void ReplayPlayer::SeekTo( uint32_t target )
{
	target = std::min( target, Length() );
	if ( target < Tick() )
	{
		auto it = m_keyframes.upper_bound( target );
		--it; // tick 0 is always there
		m_sim->Load( it->second );
	}
	while ( Tick() < target )
	{
		StepOne();
	}
	m_accumulator = 0.0;
	m_generation += 1;
}

int ReplayPlayer::NextActiveSlot( int from ) const
{
	for ( int i = 1; i <= kMaxPlayers; ++i )
	{
		int slot = ( from + i + kMaxPlayers ) % kMaxPlayers;
		if ( m_sim->IsPlayerActive( PlayerSlot( slot ) ) )
		{
			return slot;
		}
	}
	return -1;
}

void ReplayPlayer::StepOne()
{
	m_sim->Step( m_reader.Frames()[Tick()] );
	CaptureKeyframe();
	VerifyChecksum();
}

void ReplayPlayer::CaptureKeyframe()
{
	uint32_t tick = Tick();
	if ( tick % kKeyframeInterval == 0 && m_keyframes.count( tick ) == 0 )
	{
		m_sim->Save( m_keyframes[tick] );
	}
}

void ReplayPlayer::VerifyChecksum()
{
	// Checksums are sorted by tick; only check each once (seeking replays ticks).
	const auto& list = m_reader.Checksums();
	auto it = std::lower_bound( list.begin(), list.end(), Tick(), []( const net::MsgChecksum& c, uint32_t t ) { return c.tick < t; } );
	if ( it != list.end() && it->tick == Tick() && Tick() > m_highestVerified )
	{
		m_highestVerified = Tick();
		if ( m_sim->ComputeHash() == it->hash )
		{
			m_checksumsVerified += 1;
		}
		else
		{
			m_checksumFailures += 1;
			std::printf( "replay: checksum mismatch at tick %u\n", Tick() );
		}
	}
}

} // namespace cb
