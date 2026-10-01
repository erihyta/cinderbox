#include "live_source.h"

#include "fingerprint.h"
#include "simulation.h"

#include <chrono>

namespace cb
{

LiveSource::LiveSource( const ClientOptions& options )
	: m_options( options )
{
	StartThread();
}

LiveSource::~LiveSource()
{
	StopThread();
}

void LiveSource::SetInput( const PlayerInput& input )
{
	std::lock_guard<std::mutex> lock( m_inputMutex );
	m_input = input;
	m_latchedButtons |= uint8_t( input.buttons & BtnJump );
	m_latchedActions |= input.actions;
}

void LiveSource::Fill( GameClient& client, uint64_t fingerprint )
{
	present::ViewFrame& f = Building();
	const GameClient::Stats& s = client.GetStats();
	f.state = ToString( client.State() );
	f.alphaAtPublish = client.TickAlpha();
	f.rate = client.State() == ClientState::Playing ? 1.0f : 0.0f;
	f.hasWorld = false;
	if ( f.mapHash != client.MapHash() )
	{
		f.mapHash = client.MapHash();
		f.mapName = client.Map().name;
		f.templateNames.clear();
		f.templateVisuals.clear();
		for ( const EntityTemplate& t : client.Map().templates )
		{
			f.templateNames.push_back( t.name );
			f.templateVisuals.push_back( t.visual );
		}
	}
	if ( f.schemaGeneration != client.SchemaGeneration() )
	{
		f.schemaGeneration = client.SchemaGeneration();
		f.schema = client.Schema();
	}
	if ( f.namesGeneration != client.NamesGeneration() )
	{
		f.namesGeneration = client.NamesGeneration();
		f.names = client.Names();
	}

	RollbackSession::Stats rollback;
	uint32_t confirmedTick = 0;
	uint32_t rollbackWindow = 0;
	if ( RollbackSession* session = client.Session() )
	{
		Simulation& sim = session->Sim();
		present::CaptureFrame( sim, f.frame );
		const InputFrame* last = session->LastSimulatedFrame();
		f.frame.hasInputs = last != nullptr;
		if ( last != nullptr )
		{
			f.frame.inputs = last->inputs;
		}
		f.frame.resetGeneration = client.ResetGeneration();
		f.frame.localNetId = sim.Globals().playerNetIds[client.Slot()];
		rollback = session->GetStats();
		confirmedTick = session->ConfirmedTick();
		rollbackWindow = session->MaxRollback();
		f.hasWorld = true;
	}

	f.stats.clear();
	f.stats.push_back( { "reject_reason", client.RejectReason() } );
	f.stats.push_back( { "tick", int64_t( client.CurrentTick() ) } );
	f.stats.push_back( { "confirmed_tick", int64_t( confirmedTick ) } );
	f.stats.push_back( { "rollback_window", int64_t( rollbackWindow ) } );
	f.stats.push_back( { "rtt_ms", int64_t( s.rttMs ) } );
	f.stats.push_back( { "clock_error", s.tickError } );
	f.stats.push_back( { "rate_scale", s.rateScale } );
	f.stats.push_back( { "rollbacks", int64_t( rollback.rollbacks ) } );
	f.stats.push_back( { "last_rollback_depth", int64_t( rollback.lastRollbackDepth ) } );
	f.stats.push_back( { "resimulated_ticks", int64_t( rollback.resimulatedTicks ) } );
	f.stats.push_back( { "stalled_seconds", s.stalledSeconds } );
	f.stats.push_back( { "checksums_verified", int64_t( s.checksumsVerified ) } );
	f.stats.push_back( { "desyncs", int64_t( s.desyncs ) } );
	f.stats.push_back( { "welcomes", int64_t( s.welcomes ) } );
	f.stats.push_back( { "connect_failures", int64_t( s.connectFailures ) } );
	f.stats.push_back( { "client_work_ms", s.simMsLastFrame } );
	f.stats.push_back( { "kbit_down_total", double( s.bytesReceived ) * 8.0 / 1000.0 } );
	f.stats.push_back( { "fingerprint", FingerprintText( fingerprint ) } );
	f.stats.push_back( { "fp_environment_ok", FpEnvironmentOk() } );
}

void LiveSource::Run()
{
	// Computed here first, under the simulation's FP environment; the server compares it.
	uint64_t fingerprint = BuildFingerprint();

	GameClient client;
	client.Start( m_options, present::ViewClock() );

	uint32_t lastTick = UINT32_MAX;
	uint64_t lastReset = UINT64_MAX;
	uint64_t lastFailures = 0;
	ClientState lastState = ClientState::Idle;
	bool published = false;
	while ( Stopping() == false )
	{
		client.Update( present::ViewClock(), [this]( uint32_t ) {
			std::lock_guard<std::mutex> lock( m_inputMutex );
			PlayerInput in = m_input;
			in.buttons |= m_latchedButtons;
			in.actions |= m_latchedActions;
			m_latchedButtons = 0;
			m_latchedActions = 0;
			return in;
		} );

		uint32_t tick = client.CurrentTick();
		bool rolledBack = client.GetStats().rolledBackLastFrame;
		bool changed = published == false || tick != lastTick || rolledBack || client.ResetGeneration() != lastReset ||
					   client.State() != lastState || client.GetStats().connectFailures != lastFailures;
		if ( changed )
		{
			Fill( client, fingerprint );
			Publish( rolledBack );
			published = true;
			lastTick = tick;
			lastReset = client.ResetGeneration();
			lastFailures = client.GetStats().connectFailures;
			lastState = client.State();
		}

		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
}

} // namespace cb
