#include "hybrid_session.h"

#include "box3d/box3d.h"

#include <algorithm>
#include <chrono>

namespace cb
{

namespace
{
constexpr uint32_t kMaxAheadCap = 120;
constexpr size_t kMaxStatesKept = 256;
constexpr size_t kMaxFramesKept = 1024;

uint64_t NsSince( std::chrono::steady_clock::time_point start )
{
	return uint64_t( std::chrono::duration_cast<std::chrono::nanoseconds>( std::chrono::steady_clock::now() - start ).count() );
}
} // namespace

HybridSession::HybridSession( const SimConfig& config, PlayerSlot localSlot, uint32_t maxAhead, const LevelLayout& map )
	: m_config( config )
	, m_localSlot( localSlot )
	, m_maxAhead( std::clamp<uint32_t>( maxAhead, 1, kMaxAheadCap ) )
{
	m_sim = std::make_unique<Simulation>( config, map );
	m_sampled.resize( kMaxAheadCap + 8 );
	m_last.tick = UINT32_MAX;
}

void HybridSession::Start()
{
	m_started = true;
	m_sim->SetFollowsPhysics( true );
	m_predicted = m_sim->Tick();
	m_framesReached = std::max( m_framesReached, m_sim->Tick() );
	m_newest = m_reconciled = m_sim->Tick();
	m_placed = UINT32_MAX;
	m_frames.erase( m_frames.begin(), m_frames.lower_bound( m_sim->Tick() ) );
	// The characters as the joining snapshot has them, everyone else's a stand-in from now on.
	std::vector<uint8_t> state;
	m_sim->SaveState( state );
	m_sim->ApplyState( state.data(), state.size(), m_localSlot, Simulation::StateApply::Others );
}

void HybridSession::AddFrame( const InputFrame& frame )
{
	if ( frame.tick < m_sim->Tick() || m_frames.count( frame.tick ) != 0 )
	{
		return;
	}
	InputFrame& kept = m_frames[frame.tick];
	kept = frame;
	kept.events.clear();
	kept.inputs = {};
	m_framesReached = std::max( m_framesReached, frame.tick + 1 );
	while ( m_frames.size() > kMaxFramesKept )
	{
		m_frames.erase( std::prev( m_frames.end() ) ); // (the newest go: the physics needs the oldest first)
	}
}

void HybridSession::AddState( uint32_t tick, const uint8_t* data, size_t size, const std::array<PlayerInput, kMaxPlayers>& inputs )
{
	if ( m_states.count( tick ) != 0 || ( m_placed != UINT32_MAX && tick < m_placed ) )
	{
		return;
	}
	m_states[tick].bytes.assign( data, data + size );
	m_states[tick].inputs = inputs;
	m_newest = std::max( m_newest, tick );
	while ( m_states.size() > kMaxStatesKept )
	{
		m_states.erase( m_states.begin() );
	}
}

void HybridSession::AddChecksum( uint32_t tick, uint64_t hash )
{
	if ( tick >= m_sim->Tick() )
	{
		m_checksums[tick] = hash;
	}
}

bool HybridSession::HasNextFrame() const
{
	return m_frames.count( m_sim->Tick() ) != 0;
}

void HybridSession::PlaceOthers( uint32_t tick, bool again )
{
	// The newest state that is not ahead of the physics.
	auto it = m_states.upper_bound( tick );
	if ( it == m_states.begin() )
	{
		return;
	}
	--it;
	if ( it->first != m_placed || again )
	{
		auto start = std::chrono::steady_clock::now();
		if ( m_sim->ApplyState( it->second.bytes.data(), it->second.bytes.size(), m_localSlot, Simulation::StateApply::Others ) )
		{
			m_placed = it->first;
			m_othersInputs = it->second.inputs;
		}
		else
		{
			m_stats.rejected += 1;
		}
		m_stats.applyNs += NsSince( start );
	}
	// (Older ones are of no more use: the newest is kept for the own character either way.)
	m_states.erase( m_states.begin(), it );
}

uint32_t HybridSession::AdvancePhysics( uint32_t upTo )
{
	uint32_t stepped = 0;
	while ( m_started && m_sim->Tick() < upTo )
	{
		const uint32_t tick = m_sim->Tick();
		auto frame = m_frames.find( tick );
		if ( frame == m_frames.end() )
		{
			m_stats.physicsStalls += 1;
			break;
		}
		// The own character is ahead. For this tick it stands as the others do, where the server had
		// it then (a command of this tick reads it: a body taken over, the one left behind), and is
		// the prediction again after: what this tick's commands did to it is in a later state.
		const uint32_t own = m_sim->PlayerNetId( m_localSlot );
		m_sim->SaveState( m_own, false, own != 0 ? own : UINT32_MAX );
		auto placed = m_states.find( m_placed );
		if ( placed != m_states.end() )
		{
			m_sim->ApplyState( placed->second.bytes.data(), placed->second.bytes.size(), m_localSlot, Simulation::StateApply::Own );
		}
		if ( m_scramble > 0.0f )
		{
			m_stats.scrambled += m_sim->ScrambleCharacters( tick, m_scramble );
		}
		auto start = std::chrono::steady_clock::now();
		m_sim->Step( frame->second );
		m_stats.physicsNs += NsSince( start );
		m_stats.physicsSteps += 1;
		m_sim->ApplyState( m_own.data(), m_own.size(), m_localSlot, Simulation::StateApply::Own );
		MatchLaunches( frame->second );
		// The others where the server had them at the tick reached (and not where this tick's
		// commands put them: what a command does to a character is in the server's state).
		PlaceOthers( m_sim->Tick(), frame->second.commands.empty() == false || m_scramble > 0.0f );
		m_frames.erase( frame );
		stepped += 1;

		if ( auto sum = m_checksums.find( tick ); sum != m_checksums.end() )
		{
			m_stats.checksums += 1;
			if ( sum->second != m_sim->PhysicsHash() )
			{
				m_stats.desyncs += 1;
				if ( m_desyncTick == UINT32_MAX )
				{
					PhysicsBodies( *m_sim, m_desyncBodies );
				}
				m_desyncTick = std::min( m_desyncTick, tick );
			}
		}
		m_checksums.erase( m_checksums.begin(), m_checksums.upper_bound( tick ) );
	}
	return stepped;
}

void HybridSession::Reconcile()
{
	if ( m_started == false || m_newest <= m_reconciled )
	{
		return;
	}
	auto it = m_states.find( m_newest );
	if ( it == m_states.end() )
	{
		return;
	}
	m_reconciled = m_newest;
	auto start = std::chrono::steady_clock::now();
	const bool applied = m_sim->ApplyState( it->second.bytes.data(), it->second.bytes.size(), m_localSlot, Simulation::StateApply::Own );
	m_stats.applyNs += NsSince( start );
	if ( applied == false )
	{
		m_stats.rejected += 1;
		return;
	}
	m_stats.states += 1;
	if ( m_predicted < m_newest )
	{
		m_predicted = m_newest; // (behind the server: from its state on)
		m_stats.lastDepth = 0;
		return;
	}
	// The local inputs since the state's tick, again (each as it was sampled).
	uint32_t depth = 0;
	start = std::chrono::steady_clock::now();
	for ( uint32_t t = m_newest; t < m_predicted; ++t )
	{
		const Sampled& s = m_sampled[t % m_sampled.size()];
		if ( s.tick != t )
		{
			break; // (never sampled: from before this session predicted)
		}
		m_sim->StepCharacters( t, m_localSlot, s.input );
		depth += 1;
	}
	m_sim->TakePredictedLaunches(); // (drawn already, when first predicted)
	m_stats.predictNs += NsSince( start );
	m_stats.resimulatedTicks += depth;
	m_stats.lastDepth = depth;
}

bool HybridSession::CanPredict() const
{
	return m_started && m_predicted < m_newest + m_maxAhead;
}

bool HybridSession::Predict( const PlayerInput& input )
{
	Reconcile();
	if ( CanPredict() == false )
	{
		m_stats.predictStalls += 1;
		return false;
	}
	m_sampled[m_predicted % m_sampled.size()] = { m_predicted, input };
	auto start = std::chrono::steady_clock::now();
	m_sim->StepCharacters( m_predicted, m_localSlot, input );
	m_stats.predictNs += NsSince( start );
	for ( const Simulation::PredictedLaunch& launch : m_sim->TakePredictedLaunches() )
	{
		m_launches.push_back( { launch, m_nextLaunchId++, 0, 0 } );
	}
	m_stats.predicted += 1;
	m_last.tick = m_predicted;
	m_last.inputs = m_othersInputs;
	m_last.inputs[m_localSlot] = input;
	m_predicted += 1;
	return true;
}

void HybridSession::PhysicsBodies( Simulation& sim, std::vector<BodyAt>& out )
{
	// (As PhysicsHash has them: in the physics world, not a pusher or a character.)
	out.clear();
	for ( const auto& r : sim.Entities() )
	{
		flecs::entity e( sim.World(), r.entity );
		const PhysicsBody* pb = e.try_get<PhysicsBody>();
		const bool physics = e.has<RagdollBodies>() || ( pb != nullptr && pb->body.world0 == 0 );
		if ( physics == false || e.has<Pusher>() || e.has<Character>() )
		{
			continue;
		}
		const Transform* t = e.try_get<Transform>();
		const Velocity* v = e.try_get<Velocity>();
		out.push_back( { r.netId, t ? t->position : b3Vec3{}, v ? v->linear : b3Vec3{} } );
	}
}

void HybridSession::MatchLaunches( const InputFrame& frame )
{
	// The server's copy of an own throw: the carried copy by its NetId, or by its thrower and kind at
	// about the tick it was predicted (a new one; one split off a stack, which the server named).
	const uint32_t own = m_sim->PlayerNetId( m_localSlot );
	for ( const PhysicsEvent& event : frame.physics )
	{
		if ( event.type != PhysicsEventType::IntoWorld )
		{
			continue;
		}
		for ( OwnLaunch& l : m_launches )
		{
			const bool same = ( l.launch.item != 0 && event.netId == l.launch.item ) ||
							  ( event.owner == own && event.kind == l.launch.kind && frame.tick + 3 >= l.launch.tick && frame.tick <= l.launch.tick + 3 );
			if ( l.server == 0 && same )
			{
				l.server = event.netId;
				l.matchedAt = frame.tick;
				break;
			}
		}
	}
	// Done with: one the server never threw (it was predicted wrong), one handed over a while ago,
	// one whose item is gone.
	const uint32_t now = m_sim->Tick();
	const uint32_t rate = m_config.tickRate;
	m_launches.erase( std::remove_if( m_launches.begin(), m_launches.end(),
									  [&]( const OwnLaunch& l ) {
										  if ( l.server == 0 )
										  {
											  return now > l.launch.tick + rate; // (a second: never coming)
										  }
										  return now > l.matchedAt + 3 * rate || m_sim->FindEntity( l.server ).is_valid() == false;
									  } ),
					  m_launches.end() );
}

void HybridSession::OwnLaunches( std::vector<DrawnLaunch>& out )
{
	out.clear();
	const uint32_t physics = m_sim->Tick();
	// (Handed over in a fifth of a second.)
	const float blendTicks = std::max( 1.0f, 0.2f * float( m_config.tickRate ) );
	for ( const OwnLaunch& l : m_launches )
	{
		DrawnLaunch d;
		d.kind = l.launch.kind;
		d.grip.rotation = l.launch.rotation;
		// Where the prediction has it at the predicted tick.
		b3Vec3 velocity = l.launch.velocity;
		const uint32_t flown = m_predicted > l.launch.tick ? m_predicted - l.launch.tick : 0;
		// (Through the server's copy, once that is in the world: it is this one.)
		d.grip.position = m_sim->FlyItem( l.launch.kind, l.launch.position, velocity, flown, l.server );
		d.velocity = velocity;
		if ( l.server == 0 )
		{
			d.netId = l.id;
			out.push_back( d );
			continue;
		}
		const flecs::entity e = m_sim->FindEntity( l.server );
		const Transform* t = e.is_valid() ? e.try_get<Transform>() : nullptr;
		const PhysicsBody* pb = e.is_valid() ? e.try_get<PhysicsBody>() : nullptr;
		if ( t == nullptr || pb == nullptr )
		{
			continue;
		}
		// The server's, in the past: carried forward to the predicted tick while it flies.
		b3Vec3 going = b3Body_GetLinearVelocity( m_sim->BodyOf( *pb ) );
		const uint32_t ahead = m_predicted > physics ? m_predicted - physics : 0;
		const b3Vec3 later = m_sim->FlyItem( l.launch.kind, t->position, going, ahead, l.server );
		d.netId = l.server;
		d.server = true;
		d.forward = b3Sub( later, t->position );
		d.handedOver = std::min( 1.0f, float( physics - l.matchedAt ) / blendTicks );
		out.push_back( d );
	}
}

} // namespace cb
