#include "replay_viewer.h"

#include "fingerprint.h"
#include "orbit_camera.h"
#include "presentation.h"
#include "replay_player.h"

#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <cstdio>

namespace cb::present
{

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
			follow = player.NextActiveSlot( follow );
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
			follow = player.NextActiveSlot( -1 );
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
