#include "rollback.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace cb
{

namespace
{
constexpr uint32_t kNone = UINT32_MAX;
constexpr size_t kConfirmedHashHistory = 512;
} // namespace

RollbackSession::RollbackSession( const SimConfig& config, PlayerSlot localSlot, uint32_t maxRollbackTicks,
								  uint32_t maxRollbackCap, const LevelLayout& map )
	: m_config( config )
	, m_localSlot( localSlot )
	, m_maxRollback( std::max<uint32_t>( maxRollbackTicks, 1 ) )
	, m_maxRollbackCap( std::max( maxRollbackCap, m_maxRollback ) )
{
	m_sim = std::make_unique<Simulation>( config, map );
	// The largest window, plus the same again of history for finalizing hashes, plus slack.
	m_records.resize( 2 * size_t( m_maxRollbackCap ) + 4 );
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
	m_finalized = snapshot.tick;
	m_confirmedHashes[m_finalized % kConfirmedHashHistory] = { m_finalized, snapshot.hash };
}

void RollbackSession::SetMaxRollback( uint32_t ticks )
{
	m_maxRollback = std::clamp<uint32_t>( ticks, 1, m_maxRollbackCap );
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

void RollbackSession::SimulateTick( const InputFrame& frame, bool keepExistingSnapshot )
{
	uint32_t tick = m_sim->Tick();
	TickRecord& r = Record( tick );

	if ( keepExistingSnapshot == false )
	{
		r.tick = tick;
		// Only states that might still be rolled back need a full snapshot.
		if ( tick >= m_confirmedEnd )
		{
			m_sim->Save( r.snapshot );
			r.hasSnapshot = true;
			r.hashBefore = r.snapshot.hash;
		}
		else
		{
			r.hasSnapshot = false;
			r.hashBefore = m_sim->ComputeHash();
		}
	}

	r.used = frame;
	m_sim->Step( frame );
}

void RollbackSession::Reconcile()
{
	if ( m_rollbackFrom != kNone )
	{
		uint32_t from = m_rollbackFrom;
		uint32_t to = m_sim->Tick();
		TickRecord& start = Record( from );

		// The window guarantees this snapshot exists; anything else is a logic error.
		if ( start.tick != from || start.hasSnapshot == false )
		{
			std::fprintf( stderr, "RollbackSession: missing snapshot for tick %u\n", from );
			std::abort();
		}

		m_sim->Load( start.snapshot );
		for ( uint32_t t = from; t < to; ++t )
		{
			// Keep the local input that was actually sampled for this tick.
			PlayerInput local = Record( t ).used.inputs[m_localSlot];
			SimulateTick( BuildFrame( t, local ), t == from );
		}

		m_stats.rollbacks += 1;
		m_stats.resimulatedTicks += to - from;
		m_stats.lastRollbackDepth = to - from;
		m_rollbackFrom = kNone;
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
		Reconcile();
	}

	if ( CanAdvance() == false )
	{
		m_stats.stalls += 1;
		return false;
	}

	SimulateTick( BuildFrame( m_sim->Tick(), localInput ), false );
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
