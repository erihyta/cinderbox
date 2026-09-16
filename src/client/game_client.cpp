#include "game_client.h"

#include "fingerprint.h"

#include <algorithm>
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
constexpr uint32_t kChecksumHistory = 600;
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
	std::printf( "[%s %6u] ", m_options.logName.c_str(), m_session ? m_session->CurrentTick() : 0 );
	std::vprintf( fmt, args );
	std::printf( "\n" );
	std::fflush( stdout );
	va_end( args );
}

bool GameClient::Start( const ClientOptions& options, double now )
{
	m_options = options;
	m_inputHistory.assign( std::max<uint32_t>( 64, options.inputRedundancy * 2 ), SentInput{} );
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
				case MsgType::Frame:
				{
					if ( m_state != ClientState::Playing )
					{
						break;
					}
					InputFrame frame;
					if ( m_codec.Decode( r, frame ) == false )
					{
						Log( "corrupt frame, requesting state" );
						Encode( MsgResyncRequest{ m_session->ConfirmedTick() }, m_buffer );
						m_transport.Send( ev.peer, ChannelReliable, m_buffer, true );
						break;
					}
					m_session->AddAuthoritativeFrame( frame );
					m_latestServerTick = frame.tick + 1;
					m_latestFrameTime = now;
					break;
				}
				case MsgType::Checksum:
				{
					MsgChecksum msg;
					if ( m_state == ClientState::Playing && Decode( r, msg ) )
					{
						m_pendingChecksums.push_back( msg );
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

	bool rebuild = m_session == nullptr || !( msg.config == m_config ) || msg.slot != m_slot;
	if ( rebuild )
	{
		m_config = msg.config;
		m_slot = msg.slot;
		m_session = std::make_unique<RollbackSession>( m_config, m_slot, m_options.maxRollbackTicks );
	}

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
	m_codec.Reset( msg.baseInputs );

	bool reconnect = m_token != 0 && m_token == msg.reconnectToken;
	m_token = msg.reconnectToken;
	m_state = ClientState::Playing;
	m_latestServerTick = msg.snapshotTick;
	m_latestFrameTime = now;
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

	Log( "%s as slot %u at tick %u (%zu KB state)", reconnect ? "resumed" : "joined", m_slot, msg.snapshotTick,
		 msg.image.size() / 1024 );
}

void GameClient::Advance( double now, const InputSampler& sampleInput )
{
	const double rate = double( m_config.tickRate );
	const double dt = 1.0 / rate;

	PeerStats ps = m_transport.Stats( m_transport.ServerPeer() );
	double rtt = double( ps.roundTripMs ) / 1000.0;
	double jitter = double( ps.roundTripVarianceMs ) / 1000.0;
	m_stats.rttMs = ps.roundTripMs;

	// Be far enough ahead of the server that our input for tick T arrives before it simulates T.
	double sinceFrame = std::min( now - m_latestFrameTime, 1.0 );
	double serverNow = double( m_latestServerTick ) + ( sinceFrame + 0.5 * rtt ) * rate;
	double target = serverNow + ( 0.5 * rtt + jitter ) * rate + double( m_options.leadMarginTicks );
	double current = double( m_session->CurrentTick() ) + m_accumulator * rate;
	double error = target - current;

	double frameDt = m_lastUpdate < 0.0 ? 0.0 : std::clamp( now - m_lastUpdate, 0.0, 0.25 );
	m_lastUpdate = now;

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
	else
	{
		scale = 1.0 + std::clamp( error * kRateGain, -kMaxRateAdjust, kMaxRateAdjust );
	}
	m_accumulator += frameDt * scale;
	m_stats.tickError = error;
	m_stats.rateScale = scale;

	uint64_t rollbacksBefore = m_session->GetStats().rollbacks;
	m_session->Reconcile();

	uint32_t ticks = 0;
	while ( m_accumulator >= dt && ticks < kMaxTicksPerFrame )
	{
		uint32_t tick = m_session->CurrentTick();
		PlayerInput input = sampleInput( tick );
		if ( m_session->AdvanceOne( input ) == false )
		{
			// Too far ahead of confirmed data (latency above the rollback window, or the server
			// stopped sending): hold time instead of drifting.
			m_accumulator = std::min( m_accumulator, dt );
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
	m_stats.rolledBackLastFrame = m_session->GetStats().rollbacks != rollbacksBefore;

	if ( ticks > 0 )
	{
		SendInputs();
	}
	VerifyChecksums();
}

void GameClient::SendInputs()
{
	if ( m_haveInput == false )
	{
		return;
	}

	MsgInput msg;
	msg.newestTick = m_newestInputTick;
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
