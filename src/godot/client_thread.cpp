#include "client_thread.h"

#include "fingerprint.h"
#include "simulation.h"

#if defined( __x86_64__ ) || defined( _M_X64 )
#include <xmmintrin.h>
#endif

namespace cb::gd
{

namespace
{

// Default SSE control state: all exceptions masked, round to nearest, no flush-to-zero and no
// denormals-are-zero. The simulation is only bit-exact under this environment.
constexpr unsigned kDefaultMxcsr = 0x1F80;

bool ResetFloatingPointEnvironment()
{
#if defined( __x86_64__ ) || defined( _M_X64 )
	_mm_setcsr( kDefaultMxcsr );
	return ( _mm_getcsr() & 0xFFC0 ) == ( kDefaultMxcsr & 0xFFC0 );
#else
	return true;
#endif
}

} // namespace

ClientThread::~ClientThread()
{
	Stop();
}

void ClientThread::Start( const ClientOptions& options )
{
	Stop();
	m_stop = false;
	m_start = std::chrono::steady_clock::now();
	m_thread = std::thread( &ClientThread::Run, this, options );
}

void ClientThread::Stop()
{
	if ( m_thread.joinable() )
	{
		m_stop = true;
		m_thread.join();
	}
}

double ClientThread::Now() const
{
	return std::chrono::duration<double>( std::chrono::steady_clock::now() - m_start ).count();
}

void ClientThread::SetInput( const PlayerInput& input )
{
	std::lock_guard<std::mutex> lock( m_inputMutex );
	m_input = input;
	m_latchedButtons |= uint8_t( input.buttons & ( BtnJump | BtnSpawnProp ) );
}

bool ClientThread::TakeFrame( PublishedFrame& out )
{
	std::lock_guard<std::mutex> lock( m_frameMutex );
	if ( m_latest.serial == out.serial )
	{
		return false;
	}
	out = m_latest;
	m_latestTaken = true;
	return true;
}

void ClientThread::Publish( GameClient& client, double now, bool rolledBack )
{
	PublishedFrame& f = m_building;
	f.stats = client.GetStats();
	f.state = client.State();
	f.rejectReason = client.RejectReason();
	f.publishedAt = now;
	f.alphaAtPublish = client.TickAlpha();
	f.hasSimulation = false;
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

	if ( RollbackSession* session = client.Session() )
	{
		Simulation& sim = session->Sim();
		present::CaptureFrame( sim, f.frame );
		f.frame.resetGeneration = client.ResetGeneration();
		f.frame.localNetId = sim.Globals().playerNetIds[client.Slot()];
		f.rollback = session->GetStats();
		f.currentTick = session->CurrentTick();
		f.confirmedTick = session->ConfirmedTick();
		f.rollbackWindow = session->MaxRollback();
		f.hasSimulation = true;
	}

	std::lock_guard<std::mutex> lock( m_frameMutex );
	// A rollback must not get lost if the main thread skipped the previous frame.
	f.frame.rolledBack = rolledBack || ( m_latestTaken == false && m_latest.frame.rolledBack );
	f.serial = m_latest.serial + 1;
	std::swap( m_latest, f );
	m_latestTaken = false;
}

void ClientThread::Run( ClientOptions options )
{
	bool fpOk = ResetFloatingPointEnvironment();
	// Computed here first, under the simulation's FP environment; the server compares it.
	uint64_t fingerprint = BuildFingerprint();
	m_building.fingerprint = fingerprint;
	m_building.fpEnvironmentOk = fpOk;

	GameClient client;
	client.Start( options, Now() );

	uint32_t lastTick = UINT32_MAX;
	uint64_t lastReset = UINT64_MAX;
	while ( m_stop.load() == false )
	{
		double now = Now();
		client.Update( now, [this]( uint32_t ) {
			std::lock_guard<std::mutex> lock( m_inputMutex );
			PlayerInput in = m_input;
			in.buttons |= m_latchedButtons;
			m_latchedButtons = 0;
			return in;
		} );

		uint32_t tick = client.CurrentTick();
		bool rolledBack = client.GetStats().rolledBackLastFrame;
		bool changed = tick != lastTick || rolledBack || client.ResetGeneration() != lastReset || client.State() != m_lastState;
		if ( changed )
		{
			m_building.fingerprint = fingerprint;
			m_building.fpEnvironmentOk = fpOk;
			Publish( client, now, rolledBack );
			lastTick = tick;
			lastReset = client.ResetGeneration();
			m_lastState = client.State();
		}

		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
}

} // namespace cb::gd
