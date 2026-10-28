#include "live_source.h"

#include "capture.h"
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

void LiveSource::SendText( const std::string& channel, const std::string& text )
{
	std::lock_guard<std::mutex> lock( m_inputMutex );
	if ( m_toSend.size() < 8 )
	{
		m_toSend.push_back( { channel, text } );
	}
}

void LiveSource::Fill( GameClient& client, uint64_t fingerprint )
{
	present::ViewFrame& f = Building();
	const GameClient::Stats& s = client.GetStats();
	f.state = ToString( client.State() );
	f.alphaAtPublish = client.TickAlpha();
	f.rate = client.State() == ClientState::Playing ? 1.0f : 0.0f;
	f.hasWorld = false;
	f.confirmedTick = 0;
	if ( f.mapHash != client.MapHash() )
	{
		f.mapHash = client.MapHash();
		f.mapName = client.Map().name;
		m_level.SetLevel( client.Map(), client.MapHash() );
		f.templateNames.clear();
		f.templateVisuals.clear();
		for ( const EntityTemplate& t : client.Map().templates )
		{
			f.templateNames.push_back( t.name );
			f.templateVisuals.push_back( TemplateLookName( t ) );
		}
	}
	if ( f.schemaGeneration != client.SchemaGeneration() )
	{
		f.schemaGeneration = client.SchemaGeneration();
		f.schema = client.Schema();
	}
	f.privates = client.Privates();
	if ( f.textCount != client.TextCount() )
	{
		f.textCount = client.TextCount();
		f.texts.clear();
		for ( const net::MsgText& t : client.Texts() )
		{
			f.texts.push_back( { t.channel, t.from, t.text } );
		}
	}
	if ( f.namesGeneration != client.NamesGeneration() )
	{
		f.namesGeneration = client.NamesGeneration();
		f.names = client.Names();
	}

	// Either network model's session: what the viewer is shown is the predicted world either way.
	RollbackSession::Stats rollback;
	uint32_t confirmedTick = 0;
	uint32_t rollbackWindow = 0;
	if ( Simulation* simulation = client.Sim() )
	{
		Simulation& sim = *simulation;
		present::CaptureFrame( sim, f.frame );
		const InputFrame* last = client.LastSimulatedFrame();
		f.frame.hasInputs = last != nullptr;
		if ( last != nullptr )
		{
			f.frame.inputs = last->inputs;
		}
		f.frame.resetGeneration = client.ResetGeneration();
		f.frame.localNetId = sim.Globals().playerNetIds[client.Slot()];
		if ( RollbackSession* session = client.Session() )
		{
			rollback = session->GetStats();
			rollbackWindow = session->MaxRollback();
		}
		else if ( StateSession* states = client.States() )
		{
			// (A state taken is the state model's rollback: back to the server's, inputs run again.)
			rollback.rollbacks = states->GetStats().states;
			rollback.lastRollbackDepth = states->GetStats().lastDepth;
			rollback.resimulatedTicks = states->GetStats().resimulatedTicks;
			rollbackWindow = states->MaxAhead();
		}
		confirmedTick = client.ConfirmedTick();
		f.confirmedTick = confirmedTick;
		f.hasWorld = true;
	}
	// The local player's motions, for the debug HUD (F1): what is on, and why a start was refused.
	std::string motions;
	if ( Simulation* sim = client.Sim() )
	{
		motions = sim->DescribeMotions( client.Slot() );
	}

	f.stats.clear();
	f.stats.push_back( { "reject_reason", client.RejectReason() } );
	f.stats.push_back( { "net_model", std::string( net::ToString( client.Model() ) ) } );
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
	f.stats.push_back( { "motions", motions } );
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

		std::vector<std::pair<std::string, std::string>> toSend;
		{
			std::lock_guard<std::mutex> lock( m_inputMutex );
			toSend.swap( m_toSend );
		}
		for ( const auto& [channel, text] : toSend )
		{
			client.SendText( channel, text );
		}

		uint32_t tick = client.CurrentTick();
		bool rolledBack = client.GetStats().rolledBackLastFrame;
		bool changed = published == false || tick != lastTick || rolledBack || client.ResetGeneration() != lastReset ||
					   client.TextCount() != Building().textCount ||
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
