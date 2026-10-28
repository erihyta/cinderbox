#include "game_client.h"

#include "anim_graph.h"

#include "fingerprint.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace cb
{

using namespace net;

namespace
{
constexpr uint32_t kMaxTicksPerFrame = 8;
constexpr double kSnapTicks = 30.0;
constexpr double kRateGain = 0.02;
constexpr double kMaxRateAdjust = 0.15;
// Behind the server, every input arrives late and the server drops presses, so catching up is urgent:
// beyond this many ticks the client runs up to kMaxCatchUp times as fast. Ahead only costs latency,
// so slowing down stays gentle.
constexpr double kCatchUpTicks = 1.5;
constexpr double kCatchUpGain = 0.25;
constexpr double kMaxCatchUp = 2.0;
constexpr uint32_t kChecksumHistory = 600;
constexpr double kClockDecayTicksPerSecond = 0.25;
constexpr size_t kKnownInputHistory = 256;
constexpr double kWindowShrinkDelay = 3.0; // seconds of lower latency before the window shrinks
} // namespace

const char* ToString( ClientState state )
{
	switch ( state )
	{
		case ClientState::Idle:
			return "idle";
		case ClientState::Connecting:
			return "connecting";
		case ClientState::AwaitingWelcome:
			return "joining";
		case ClientState::Playing:
			return "playing";
		case ClientState::Reconnecting:
			return "reconnecting";
		case ClientState::Rejected:
			return "rejected";
	}
	return "?";
}

GameClient::GameClient() = default;
GameClient::~GameClient() = default;

void GameClient::Log( const char* fmt, ... ) const
{
	if ( m_options.verbose == false )
	{
		return;
	}
	va_list args;
	va_start( args, fmt );
	std::printf( "[%s %6u] ", m_options.logName.c_str(), CurrentTick() );
	std::vprintf( fmt, args );
	std::printf( "\n" );
	std::fflush( stdout );
	va_end( args );
}

bool GameClient::Start( const ClientOptions& options, double now )
{
	m_options = options;
	m_inputHistory.assign( std::max<uint32_t>( 64, options.inputRedundancy * 2 ), SentInput{} );
	m_knownInputs.assign( kKnownInputHistory, KnownInputs{} );
	m_options.maxRollbackTicks = std::max( m_options.maxRollbackTicks, m_options.minRollbackTicks );
	m_state = ClientState::Connecting;
	m_nextConnectAttempt = now;
	m_lastNow = now;
	return true;
}

float GameClient::TickAlpha() const
{
	if ( m_state != ClientState::Playing || m_config.tickRate == 0 )
	{
		return 0.0f;
	}
	double a = m_accumulator * double( m_config.tickRate );
	return float( std::clamp( a, 0.0, 1.0 ) );
}

void GameClient::BeginReconnect( double now )
{
	m_connectPending = false;
	if ( m_state == ClientState::Rejected )
	{
		return;
	}
	m_stats.disconnects += 1;
	m_state = m_token != 0 ? ClientState::Reconnecting : ClientState::Connecting;
	m_nextConnectAttempt = now + m_options.reconnectIntervalSeconds;
	Log( "connection lost, %s", m_token != 0 ? "time frozen, reconnecting" : "retrying" );
}

void GameClient::DropConnectionHard()
{
	if ( m_transport.ServerPeer() != 0 )
	{
		m_transport.DropHard( m_transport.ServerPeer() );
		BeginReconnect( m_lastNow );
	}
}

void GameClient::Update( double now, const InputSampler& sampleInput )
{
	m_lastNow = now;

	m_events.clear();
	m_transport.Poll( m_events );
	for ( NetEvent& ev : m_events )
	{
		HandleEvent( ev, now );
	}

	bool wantsConnection = m_state == ClientState::Connecting || m_state == ClientState::Reconnecting;
	if ( wantsConnection && m_connectPending == false && now >= m_nextConnectAttempt )
	{
		Log( "connecting to %s:%u", m_options.host.c_str(), m_options.port );
		if ( m_transport.Connect( m_options.host, m_options.port ) )
		{
			m_connectPending = true;
		}
		else
		{
			// Only an address that names nothing counts: a busy peer is tried again.
			m_stats.connectFailures += Transport::Resolves( m_options.host ) ? 0 : 1;
			m_nextConnectAttempt = now + m_options.reconnectIntervalSeconds;
		}
	}

	if ( m_state == ClientState::Playing )
	{
		Advance( now, sampleInput );
	}
	else
	{
		m_lastUpdate = -1.0;
		m_stats.ticksLastFrame = 0;
		m_stats.rolledBackLastFrame = false;
	}

	m_transport.Flush();
}

void GameClient::HandleEvent( const NetEvent& ev, double now )
{
	switch ( ev.type )
	{
		case NetEvent::Type::Connected:
		{
			m_connectPending = false;
			if ( m_state == ClientState::Connecting || m_state == ClientState::Reconnecting )
			{
				MsgHello hello;
				hello.fingerprint = BuildFingerprint();
				hello.reconnectToken = m_token;
				hello.name = m_options.playerName;
				Encode( hello, m_buffer );
				m_transport.Send( ev.peer, ChannelReliable, m_buffer, true );
				m_state = ClientState::AwaitingWelcome;
			}
			break;
		}

		case NetEvent::Type::Disconnected:
			BeginReconnect( now );
			break;

		case NetEvent::Type::Received:
		{
			ByteReader r( ev.data.data(), ev.data.size() );
			auto type = ReadType( r );
			if ( !type )
			{
				break;
			}

			switch ( *type )
			{
				case MsgType::Welcome:
				{
					MsgWelcome msg;
					if ( Decode( r, msg ) )
					{
						HandleWelcome( msg, now );
					}
					break;
				}
				case MsgType::FrameBatch:
					if ( m_state == ClientState::Playing && m_model == NetModel::Inputs )
					{
						HandleFrameBatch( r, now );
					}
					break;
				case MsgType::State:
					if ( m_state == ClientState::Playing && m_model == NetModel::State )
					{
						HandleState( r, now );
					}
					break;
				case MsgType::Checksum:
				{
					MsgChecksum msg;
					if ( m_state == ClientState::Playing && m_session != nullptr && Decode( r, msg ) )
					{
						m_pendingChecksums.push_back( msg );
					}
					break;
				}
				case MsgType::PrivateFields:
				{
					MsgPrivateFields msg;
					if ( Decode( r, msg ) )
					{
						m_privates = msg.values;
					}
					break;
				}
				case MsgType::Items:
				{
					// A look published again: the items to have now. Nothing of the simulation.
					MsgItems msg;
					if ( Decode( r, msg ) && m_schemaGeneration != 0 && !( msg.items == m_schema.items ) )
					{
						m_schema.items = msg.items;
						m_schemaGeneration += 1;
					}
					break;
				}
				case MsgType::Text:
				{
					MsgText msg;
					if ( Decode( r, msg ) )
					{
						m_texts.push_back( std::move( msg ) );
						if ( m_texts.size() > 32 )
						{
							m_texts.erase( m_texts.begin() );
						}
						m_textCount += 1;
					}
					break;
				}
				case MsgType::PlayerNames:
				{
					MsgPlayerNames msg;
					if ( Decode( r, msg ) )
					{
						m_names = {};
						for ( const auto& [slot, name] : msg.names )
						{
							m_names[slot] = name;
						}
						m_namesGeneration += 1;
					}
					break;
				}
				case MsgType::Reject:
				{
					MsgReject msg;
					Decode( r, msg );
					m_rejectReason = msg.reason;
					m_state = ClientState::Rejected;
					Log( "rejected: %s", msg.reason.c_str() );
					break;
				}
				default:
					break;
			}
			break;
		}
	}
}

void GameClient::HandleWelcome( MsgWelcome& msg, double now )
{
	if ( msg.fingerprint != BuildFingerprint() )
	{
		m_rejectReason = "simulation build mismatch";
		m_state = ClientState::Rejected;
		Log( "rejected: %s", m_rejectReason.c_str() );
		return;
	}

	ModSchema schema;
	if ( DecodeSchema( msg.schema.data(), msg.schema.size(), schema ) == false )
	{
		m_rejectReason = "bad mod schema from the server";
		m_state = ClientState::Rejected;
		Log( "rejected: %s", m_rejectReason.c_str() );
		return;
	}

	// The characters' state machines and capsules: the simulation must run exactly the server's.
	std::vector<Simulation::CharacterBody> characters;
	{
		std::string error;
		if ( Simulation::CharactersOf( schema, characters, error ) == false )
		{
			m_rejectReason = "bad animation state machine from the server: " + error;
			m_state = ClientState::Rejected;
			Log( "rejected: %s", m_rejectReason.c_str() );
			return;
		}
	}

	LevelLayout map;
	std::string mapError;
	if ( DeserializeMap( msg.map.data(), msg.map.size(), map, mapError ) == false )
	{
		m_rejectReason = "bad map from the server: " + mapError;
		m_state = ClientState::Rejected;
		Log( "rejected: %s", m_rejectReason.c_str() );
		return;
	}

	if ( m_options.simulate && msg.model == NetModel::State )
	{
		// The state model: a world of the server's map that waits for its first state (whole).
		m_session.reset();
		bool rebuild = m_states == nullptr || !( msg.config == m_config ) || msg.slot != m_slot || msg.mapHash != m_mapHash;
		if ( rebuild )
		{
			m_states = std::make_unique<StateSession>( msg.config, msg.slot, m_options.maxRollbackTicks + msg.stateEvery, map );
			m_states->Sim().SetPhysicsThreaded( m_options.physicsThreaded );
		}
		m_states->Sim().SetCharacters( characters );
		std::string packWarnings;
		m_states->Sim().SetAnimPacks( CompileAnimPacks( schema, packWarnings ) );
		m_states->Sim().SetMotions( CompileMotions( schema, packWarnings ) );
		m_states->Sim().SetItemShapes( schema.itemShapes );
	}
	else if ( m_options.simulate )
	{
		m_states.reset();
		// A different map means a different level, so the session cannot be reused.
		bool rebuild = m_session == nullptr || !( msg.config == m_config ) || msg.slot != m_slot || msg.mapHash != m_mapHash;
		if ( rebuild )
		{
			m_session = std::make_unique<RollbackSession>( msg.config, msg.slot, m_options.minRollbackTicks,
														   m_options.maxRollbackTicks, map );
			m_session->SetReconcileEvery( m_options.reconcileEveryTicks );
			m_session->Sim().SetPhysicsThreaded( m_options.physicsThreaded );
		}
		m_session->Sim().SetCharacters( characters );
		std::string packWarnings;
		m_session->Sim().SetAnimPacks( CompileAnimPacks( schema, packWarnings ) );
		m_session->Sim().SetMotions( CompileMotions( schema, packWarnings ) );
		m_session->Sim().SetItemShapes( schema.itemShapes );

		if ( m_session->Sim().LoadPortable( msg.image ) == false )
		{
			m_rejectReason = "could not load the server state";
			m_state = ClientState::Rejected;
			Log( "rejected: %s", m_rejectReason.c_str() );
			return;
		}

		Snapshot snapshot;
		m_session->Sim().Save( snapshot );
		m_session->Reset( snapshot, msg.baseInputs );
	}
	m_model = msg.model;
	m_stateEvery = msg.stateEvery;
	m_stateFrames.clear();
	m_stateAck = 0;
	m_config = msg.config;
	m_map = std::move( map );
	if ( !( schema == m_schema ) || m_schemaGeneration == 0 )
	{
		m_schema = std::move( schema );
		m_schemaGeneration += 1;
	}
	m_mapHash = msg.mapHash;
	m_slot = msg.slot;
	m_liteTick = msg.snapshotTick;
	m_liteConfirmed = msg.snapshotTick;
	m_waitingBatches.clear();
	for ( KnownInputs& k : m_knownInputs )
	{
		k.tick = UINT32_MAX;
	}
	if ( msg.snapshotTick > 0 )
	{
		m_knownInputs[( msg.snapshotTick - 1 ) % kKnownInputHistory] = { msg.snapshotTick - 1, msg.baseInputs };
	}

	bool reconnect = m_token != 0 && m_token == msg.reconnectToken;
	m_token = msg.reconnectToken;
	m_state = ClientState::Playing;
	m_haveClock = false;
	OnServerTick( msg.snapshotTick, now );
	m_accumulator = 0.0;
	m_lastUpdate = -1.0;
	m_pendingChecksums.clear();
	for ( SentInput& s : m_inputHistory )
	{
		s.tick = UINT32_MAX;
	}
	m_haveInput = false;
	m_resetGeneration += 1;
	m_stats.welcomes += 1;

	Log( "%s as slot %u at tick %u (the %s network model, %zu KB state)", reconnect ? "resumed" : "joined", m_slot, msg.snapshotTick,
		 ToString( m_model ), msg.image.size() / 1024 );
}

uint32_t GameClient::AckTick() const
{
	if ( m_model == NetModel::State )
	{
		return m_stateAck;
	}
	return m_session ? m_session->ConfirmedTick() : m_liteConfirmed;
}

// The state model: one of the server's states, whole or against one we said we have. Kept as a
// base and handed to the session; older ones than the newest are no use (the next is a few ticks
// away), nor one against a base we no longer have.
void GameClient::HandleState( ByteReader& r, double now )
{
	uint32_t tick = 0, baseTick = 0;
	if ( ReadStateHeader( r, tick, baseTick ) == false )
	{
		m_stats.statesIgnored += 1;
		return;
	}
	m_stats.statesReceived += 1;
	OnServerTick( tick, now );
	if ( m_stateFrames.empty() == false && tick <= m_stateFrames.back().tick )
	{
		m_stats.statesIgnored += 1;
		return;
	}
	const StateFrame* base = nullptr;
	if ( baseTick != kNoStateBase )
	{
		for ( const StateFrame& f : m_stateFrames )
		{
			base = f.tick == baseTick ? &f : base;
		}
		if ( base == nullptr )
		{
			m_stats.statesIgnored += 1;
			return;
		}
	}
	StateFrame frame;
	if ( DecodeState( r, tick, base, frame ) == false )
	{
		Log( "corrupt state for tick %u", tick );
		m_stats.statesIgnored += 1;
		return;
	}
	if ( m_states != nullptr )
	{
		m_states->AddState( frame.tick, frame.state.data(), frame.state.size(), frame.inputs );
	}
	else
	{
		// A lite client simulates nothing, but keeps and acknowledges states as a player's game
		// does: the server's work and the bandwidth are a real client's.
		m_liteTick = std::max( m_liteTick, tick );
	}
	m_stateFrames.push_back( std::move( frame ) );
	// (Enough for the bases the server may still use: it answers our acknowledgements a round trip
	// late.)
	while ( m_stateFrames.size() > 32 )
	{
		m_stateFrames.pop_front();
	}
	m_stateAck = tick;
}

// A batch holds the server's newest frames, each for the third time at most (or everything from
// our last acknowledgement, when the server has waited for one). Old or duplicate batches are
// harmless: frames we already have are skipped. One that begins after what we have waits.
void GameClient::HandleFrameBatch( ByteReader& r, double now )
{
	uint32_t firstTick, count;
	if ( ReadFrameBatchHeader( r, firstTick, count ) == false )
	{
		return;
	}
	m_stats.batchesReceived += 1;
	uint32_t confirmed = AckTick();
	uint32_t lastTick = firstTick + count - 1;
	OnServerTick( lastTick + 1, now );

	if ( lastTick < confirmed )
	{
		return; // nothing new
	}
	if ( m_session == nullptr )
	{
		if ( firstTick <= confirmed )
		{
			m_liteConfirmed = lastTick + 1;
		}
		return;
	}
	if ( firstTick > confirmed )
	{
		// A hole before it: kept until what fills the hole arrives (a few, the newest).
		const size_t kWaiting = 16;
		bool have = false;
		for ( const WaitingBatch& w : m_waitingBatches )
		{
			have |= w.firstTick == firstTick && w.count == count;
		}
		if ( have == false )
		{
			if ( m_waitingBatches.size() >= kWaiting )
			{
				m_waitingBatches.erase( m_waitingBatches.begin() );
				m_stats.batchesIgnored += 1;
			}
			m_waitingBatches.push_back( { firstTick, count, std::vector<uint8_t>( r.Here(), r.Here() + r.Remaining() ) } );
		}
		return;
	}

	if ( UseFrameBatch( firstTick, count, r.Here(), r.Remaining() ) == false )
	{
		return;
	}
	// What waited for these frames, and what waited for those.
	for ( bool used = true; used && m_waitingBatches.empty() == false; )
	{
		used = false;
		for ( size_t i = 0; i < m_waitingBatches.size(); )
		{
			WaitingBatch& w = m_waitingBatches[i];
			uint32_t have = m_session->ConfirmedTick();
			if ( w.firstTick + w.count - 1 < have )
			{
				m_waitingBatches.erase( m_waitingBatches.begin() + ptrdiff_t( i ) );
				continue;
			}
			if ( w.firstTick <= have )
			{
				used |= UseFrameBatch( w.firstTick, w.count, w.body.data(), w.body.size() );
				m_waitingBatches.erase( m_waitingBatches.begin() + ptrdiff_t( i ) );
				continue;
			}
			++i;
		}
	}
}

// The frames of a batch that begins at or before the next one we need. False: none could be read.
bool GameClient::UseFrameBatch( uint32_t firstTick, uint32_t count, const uint8_t* body, size_t size )
{
	InputArray base{};
	if ( firstTick > 0 )
	{
		const KnownInputs& k = m_knownInputs[( firstTick - 1 ) % kKnownInputHistory];
		if ( k.tick != firstTick - 1 )
		{
			m_stats.batchesIgnored += 1;
			return false;
		}
		base = k.inputs;
	}

	ByteReader r( body, size );
	if ( ReadFrameBatchBody( r, base, firstTick, count, m_batchScratch ) == false )
	{
		Log( "corrupt frame batch at tick %u", firstTick );
		m_stats.batchesIgnored += 1;
		return false;
	}

	for ( const InputFrame& frame : m_batchScratch )
	{
		if ( frame.tick != m_session->ConfirmedTick() )
		{
			continue;
		}
		m_session->AddAuthoritativeFrame( frame );
		m_knownInputs[frame.tick % kKnownInputHistory] = { frame.tick, frame.inputs };
	}
	return true;
}

// Prediction must reach about RTT + jitter + the lead margin ahead of confirmed frames. Grow the
// window at once when latency rises; shrink it only after latency has stayed lower for a while.
void GameClient::UpdateRollbackWindow( double rttTicks, double frameDt )
{
	double needed = std::ceil( rttTicks ) + double( m_options.leadMarginTicks ) + 2.0;
	if ( m_states != nullptr )
	{
		// How far past the newest state: a round trip and a state's interval (no snapshots: no cost
		// in going further, so it never shrinks).
		uint32_t ahead = uint32_t( std::clamp( needed + double( m_stateEvery ), double( m_options.minRollbackTicks ),
											   double( m_options.maxRollbackTicks + m_stateEvery ) ) );
		m_states->SetMaxAhead( std::max( ahead, m_states->MaxAhead() ) );
		m_stats.rollbackWindow = m_states->MaxAhead();
		return;
	}
	if ( m_session == nullptr )
	{
		return;
	}
	uint32_t desired = uint32_t( std::clamp( needed, double( m_options.minRollbackTicks ), double( m_options.maxRollbackTicks ) ) );
	uint32_t current = m_session->MaxRollback();
	if ( desired > current )
	{
		m_session->SetMaxRollback( desired );
		m_windowShrinkTimer = 0.0;
	}
	else if ( desired + 1 < current )
	{
		m_windowShrinkTimer += frameDt;
		if ( m_windowShrinkTimer >= kWindowShrinkDelay )
		{
			m_session->SetMaxRollback( current - 1 );
			m_windowShrinkTimer = 0.0;
		}
	}
	else
	{
		m_windowShrinkTimer = 0.0;
	}
	m_stats.rollbackWindow = m_session->MaxRollback();
}

// `tickAfter` is the server's tick right after it sent the message that just arrived.
void GameClient::OnServerTick( uint32_t tickAfter, double now )
{
	double offset = double( tickAfter ) - now * double( m_config.tickRate );
	if ( m_haveClock == false || offset > m_clockOffset )
	{
		m_clockOffset = offset;
		m_haveClock = true;
	}
	m_latestServerTick = tickAfter;
	m_latestFrameTime = now;
}

void GameClient::Advance( double now, const InputSampler& sampleInput )
{
	const double rate = double( m_config.tickRate );
	const double dt = 1.0 / rate;

	PeerStats ps = m_transport.Stats( m_transport.ServerPeer() );
	double rtt = double( ps.roundTripMs ) / 1000.0;
	double jitter = double( ps.roundTripVarianceMs ) / 1000.0;
	m_stats.rttMs = ps.roundTripMs;

	double frameDt = m_lastUpdate < 0.0 ? 0.0 : std::clamp( now - m_lastUpdate, 0.0, 0.25 );
	m_stats.playingSeconds += frameDt;
	m_lastUpdate = now;

	// Server clock estimate: the fastest frame arrivals define the offset (frames held back behind a
	// lost packet arrive late and would make the server look slower than it is). The estimate
	// decays slowly so it still follows a real increase in latency.
	m_clockOffset -= kClockDecayTicksPerSecond * frameDt;
	double serverNow = now * rate + m_clockOffset + 0.5 * rtt * rate;
	double sinceFrame = now - m_latestFrameTime;
	if ( sinceFrame > 1.0 )
	{
		// The server went quiet; do not keep extrapolating.
		serverNow = std::min( serverNow, double( m_latestServerTick ) + ( 1.0 + 0.5 * rtt ) * rate );
	}

	UpdateRollbackWindow( ( rtt + 2.0 * jitter ) * rate, frameDt );

	// Be far enough ahead of the server that our input for tick T arrives before it simulates T.
	double target = serverNow + ( 0.5 * rtt + jitter ) * rate + double( m_options.leadMarginTicks );
	double current = double( CurrentTick() ) + m_accumulator * rate;
	double error = target - current;

	double scale = 1.0;
	if ( error > kSnapTicks )
	{
		// Far behind (just joined or hitched): catch up as fast as the per-frame cap allows.
		m_accumulator += double( kMaxTicksPerFrame ) * dt;
	}
	else if ( error < -kSnapTicks )
	{
		scale = 0.0; // far ahead: wait for the server
	}
	else if ( error > kCatchUpTicks )
	{
		scale = 1.0 + std::min( error * kCatchUpGain, kMaxCatchUp - 1.0 );
	}
	else
	{
		scale = 1.0 + std::clamp( error * kRateGain, -kMaxRateAdjust, kMaxRateAdjust );
	}
	m_accumulator += frameDt * scale;
	m_stats.tickError = error;
	m_stats.rateScale = scale;

	auto workStart = std::chrono::steady_clock::now();
	uint64_t rollbacksBefore = m_session ? m_session->GetStats().rollbacks : m_states ? m_states->GetStats().states : 0;
	if ( m_session )
	{
		m_session->Reconcile();
	}
	if ( m_states )
	{
		m_states->Reconcile();
	}

	uint32_t ticks = 0;
	while ( m_accumulator >= dt && ticks < kMaxTicksPerFrame )
	{
		uint32_t tick = CurrentTick();
		PlayerInput input = sampleInput( tick );
		const bool advanced = m_session ? m_session->AdvanceOne( input ) : m_states ? m_states->AdvanceOne( input ) : true;
		if ( m_session == nullptr && m_states == nullptr )
		{
			m_liteTick += 1;
		}
		else if ( advanced == false )
		{
			// Too far ahead of confirmed data (latency above the rollback window, or the server
			// stopped sending): hold time instead of drifting.
			m_accumulator = std::min( m_accumulator, dt );
			m_stats.stalledSeconds += frameDt;
			break;
		}
		m_inputHistory[tick % m_inputHistory.size()] = { tick, input };
		m_newestInputTick = tick;
		m_haveInput = true;
		m_accumulator -= dt;
		++ticks;
	}
	if ( m_accumulator > dt * kMaxTicksPerFrame )
	{
		m_accumulator = dt;
	}

	m_stats.ticksLastFrame = ticks;
	m_stats.rolledBackLastFrame = m_session ? m_session->GetStats().rollbacks != rollbacksBefore
										   : m_states && m_states->GetStats().states != rollbacksBefore;
	m_stats.simMsLastFrame = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - workStart ).count();
	m_stats.simMsMax = std::max( m_stats.simMsMax, m_stats.simMsLastFrame );
	m_stats.bytesSent = m_transport.BytesSent();
	m_stats.bytesReceived = m_transport.BytesReceived();

	if ( ticks > 0 )
	{
		SendInputs();
	}
	if ( m_session )
	{
		VerifyChecksums();
	}
}

void GameClient::SendText( const std::string& channel, const std::string& text )
{
	if ( m_state != ClientState::Playing )
	{
		return;
	}
	MsgText msg;
	msg.channel = channel.substr( 0, kMaxTextChannel );
	msg.text = SanitizeText( text, kMaxText );
	if ( msg.text.empty() )
	{
		return;
	}
	Encode( msg, m_buffer );
	m_transport.Send( m_transport.ServerPeer(), ChannelReliable, m_buffer, true );
}

void GameClient::SendInputs()
{
	if ( m_haveInput == false )
	{
		return;
	}

	MsgInput msg;
	msg.newestTick = m_newestInputTick;
	msg.ackTick = AckTick();
	uint32_t count = std::min( m_options.inputRedundancy, m_newestInputTick + 1 );
	uint32_t first = m_newestInputTick + 1 - count;
	for ( uint32_t t = first; t <= m_newestInputTick; ++t )
	{
		const SentInput& s = m_inputHistory[t % m_inputHistory.size()];
		if ( s.tick != t )
		{
			// Gap (e.g. right after a welcome): only send the contiguous tail.
			msg.inputs.clear();
			continue;
		}
		msg.inputs.push_back( s.input );
	}
	if ( msg.inputs.empty() )
	{
		return;
	}

	Encode( msg, m_buffer );
	m_transport.Send( m_transport.ServerPeer(), ChannelInput, m_buffer, false );
}

void GameClient::VerifyChecksums()
{
	uint32_t confirmed = m_session->ConfirmedTick();
	size_t kept = 0;
	for ( const MsgChecksum& c : m_pendingChecksums )
	{
		uint64_t hash;
		if ( m_session->GetConfirmedHash( c.tick, hash ) )
		{
			if ( hash == c.hash )
			{
				m_stats.checksumsVerified += 1;
			}
			else
			{
				m_stats.desyncs += 1;
				Log( "DESYNC at tick %u (local %016llx, server %016llx), requesting state", c.tick, (unsigned long long)hash,
					 (unsigned long long)c.hash );
				Encode( MsgResyncRequest{ c.tick }, m_buffer );
				m_transport.Send( m_transport.ServerPeer(), ChannelReliable, m_buffer, true );
			}
		}
		else if ( c.tick + kChecksumHistory > confirmed )
		{
			m_pendingChecksums[kept++] = c;
		}
	}
	m_pendingChecksums.resize( kept );
}

} // namespace cb
