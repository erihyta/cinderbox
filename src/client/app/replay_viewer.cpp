#include "replay_viewer.h"

#include "fingerprint.h"
#include "orbit_camera.h"
#include "presentation.h"
#include "replay.h"
#include "simulation.h"

#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace cb::present
{

namespace
{

constexpr uint32_t kKeyframeInterval = 300;

class ReplayPlayer
{
public:
	bool Open( const std::string& path, std::string& error )
	{
		if ( m_reader.Open( path, error ) == false )
		{
			return false;
		}
		m_sim = std::make_unique<Simulation>( m_reader.Config() );
		CaptureKeyframe();
		return true;
	}

	const net::ReplayReader& Reader() const
	{
		return m_reader;
	}
	Simulation& Sim()
	{
		return *m_sim;
	}
	uint32_t Tick() const
	{
		return m_sim->Tick();
	}
	uint32_t Length() const
	{
		return uint32_t( m_reader.Frames().size() );
	}
	uint64_t Generation() const
	{
		return m_generation;
	}
	uint64_t ChecksumFailures() const
	{
		return m_checksumFailures;
	}
	uint64_t ChecksumsVerified() const
	{
		return m_checksumsVerified;
	}

	// Advance playback time; returns the interpolation factor toward the next tick.
	float Advance( double seconds )
	{
		double dt = 1.0 / double( m_reader.Config().tickRate );
		m_accumulator += seconds;
		int steps = 0;
		while ( m_accumulator >= dt && Tick() < Length() && steps < 64 )
		{
			StepOne();
			m_accumulator -= dt;
			++steps;
		}
		if ( Tick() >= Length() )
		{
			m_accumulator = 0.0;
		}
		m_accumulator = std::min( m_accumulator, dt );
		return float( m_accumulator / dt );
	}

	void SeekTo( uint32_t target )
	{
		target = std::min( target, Length() );
		if ( target < Tick() )
		{
			auto it = m_keyframes.upper_bound( target );
			--it; // tick 0 is always there
			m_sim->Load( it->second );
		}
		while ( Tick() < target )
		{
			StepOne();
		}
		m_accumulator = 0.0;
		m_generation += 1;
	}

private:
	void StepOne()
	{
		m_sim->Step( m_reader.Frames()[Tick()] );
		CaptureKeyframe();
		VerifyChecksum();
	}

	void CaptureKeyframe()
	{
		uint32_t tick = Tick();
		if ( tick % kKeyframeInterval == 0 && m_keyframes.count( tick ) == 0 )
		{
			m_sim->Save( m_keyframes[tick] );
		}
	}

	void VerifyChecksum()
	{
		// Checksums are sorted by tick; only check each once (seeking replays ticks).
		const auto& list = m_reader.Checksums();
		auto it = std::lower_bound( list.begin(), list.end(), Tick(),
									[]( const net::MsgChecksum& c, uint32_t t ) { return c.tick < t; } );
		if ( it != list.end() && it->tick == Tick() && Tick() > m_highestVerified )
		{
			m_highestVerified = Tick();
			if ( m_sim->ComputeHash() == it->hash )
			{
				m_checksumsVerified += 1;
			}
			else
			{
				m_checksumFailures += 1;
				std::printf( "replay: checksum mismatch at tick %u\n", Tick() );
			}
		}
	}

	net::ReplayReader m_reader;
	std::unique_ptr<Simulation> m_sim;
	std::map<uint32_t, Snapshot> m_keyframes;
	double m_accumulator = 0.0;
	uint64_t m_generation = 1;
	uint64_t m_checksumsVerified = 0;
	uint64_t m_checksumFailures = 0;
	uint32_t m_highestVerified = 0;
};

// Next active player slot after `from`, or -1 if there is none.
int NextActiveSlot( const Simulation& sim, int from )
{
	for ( int i = 1; i <= kMaxPlayers; ++i )
	{
		int slot = ( from + i + kMaxPlayers ) % kMaxPlayers;
		if ( sim.IsPlayerActive( PlayerSlot( slot ) ) )
		{
			return slot;
		}
	}
	return -1;
}

} // namespace

int RunReplayViewer( std::shared_ptr<const anim::AnimSet> animSet, const ReplayViewerOptions& options )
{
	ReplayPlayer player;
	std::string error;
	if ( player.Open( options.path, error ) == false )
	{
		std::printf( "replay: %s\n", error.c_str() );
		return 1;
	}
	if ( player.Reader().Fingerprint() != BuildFingerprint() )
	{
		std::printf( "replay: recorded with a different simulation build, playback will diverge\n" );
	}
	uint32_t rate = player.Reader().Config().tickRate;
	std::printf( "replay: %u ticks (%.1f s)\n", player.Length(), double( player.Length() ) / double( rate ) );

	Presentation presentation( animSet );
	OrbitCamera camera;
	camera.distance = 9.0f;
	camera.pitch = 0.5f;
	double speed = 1.0;
	bool paused = false;
	int follow = -1;
	bool autoFollow = true;
	bool mouseCaptured = false;
	double started = GetTime();
	if ( options.startSeconds > 0.0 )
	{
		player.SeekTo( uint32_t( options.startSeconds * double( rate ) ) );
	}

	while ( WindowShouldClose() == false )
	{
		float frameSeconds = GetFrameTime();
		int seekTicks = 0;
		if ( IsKeyPressed( KEY_SPACE ) )
			paused = !paused;
		if ( IsKeyPressed( KEY_UP ) )
			speed = std::min( speed * 2.0, 16.0 );
		if ( IsKeyPressed( KEY_DOWN ) )
			speed = std::max( speed * 0.5, 0.125 );
		if ( IsKeyPressed( KEY_RIGHT ) )
			seekTicks = int( 5 * rate );
		if ( IsKeyPressed( KEY_LEFT ) )
			seekTicks = -int( 5 * rate );
		if ( IsKeyPressed( KEY_PERIOD ) )
			seekTicks = 1;
		if ( IsKeyPressed( KEY_COMMA ) )
			seekTicks = -1;
		if ( IsKeyPressed( KEY_HOME ) )
			seekTicks = -int( player.Tick() );
		if ( IsKeyPressed( KEY_TAB ) )
		{
			follow = NextActiveSlot( player.Sim(), follow );
			autoFollow = true;
		}
		if ( IsKeyPressed( KEY_BACKSPACE ) )
		{
			follow = -1;
			autoFollow = false;
		}
		if ( IsKeyPressed( KEY_ESCAPE ) )
		{
			mouseCaptured = !mouseCaptured;
			mouseCaptured ? DisableCursor() : EnableCursor();
		}

		if ( seekTicks != 0 )
		{
			player.SeekTo( uint32_t( std::max( 0, int( player.Tick() ) + seekTicks ) ) );
		}
		float alpha = player.Advance( paused ? 0.0 : frameSeconds * speed );

		if ( follow >= 0 && player.Sim().IsPlayerActive( PlayerSlot( follow ) ) == false )
		{
			follow = -1;
		}
		if ( follow < 0 && autoFollow )
		{
			follow = NextActiveSlot( player.Sim(), -1 );
		}

		SimView view;
		view.sim = &player.Sim();
		view.resetGeneration = player.Generation();
		view.tickAlpha = paused ? 0.0f : alpha;
		view.hasLocalPlayer = follow >= 0;
		view.localSlot = PlayerSlot( std::max( follow, 0 ) );
		presentation.Update( view, paused ? 0.0f : frameSeconds );

		camera.HandleInput( mouseCaptured || IsMouseButtonDown( MOUSE_BUTTON_RIGHT ) );
		Vector3 target;
		if ( follow >= 0 && presentation.LocalPlayerPosition( target ) )
		{
			camera.target = Vector3Add( target, { 0.0f, 0.4f, 0.0f } );
		}
		else
		{
			camera.target = { 0.0f, 1.0f, 0.0f };
		}

		BeginDrawing();
		ClearBackground( { 200, 215, 230, 255 } );
		BeginMode3D( camera.ToCamera() );
		presentation.Render();
		EndMode3D();

		double t = double( player.Tick() ) / double( rate );
		double total = double( player.Length() ) / double( rate );
		DrawText( TextFormat( "REPLAY  %6.1f / %.1f s   tick %u / %u   x%.3g %s", t, total, player.Tick(), player.Length(), speed,
							  paused ? "(paused)" : ( player.Tick() >= player.Length() ? "(end)" : "" ) ),
				  10, 10, 20, BLACK );
		DrawText( TextFormat( "following: %s   checksums ok %llu   mismatches %llu",
							  follow >= 0 ? TextFormat( "slot %d", follow ) : "free camera",
							  (unsigned long long)player.ChecksumsVerified(), (unsigned long long)player.ChecksumFailures() ),
				  10, 34, 18, player.ChecksumFailures() ? RED : DARKGRAY );
		DrawText( "Space pause  Up/Down speed  Left/Right -/+5 s  ,/. step  Home restart  Tab next player  Bksp free cam  RMB/Esc orbit", 10, 56,
				  16, DARKGRAY );
		// Progress bar
		int w = GetScreenWidth() - 20;
		DrawRectangle( 10, GetScreenHeight() - 16, w, 6, Fade( BLACK, 0.2f ) );
		DrawRectangle( 10, GetScreenHeight() - 16, int( double( w ) * ( total > 0.0 ? t / total : 0.0 ) ), 6, DARKBLUE );
		EndDrawing();

		if ( options.autoSeconds > 0.0 && GetTime() - started >= options.autoSeconds )
		{
			if ( options.screenshot.empty() == false )
			{
				int rw = GetRenderWidth();
				int rh = GetRenderHeight();
				unsigned char* pixels = rlReadScreenPixels( rw, rh );
				Image image = { pixels, rw, rh, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
				ExportImage( image, options.screenshot.c_str() );
				RL_FREE( pixels );
			}
			break;
		}
	}
	return player.ChecksumFailures() == 0 ? 0 : 2;
}

} // namespace cb::present
