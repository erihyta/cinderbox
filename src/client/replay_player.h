#pragma once

// A server recording (cb_server --record), re-simulated: play, seek, and check the recorded
// checksums on the way. No rendering and no clock of its own; replay_source.h drives it.

#include "replay.h"
#include "simulation.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace cb
{

class ReplayPlayer
{
public:
	bool Open( const std::string& path, std::string& error );

	const net::ReplayReader& Reader() const
	{
		return m_reader;
	}
	Simulation& Sim()
	{
		return *m_sim;
	}
	// The tick the next step simulates; Length() at the end.
	uint32_t Tick() const
	{
		return m_sim->Tick();
	}
	uint32_t Length() const
	{
		return uint32_t( m_reader.Frames().size() );
	}
	// Changes whenever the world jumped (a seek), so presentation snaps instead of smoothing.
	uint64_t Generation() const
	{
		return m_generation;
	}
	uint64_t ChecksumFailures() const
	{
		return m_checksumFailures;
	}
	uint64_t ChecksumsVerified() const
	{
		return m_checksumsVerified;
	}

	// Advances playback by `seconds`; returns the interpolation factor toward the next tick.
	// `onStep( frame )` is called with each frame it simulates.
	template <typename Fn>
	float Advance( double seconds, Fn&& onStep )
	{
		double dt = 1.0 / double( m_reader.Config().tickRate );
		m_accumulator += seconds;
		int steps = 0;
		while ( m_accumulator >= dt && Tick() < Length() && steps < 64 )
		{
			onStep( m_reader.Frames()[Tick()] );
			StepOne();
			m_accumulator -= dt;
			++steps;
		}
		if ( Tick() >= Length() )
		{
			m_accumulator = 0.0;
		}
		m_accumulator = m_accumulator < dt ? m_accumulator : dt;
		return float( m_accumulator / dt );
	}
	float Advance( double seconds )
	{
		return Advance( seconds, []( const InputFrame& ) {} );
	}

	void SeekTo( uint32_t target );

	// The next active player slot after `from` (-1: the first), or -1 if there is none.
	int NextActiveSlot( int from ) const;

private:
	void StepOne();
	void CaptureKeyframe();
	void VerifyChecksum();

	net::ReplayReader m_reader;
	std::unique_ptr<Simulation> m_sim;
	std::map<uint32_t, Snapshot> m_keyframes;
	double m_accumulator = 0.0;
	uint64_t m_generation = 1;
	uint64_t m_checksumsVerified = 0;
	uint64_t m_checksumFailures = 0;
	uint32_t m_highestVerified = 0;
};

} // namespace cb
