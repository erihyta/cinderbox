#include "rollback.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace cb
{

namespace
{
constexpr uint32_t kNone = UINT32_MAX;
constexpr size_t kConfirmedHashHistory = 512;

// Adds the time it lives for to a counter.
struct Timed
{
	Timed( uint64_t& count, uint64_t& ns )
		: m_ns( ns )
		, m_start( std::chrono::steady_clock::now() )
	{
		count += 1;
	}
	~Timed()
	{
		m_ns += uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now() - m_start ).count() );
	}
	uint64_t& m_ns;
	std::chrono::steady_clock::time_point m_start;
};
} // namespace

RollbackSession::RollbackSession( const SimConfig& config, PlayerSlot localSlot, uint32_t maxRollbackTicks,
								  uint32_t maxRollbackCap, const LevelLayout& map )
	: m_config( config )
	, m_localSlot( localSlot )
	, m_maxRollback( std::max<uint32_t>( maxRollbackTicks, 1 ) )
	, m_maxRollbackCap( std::max( maxRollbackCap, m_maxRollback ) )
{
	m_sim = std::make_unique<Simulation>( config, map );
	// The largest window, plus the same again of history for finalizing hashes, plus slack (and
	// the ticks a fix may wait).
	m_records.resize( 2 * size_t( m_maxRollbackCap ) + 4 + kMaxReconcileEvery );
	m_confirmedHashes.resize( kConfirmedHashHistory );

	m_finalized = m_sim->Tick();
	m_confirmedHashes[m_finalized % kConfirmedHashHistory] = { m_finalized, m_sim->ComputeHash() };
}

void RollbackSession::Reset( const Snapshot& snapshot, const std::array<PlayerInput, kMaxPlayers>& previousInputs )
{
	m_sim->Load( snapshot );
	for ( TickRecord& r : m_records )
	{
		r.tick = kNone;
		r.hasSnapshot = false;
		r.hasHash = false;
	}
	for ( ConfirmedHash& h : m_confirmedHashes )
	{
		h.tick = kNone;
	}
	m_authFrames.clear();
	m_authBase = snapshot.tick;
	m_confirmedEnd = snapshot.tick;
	if ( snapshot.tick > 0 )
	{
		// Stand-in for the frame before the snapshot, so prediction repeats real inputs.
		InputFrame previous;
		previous.tick = snapshot.tick - 1;
		previous.inputs = previousInputs;
		m_authFrames.push_back( previous );
		m_authBase = previous.tick;
	}
	m_rollbackFrom = kNone;
	m_reconciledAt = snapshot.tick;
	m_finalized = snapshot.tick;
	m_confirmedHashes[m_finalized % kConfirmedHashHistory] = { m_finalized, snapshot.hash };
}

void RollbackSession::SetMaxRollback( uint32_t ticks )
{
	m_maxRollback = std::clamp<uint32_t>( ticks, 1, m_maxRollbackCap );
}

void RollbackSession::SetReconcileEvery( uint32_t ticks )
{
	m_reconcileEvery = std::clamp<uint32_t>( ticks, 1, kMaxReconcileEvery );
}

RollbackSession::TickRecord& RollbackSession::Record( uint32_t tick )
{
	return m_records[tick % m_records.size()];
}

const InputFrame* RollbackSession::FindAuthoritative( uint32_t tick ) const
{
	if ( tick < m_authBase || tick - m_authBase >= m_authFrames.size() )
	{
		return nullptr;
	}
	return &m_authFrames[tick - m_authBase];
}

void RollbackSession::AddAuthoritativeFrame( const InputFrame& frame )
{
	if ( frame.tick != m_confirmedEnd )
	{
		return;
	}

	if ( m_authFrames.empty() )
	{
		m_authBase = frame.tick;
	}
	m_authFrames.push_back( frame );
	m_confirmedEnd += 1;

	// Already simulated with a prediction? Then check it.
	if ( frame.tick < m_sim->Tick() )
	{
		const TickRecord& r = Record( frame.tick );
		if ( r.tick != frame.tick || !( r.used == frame ) )
		{
			m_rollbackFrom = std::min( m_rollbackFrom, frame.tick );
		}
	}
}

InputFrame RollbackSession::BuildFrame( uint32_t tick, const PlayerInput& localInput ) const
{
	if ( const InputFrame* auth = FindAuthoritative( tick ) )
	{
		return *auth;
	}

	// Predict: everyone repeats their last authoritative input, no join/leave events.
	InputFrame frame;
	if ( m_confirmedEnd > 0 )
	{
		if ( const InputFrame* last = FindAuthoritative( m_confirmedEnd - 1 ) )
		{
			frame.inputs = last->inputs;
		}
	}
	frame.tick = tick;
	frame.inputs[m_localSlot] = localInput;
	return frame;
}

void RollbackSession::SimulateTick( const InputFrame& frame, Keep keep )
{
	uint32_t tick = m_sim->Tick();
	TickRecord& r = Record( tick );

	if ( keep != Keep::Start )
	{
		r.tick = tick;
		r.hasSnapshot = false;
		r.hasHash = false;
		// A snapshot where a re-simulation may have to start: before a tick predicted for the first
		// time, and, in a re-simulation, where the confirmed frames end (the one after them is the
		// next that can turn out wrong). Not before a tick that is about to be re-simulated anyway.
		bool snapshot = keep == Keep::Predicting ? tick >= m_confirmedEnd && m_rollbackFrom == kNone : tick == m_confirmedEnd;
		if ( snapshot )
		{
			{
				Timed timed( m_stats.saves, m_stats.saveNs );
				m_sim->Save( r.snapshot );
			}
			r.hasSnapshot = true;
			r.hasHash = true;
			r.hashBefore = r.snapshot.hash;
		}
		else if ( tick < m_confirmedEnd )
		{
			// A confirmed state: its hash is what the server's checksums are compared with.
			Timed timed( m_stats.hashes, m_stats.hashNs );
			r.hashBefore = m_sim->ComputeHash();
			r.hasHash = true;
		}
	}

	r.used = frame;
	Timed timed( m_stats.steps, m_stats.stepNs );
	m_sim->Step( frame );
}

void RollbackSession::Reconcile()
{
	// (A fix may wait a few ticks: SetReconcileEvery.)
	uint32_t now = m_sim->Tick();
	if ( m_rollbackFrom != kNone && ( m_reconcileEvery <= 1 || now < m_reconciledAt || now - m_reconciledAt >= m_reconcileEvery ) )
	{
		uint32_t to = now;
		// The nearest snapshot at or before the first wrong tick. Every state up to that tick was
		// simulated with what the server then confirmed, so any snapshot there still holds.
		uint32_t from = m_rollbackFrom;
		for ( size_t back = 0; back < m_records.size(); ++back, --from )
		{
			const TickRecord& r = Record( from );
			if ( ( r.tick == from && r.hasSnapshot ) || from == 0 )
			{
				break;
			}
		}
		TickRecord& start = Record( from );

		// The window guarantees one exists; anything else is a logic error.
		if ( start.tick != from || start.hasSnapshot == false )
		{
			std::fprintf( stderr, "RollbackSession: no snapshot at or before tick %u\n", m_rollbackFrom );
			std::abort();
		}

		{
			Timed timed( m_stats.loads, m_stats.loadNs );
			m_sim->Load( start.snapshot );
		}
		m_rollbackFrom = kNone;
		for ( uint32_t t = from; t < to; ++t )
		{
			// Keep the local input that was actually sampled for this tick.
			PlayerInput local = Record( t ).used.inputs[m_localSlot];
			SimulateTick( BuildFrame( t, local ), t == from ? Keep::Start : Keep::Resimulated );
		}

		m_stats.rollbacks += 1;
		m_stats.resimulatedTicks += to - from;
		m_stats.lastRollbackDepth = to - from;
		m_reconciledAt = to;
	}

	FinalizeStates();
	TrimHistory();
}

bool RollbackSession::CanAdvance() const
{
	return m_sim->Tick() < m_confirmedEnd + m_maxRollback;
}

bool RollbackSession::AdvanceOne( const PlayerInput& localInput )
{
	if ( m_rollbackFrom != kNone )
	{
		Reconcile(); // (does nothing while the fix waits)
	}

	if ( CanAdvance() == false )
	{
		m_stats.stalls += 1;
		return false;
	}

	SimulateTick( BuildFrame( m_sim->Tick(), localInput ), Keep::Predicting );
	FinalizeStates();
	TrimHistory();
	return true;
}

void RollbackSession::FinalizeStates()
{
	uint32_t current = m_sim->Tick();
	uint32_t limit = std::min( m_confirmedEnd, current );
	if ( m_rollbackFrom != kNone )
	{
		limit = std::min( limit, m_rollbackFrom );
	}

	while ( m_finalized < limit )
	{
		uint32_t s = m_finalized + 1;
		uint64_t hash;
		if ( s == current )
		{
			hash = m_sim->ComputeHash();
		}
		else
		{
			const TickRecord& r = Record( s );
			if ( r.tick != s )
			{
				std::fprintf( stderr, "RollbackSession: missing record for tick %u\n", s );
				std::abort();
			}
			if ( r.hasHash == false )
			{
				// Predicted in a re-simulation and then confirmed as predicted: the state was right,
				// and was not hashed. A checksum for this tick goes unchecked.
				m_finalized = s;
				continue;
			}
			hash = r.hashBefore;
		}
		m_confirmedHashes[s % kConfirmedHashHistory] = { s, hash };
		m_finalized = s;
	}
}

void RollbackSession::TrimHistory()
{
	// Keep authoritative frames that a resimulation could still read.
	uint32_t keepFrom = m_sim->Tick() > m_records.size() ? m_sim->Tick() - uint32_t( m_records.size() ) : 0;
	keepFrom = std::min( keepFrom, m_rollbackFrom );
	while ( !m_authFrames.empty() && m_authBase < keepFrom && m_authFrames.size() > 1 )
	{
		m_authFrames.pop_front();
		m_authBase += 1;
	}
}

const InputFrame* RollbackSession::LastSimulatedFrame() const
{
	uint32_t tick = m_sim->Tick();
	if ( tick == 0 )
	{
		return nullptr;
	}
	const TickRecord& r = m_records[( tick - 1 ) % m_records.size()];
	return r.tick == tick - 1 ? &r.used : nullptr;
}

bool RollbackSession::GetConfirmedHash( uint32_t tick, uint64_t& hash ) const
{
	const ConfirmedHash& h = m_confirmedHashes[tick % kConfirmedHashHistory];
	if ( h.tick != tick )
	{
		return false;
	}
	hash = h.hash;
	return true;
}

} // namespace cb
