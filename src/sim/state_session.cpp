#include "state_session.h"

#include <algorithm>
#include <chrono>

namespace cb
{

namespace
{
constexpr uint32_t kMaxAheadCap = 120;

uint64_t NsSince( std::chrono::steady_clock::time_point start )
{
	return uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now() - start ).count() );
}
} // namespace

StateSession::StateSession( const SimConfig& config, PlayerSlot localSlot, uint32_t maxAhead, const LevelLayout& map )
	: m_config( config )
	, m_localSlot( localSlot )
	, m_maxAhead( std::clamp<uint32_t>( maxAhead, 1, kMaxAheadCap ) )
{
	m_sim = std::make_unique<Simulation>( config, map );
	m_sampled.resize( kMaxAheadCap + 8 );
	m_last.tick = UINT32_MAX;
}

void StateSession::SetMaxAhead( uint32_t ticks )
{
	m_maxAhead = std::clamp<uint32_t>( ticks, 1, kMaxAheadCap );
}

void StateSession::AddState( uint32_t tick, const uint8_t* data, size_t size, const std::array<PlayerInput, kMaxPlayers>& inputs )
{
	if ( ( m_hasState && tick <= m_stateTick ) || ( m_hasPending && tick <= m_pendingTick ) )
	{
		return; // (overtaken on the way)
	}
	if ( m_hasPending )
	{
		m_stats.skipped += 1;
	}
	m_pending.assign( data, data + size );
	m_pendingTick = tick;
	m_pendingInputs = inputs;
	m_hasPending = true;
}

void StateSession::Reconcile()
{
	if ( m_hasPending == false )
	{
		return;
	}
	m_hasPending = false;
	const uint32_t reached = m_sim->Tick();

	auto start = std::chrono::steady_clock::now();
	const bool applied = m_sim->ApplyState( m_pending.data(), m_pending.size(), m_localSlot );
	m_stats.applyNs += NsSince( start );
	if ( applied == false || m_sim->Tick() != m_pendingTick )
	{
		m_stats.rejected += 1;
		return;
	}
	m_stats.states += 1;
	m_hasState = true;
	m_stateTick = m_pendingTick;
	m_others = m_pendingInputs;

	// The local inputs since the state's tick, again (each as it was sampled).
	uint32_t depth = 0;
	for ( uint32_t t = m_stateTick; t < reached; ++t )
	{
		const Sampled& s = m_sampled[t % m_sampled.size()];
		if ( s.tick != t )
		{
			break; // (never sampled: a state from before this session predicted)
		}
		Simulate( s.input );
		depth += 1;
	}
	m_stats.resimulatedTicks += depth;
	m_stats.lastDepth = depth;
}

bool StateSession::CanAdvance() const
{
	return m_hasState && m_sim->Tick() < m_stateTick + m_maxAhead;
}

bool StateSession::AdvanceOne( const PlayerInput& localInput )
{
	if ( m_hasPending )
	{
		Reconcile();
	}
	if ( CanAdvance() == false )
	{
		m_stats.stalls += 1;
		return false;
	}
	const uint32_t tick = m_sim->Tick();
	m_sampled[tick % m_sampled.size()] = { tick, localInput };
	Simulate( localInput );
	return true;
}

void StateSession::Simulate( const PlayerInput& localInput )
{
	InputFrame frame;
	frame.tick = m_sim->Tick();
	frame.inputs = m_others;
	frame.inputs[m_localSlot] = localInput;
	auto start = std::chrono::steady_clock::now();
	m_sim->Step( frame );
	m_stats.stepNs += NsSince( start );
	m_stats.steps += 1;
	m_last = std::move( frame );
}

} // namespace cb
