// Cinderbox client.
//
//   cb_client [--host ADDRESS] [--port N] [--rollback TICKS] [--width W] [--height H]
//             [--autoplay SECONDS] [--screenshot FILE]
//
// --autoplay drives the player with scripted input for the given time after joining, optionally
// saves a screenshot, and exits with 0 if the session stayed in sync (smoke test).
//
// Controls: WASD move, Shift sprint, Space jump, F spawn a prop, mouse orbit, wheel zoom,
// Esc toggles the mouse cursor, F1 toggles the debug HUD.

#include "game_client.h"
#include "presentation.h"

#include "detmath.h"
#include "util.h"

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace cb;

namespace
{

struct AppOptions
{
	ClientOptions client;
	int width = 1280;
	int height = 720;
	double autoplaySeconds = 0.0;
	std::string screenshot;
};

bool ParseArgs( int argc, char** argv, AppOptions& o )
{
	for ( int i = 1; i < argc; ++i )
	{
		std::string arg = argv[i];
		if ( i + 1 >= argc )
		{
			std::printf( "missing value for %s\n", arg.c_str() );
			return false;
		}
		std::string value = argv[++i];
		if ( arg == "--host" )
			o.client.host = value;
		else if ( arg == "--port" )
			o.client.port = uint16_t( std::strtoul( value.c_str(), nullptr, 10 ) );
		else if ( arg == "--rollback" )
			o.client.maxRollbackTicks = uint32_t( std::strtoul( value.c_str(), nullptr, 10 ) );
		else if ( arg == "--width" )
			o.width = std::atoi( value.c_str() );
		else if ( arg == "--height" )
			o.height = std::atoi( value.c_str() );
		else if ( arg == "--autoplay" )
			o.autoplaySeconds = std::atof( value.c_str() );
		else if ( arg == "--screenshot" )
			o.screenshot = value;
		else
		{
			std::printf( "unknown option %s\n", arg.c_str() );
			return false;
		}
	}
	return true;
}

struct OrbitCamera
{
	float yaw = 0.0f; // detmath convention: 0 looks down +Z, positive turns left
	float pitch = 0.35f;
	float distance = 6.0f;
	Vector3 target = { 0.0f, 1.0f, 0.0f };

	void HandleInput( bool mouseCaptured )
	{
		if ( mouseCaptured )
		{
			Vector2 d = GetMouseDelta();
			yaw -= d.x * 0.003f;
			pitch = Clamp( pitch + d.y * 0.003f, -0.4f, 1.3f );
		}
		distance = Clamp( distance - GetMouseWheelMove() * 0.5f, 2.0f, 20.0f );
	}

	Camera3D ToCamera() const
	{
		Vector3 forward = { std::sin( yaw ) * std::cos( pitch ), -std::sin( pitch ), std::cos( yaw ) * std::cos( pitch ) };
		Camera3D cam{};
		cam.target = target;
		cam.position = Vector3Subtract( target, Vector3Scale( forward, distance ) );
		cam.up = { 0.0f, 1.0f, 0.0f };
		cam.fovy = 55.0f;
		cam.projection = CAMERA_PERSPECTIVE;
		return cam;
	}
};

PlayerInput SampleInput( const OrbitCamera& camera, bool hasFocus )
{
	PlayerInput in;
	in.cameraYaw = detmath::RadiansToYaw( camera.yaw );
	if ( hasFocus == false )
	{
		return in;
	}

	int forward = ( IsKeyDown( KEY_W ) ? 1 : 0 ) - ( IsKeyDown( KEY_S ) ? 1 : 0 );
	int right = ( IsKeyDown( KEY_D ) ? 1 : 0 ) - ( IsKeyDown( KEY_A ) ? 1 : 0 );
	in.moveForward = int8_t( forward * 127 );
	in.moveRight = int8_t( right * 127 );
	if ( IsKeyDown( KEY_SPACE ) )
	{
		in.buttons |= BtnJump;
	}
	if ( IsKeyDown( KEY_LEFT_SHIFT ) || IsKeyDown( KEY_RIGHT_SHIFT ) )
	{
		in.buttons |= BtnSprint;
	}
	if ( IsKeyDown( KEY_F ) )
	{
		in.buttons |= BtnSpawnProp;
	}
	return in;
}

void DrawHud( GameClient& client, bool showDebug )
{
	int y = 10;
	auto line = [&]( Color color, const char* fmt, auto... args ) {
		DrawText( TextFormat( fmt, args... ), 10, y, 18, color );
		y += 20;
	};

	ClientState state = client.State();
	if ( state != ClientState::Playing )
	{
		const char* msg = state == ClientState::Rejected ? TextFormat( "Rejected: %s", client.RejectReason().c_str() )
						  : client.HasJoined()			 ? "Connection lost - time is paused, reconnecting..."
														 : "Connecting...";
		int w = MeasureText( msg, 28 );
		DrawRectangle( GetScreenWidth() / 2 - w / 2 - 16, GetScreenHeight() / 2 - 30, w + 32, 60, Fade( BLACK, 0.7f ) );
		DrawText( msg, GetScreenWidth() / 2 - w / 2, GetScreenHeight() / 2 - 14, 28, state == ClientState::Rejected ? RED : ORANGE );
	}

	if ( showDebug == false )
	{
		line( DARKGRAY, "F1: debug info" );
		return;
	}

	const auto& s = client.GetStats();
	line( BLACK, "%d FPS   %s   slot %u", GetFPS(), ToString( state ), client.Slot() );
	if ( RollbackSession* session = client.Session() )
	{
		const auto& rs = session->GetStats();
		uint32_t current = session->CurrentTick();
		uint32_t confirmed = session->ConfirmedTick();
		line( BLACK, "tick %u   confirmed %u   predicting %d ahead", current, confirmed, int( current ) - int( confirmed ) );
		line( BLACK, "rtt %u ms   clock error %+.1f ticks   rate x%.3f", s.rttMs, s.tickError, s.rateScale );
		line( BLACK, "rollbacks %llu (last depth %u)   resimulated %llu   stalls %llu", (unsigned long long)rs.rollbacks,
			  rs.lastRollbackDepth, (unsigned long long)rs.resimulatedTicks, (unsigned long long)rs.stalls );
		line( s.desyncs ? RED : BLACK, "checksums ok %llu   desyncs %llu   welcomes %llu", (unsigned long long)s.checksumsVerified,
			  (unsigned long long)s.desyncs, (unsigned long long)s.welcomes );
		line( BLACK, "entities %zu   physics %zu KB", session->Sim().Entities().size(), session->Sim().PhysicsBytesInUse() / 1024 );
	}
	y += 6;
	line( DARKGRAY, "WASD move  Shift sprint  Space jump  F spawn prop" );
	line( DARKGRAY, "Mouse orbit  Wheel zoom  Esc cursor  F1 hide" );
}

} // namespace

int main( int argc, char** argv )
{
	AppOptions options;
	if ( ParseArgs( argc, argv, options ) == false )
	{
		std::printf( "usage: cb_client [--host ADDRESS] [--port N] [--rollback TICKS] [--width W] [--height H]\n"
					 "                 [--autoplay SECONDS] [--screenshot FILE]\n" );
		return 1;
	}

	SetConfigFlags( FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI );
	SetTraceLogLevel( LOG_WARNING );
	InitWindow( options.width, options.height, "Cinderbox" );
	SetExitKey( KEY_NULL );

	GameClient client;
	client.Start( options.client, GetTime() );
	present::Presentation presentation;
	OrbitCamera camera;

	bool autoplay = options.autoplaySeconds > 0.0;
	bool mouseCaptured = !autoplay;
	bool showDebug = true;
	if ( mouseCaptured )
	{
		DisableCursor();
	}
	uint64_t autoRng = uint64_t( GetRandomValue( 1, 1 << 30 ) );
	PlayerInput autoInput{};
	double playingSince = -1.0;

	while ( WindowShouldClose() == false )
	{
		if ( IsKeyPressed( KEY_ESCAPE ) )
		{
			mouseCaptured = !mouseCaptured;
			if ( mouseCaptured )
				DisableCursor();
			else
				EnableCursor();
		}
		if ( IsMouseButtonPressed( MOUSE_BUTTON_LEFT ) && mouseCaptured == false )
		{
			mouseCaptured = true;
			DisableCursor();
		}
		if ( IsKeyPressed( KEY_F1 ) )
		{
			showDebug = !showDebug;
		}

		camera.HandleInput( mouseCaptured );

		bool focused = IsWindowFocused();
		client.Update( GetTime(), [&]( uint32_t ) {
			if ( autoplay == false )
			{
				return SampleInput( camera, focused );
			}
			// Scripted player: runs around, jumps, spawns props, turns the camera.
			uint64_t r = NextRandom( autoRng );
			if ( ( r & 63 ) == 0 )
			{
				autoInput.moveForward = int8_t( ( r >> 8 ) % 3 == 0 ? 0 : 127 );
				autoInput.moveRight = int8_t( int( ( r >> 16 ) % 3 ) * 127 - 127 );
			}
			autoInput.buttons = BtnSprint;
			if ( ( ( r >> 24 ) % 45 ) == 0 )
			{
				autoInput.buttons |= BtnJump;
			}
			if ( ( ( r >> 32 ) % 20 ) == 0 )
			{
				autoInput.buttons |= BtnSpawnProp;
			}
			camera.yaw += 0.01f;
			autoInput.cameraYaw = detmath::RadiansToYaw( camera.yaw );
			return autoInput;
		} );

		if ( autoplay && client.State() == ClientState::Playing && playingSince < 0.0 )
		{
			playingSince = GetTime();
		}
		if ( autoplay && client.State() == ClientState::Rejected )
		{
			break;
		}
		bool autoplayDone = autoplay && playingSince >= 0.0 && GetTime() - playingSince >= options.autoplaySeconds;

		float frameSeconds = GetFrameTime();
		presentation.Update( client, frameSeconds );

		Vector3 playerPos;
		if ( presentation.LocalPlayerPosition( playerPos ) )
		{
			camera.target = Vector3Add( playerPos, { 0.0f, 0.4f, 0.0f } );
		}

		BeginDrawing();
		ClearBackground( { 200, 215, 230, 255 } );
		BeginMode3D( camera.ToCamera() );
		presentation.Render();
		EndMode3D();
		DrawHud( client, showDebug );
		EndDrawing();

		if ( autoplayDone )
		{
			if ( options.screenshot.empty() == false )
			{
				// TakeScreenshot over-reads under display scaling in raylib 5.5; read the real size.
				int w = GetRenderWidth();
				int h = GetRenderHeight();
				unsigned char* pixels = rlReadScreenPixels( w, h );
				Image image = { pixels, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
				ExportImage( image, options.screenshot.c_str() );
				RL_FREE( pixels );
			}
			const auto& s = client.GetStats();
			std::printf( "autoplay done: %s, checksums ok %llu, desyncs %llu\n", ToString( client.State() ),
						 (unsigned long long)s.checksumsVerified, (unsigned long long)s.desyncs );
			break;
		}
	}

	bool healthy = client.State() == ClientState::Playing && client.GetStats().desyncs == 0;
	CloseWindow();
	return autoplay && !healthy ? 2 : 0;
}
