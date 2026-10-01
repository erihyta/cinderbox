#include "replay_source.h"

#include "capture.h"
#include "fingerprint.h"
#include "map.h"
#include "replay_player.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace cb
{

ReplaySource::ReplaySource( const std::string& path )
	: m_path( path )
{
	StartThread();
}

ReplaySource::~ReplaySource()
{
	StopThread();
}

void ReplaySource::Control( const std::string& name, double value )
{
	std::lock_guard<std::mutex> lock( m_controlMutex );
	m_controls.emplace_back( name, value );
}

void ReplaySource::Fill( ReplayPlayer& player, float alpha )
{
	present::ViewFrame& f = Building();
	const net::ReplayReader& reader = player.Reader();
	Simulation& sim = player.Sim();
	uint32_t tick = player.Tick();
	double rate = double( reader.Config().tickRate );
	bool ended = tick >= player.Length();

	f.state = "playing";
	f.alphaAtPublish = alpha;
	f.rate = m_paused || ended ? 0.0f : float( m_speed );

	// The session never changes while a file plays.
	if ( f.schemaGeneration == 0 )
	{
		f.mapHash = MapHash( reader.MapBytes().data(), reader.MapBytes().size() );
		f.mapName = reader.Map().name;
		for ( const EntityTemplate& t : reader.Map().templates )
		{
			f.templateNames.push_back( t.name );
			f.templateVisuals.push_back( t.visual );
		}
		f.schema = reader.Schema();
		f.schemaGeneration = 1;
		// Recordings carry no names: viewers call players by their slot.
		f.namesGeneration = 1;
	}

	present::CaptureFrame( sim, f.frame );
	f.frame.hasInputs = tick > 0;
	if ( tick > 0 )
	{
		f.frame.inputs = reader.Frames()[tick - 1].inputs;
	}
	f.frame.resetGeneration = player.Generation();
	f.frame.localNetId = m_follow >= 0 ? sim.Globals().playerNetIds[m_follow] : 0;
	f.hasWorld = true;

	f.stats.clear();
	f.stats.push_back( { "reject_reason", std::string() } );
	f.stats.push_back( { "tick", int64_t( tick ) } );
	f.stats.push_back( { "checksums_verified", int64_t( player.ChecksumsVerified() ) } );
	f.stats.push_back( { "desyncs", int64_t( player.ChecksumFailures() ) } );
	f.stats.push_back( { "fingerprint", FingerprintText( reader.Fingerprint() ) } );
	f.stats.push_back( { "build_matches", reader.Fingerprint() == BuildFingerprint() } );
	f.stats.push_back( { "fp_environment_ok", FpEnvironmentOk() } );
	f.stats.push_back( { "replay_seconds", double( tick ) / rate } );
	f.stats.push_back( { "replay_length_seconds", double( player.Length() ) / rate } );
	f.stats.push_back( { "replay_speed", m_speed } );
	f.stats.push_back( { "replay_paused", m_paused } );
	f.stats.push_back( { "replay_ended", ended } );
	f.stats.push_back( { "replay_follow", int64_t( m_follow ) } );
}

void ReplaySource::Run()
{
	ReplayPlayer player;
	std::string error;
	if ( player.Open( m_path, error ) == false )
	{
		present::ViewFrame& f = Building();
		f.state = "rejected";
		f.stats.clear();
		f.stats.push_back( { "reject_reason", error } );
		Publish( false );
		while ( Stopping() == false )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		}
		return;
	}

	double tickRate = double( player.Reader().Config().tickRate );
	const std::vector<InputFrame>& frames = player.Reader().Frames();
	double last = present::ViewClock();
	uint32_t lastTick = UINT32_MAX;
	uint64_t lastGeneration = 0;
	std::vector<std::pair<std::string, double>> controls;

	while ( Stopping() == false )
	{
		controls.clear();
		{
			std::lock_guard<std::mutex> lock( m_controlMutex );
			controls.swap( m_controls );
		}
		bool changed = controls.empty() == false;
		for ( const auto& [name, value] : controls )
		{
			if ( std::isfinite( value ) == false )
			{
				continue;
			}
			double seconds = double( player.Tick() ) / tickRate;
			auto seekSeconds = [&]( double to ) { player.SeekTo( uint32_t( std::max( to, 0.0 ) * tickRate + 0.5 ) ); };
			if ( name == "pause" )
			{
				m_paused = value != 0.0;
			}
			else if ( name == "speed" )
			{
				m_speed = std::clamp( value, 0.125, 16.0 );
			}
			else if ( name == "seek" )
			{
				seekSeconds( value );
			}
			else if ( name == "skip" )
			{
				seekSeconds( seconds + value );
			}
			else if ( name == "step" )
			{
				m_paused = true;
				player.SeekTo( uint32_t( std::max( 0.0, double( player.Tick() ) + std::round( value ) ) ) );
			}
			else if ( name == "follow" )
			{
				m_autoFollow = false;
				m_follow = value >= 0.0 && value < double( kMaxPlayers ) ? int( value ) : -1;
			}
			else if ( name == "follow_next" )
			{
				m_autoFollow = true;
				m_follow = player.NextActiveSlot( m_follow );
			}
		}

		// The followed player's presses on the way, for the viewer's "pressed" feedback.
		double now = present::ViewClock();
		uint16_t pressed = 0;
		float alpha = player.Advance( m_paused ? 0.0 : ( now - last ) * m_speed, [&]( const InputFrame& frame ) {
			if ( m_follow >= 0 )
			{
				uint16_t before = frame.tick > 0 ? frames[frame.tick - 1].inputs[m_follow].actions : uint16_t( 0 );
				pressed |= uint16_t( frame.inputs[m_follow].actions & ~before );
			}
		} );
		last = now;

		if ( m_autoFollow && ( m_follow < 0 || player.Sim().IsPlayerActive( PlayerSlot( m_follow ) ) == false ) )
		{
			int next = player.NextActiveSlot( -1 );
			changed |= next != m_follow;
			m_follow = next;
		}

		if ( changed || player.Tick() != lastTick || player.Generation() != lastGeneration )
		{
			Fill( player, alpha );
			Publish( false, pressed );
			lastTick = player.Tick();
			lastGeneration = player.Generation();
		}

		std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
	}
}

} // namespace cb
