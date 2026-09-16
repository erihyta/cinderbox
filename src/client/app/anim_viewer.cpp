#include "anim_viewer.h"

#include "anim_controller.h"
#include "presentation.h"

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace cb::present
{

namespace
{

struct Slot
{
	const char* label;
	int clip; // shown clip, -1 for the blend sweep
	AnimMode mode;
	float speed;	 // < 0: sweep idle -> run -> idle
	bool loopMode;	 // restart one-shot modes so they keep playing
	std::unique_ptr<anim::PoseEvaluator> eval;
	AnimState state;
};

} // namespace

int RunAnimViewer( std::shared_ptr<const anim::AnimSet> set, const AnimViewerOptions& options )
{
	using namespace anim_tuning;

	std::vector<Slot> slots;
	slots.push_back( { "idle", anim::ClipIdle, AnimMode::Locomotion, 0.0f, false, nullptr, {} } );
	slots.push_back( { "walk", anim::ClipWalk, AnimMode::Locomotion, kWalkSpeed, false, nullptr, {} } );
	slots.push_back( { "run", anim::ClipRun, AnimMode::Locomotion, kRunSpeed, false, nullptr, {} } );
	slots.push_back( { "speed sweep", -1, AnimMode::Locomotion, -1.0f, false, nullptr, {} } );
	slots.push_back( { "jump_start", anim::ClipJumpStart, AnimMode::JumpStart, 0.0f, true, nullptr, {} } );
	slots.push_back( { "fall", anim::ClipFall, AnimMode::Fall, 0.0f, false, nullptr, {} } );
	slots.push_back( { "land", anim::ClipLand, AnimMode::Land, 0.0f, true, nullptr, {} } );
	for ( Slot& s : slots )
	{
		s.eval = std::make_unique<anim::PoseEvaluator>( *set );
		s.state.mode = s.mode;
		s.state.previousMode = s.mode;
	}

	Camera3D camera{};
	camera.position = { 0.0f, 2.2f, 7.5f };
	camera.target = { 0.0f, 1.0f, 0.0f };
	camera.up = { 0.0f, 1.0f, 0.0f };
	camera.fovy = 50.0f;
	camera.projection = CAMERA_PERSPECTIVE;

	float yaw = options.yaw;
	float speedScale = 1.0f;
	bool paused = false;
	double start = GetTime();
	float sweepTime = 0.0f;

	while ( WindowShouldClose() == false )
	{
		float dt = paused ? 0.0f : GetFrameTime() * speedScale;
		if ( IsKeyDown( KEY_LEFT ) )
			yaw += 1.5f * GetFrameTime();
		if ( IsKeyDown( KEY_RIGHT ) )
			yaw -= 1.5f * GetFrameTime();
		if ( IsKeyPressed( KEY_SPACE ) )
			paused = !paused;
		if ( IsKeyPressed( KEY_UP ) )
			speedScale = std::fmin( speedScale * 2.0f, 4.0f );
		if ( IsKeyPressed( KEY_DOWN ) )
			speedScale = std::fmax( speedScale * 0.5f, 0.125f );

		sweepTime += dt;
		for ( Slot& s : slots )
		{
			AnimState& st = s.state;
			float speed = s.speed;
			if ( speed < 0.0f )
			{
				// 8 s round trip through idle, walk and run.
				float t = std::fmod( sweepTime, 8.0f ) / 8.0f;
				speed = ( kRunSpeed + 0.5f ) * ( t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t );
			}
			st.groundSpeed = speed;
			st.locomotionPhase += LocomotionCycleRate( speed ) * dt;
			st.locomotionPhase -= std::floor( st.locomotionPhase );
			st.idleTime = std::fmod( st.idleTime + dt, kTimeWrap );
			st.modeTime += dt;
			if ( s.loopMode )
			{
				// Replay one-shots after a short hold on the final pose.
				float length = set->Duration( anim::Clip( s.clip ) );
				if ( st.modeTime > length + 0.6f )
				{
					st.modeTime = 0.0f;
				}
			}
			// No crossfade in the viewer: show each clip on its own.
			st.previousMode = st.mode;
			s.eval->Evaluate( st );
		}

		BeginDrawing();
		ClearBackground( { 200, 215, 230, 255 } );
		BeginMode3D( camera );
		DrawPlane( { 0, 0, 0 }, { 20, 6 }, { 90, 95, 105, 255 } );
		DrawGrid( 20, 1.0f );
		Quaternion rot = QuaternionFromAxisAngle( { 0, 1, 0 }, yaw );
		for ( size_t i = 0; i < slots.size(); ++i )
		{
			float x = ( float( i ) - float( slots.size() - 1 ) * 0.5f ) * 1.7f;
			DrawSkeleton( { x, 0.0f, 0.0f }, rot, 1.0f, slots[i].eval.get(), Color{ uint8_t( 90 + 20 * i ), 120, 220, 255 } );
		}
		EndMode3D();

		for ( size_t i = 0; i < slots.size(); ++i )
		{
			float x = ( float( i ) - float( slots.size() - 1 ) * 0.5f ) * 1.7f;
			Vector2 p = GetWorldToScreen( { x, 2.15f, 0.0f }, camera );
			const Slot& s = slots[i];
			const char* text = s.speed < 0.0f ? TextFormat( "%s\n%.1f m/s", s.label, s.state.groundSpeed ) : s.label;
			int w = MeasureText( s.label, 18 );
			DrawText( text, int( p.x ) - w / 2, int( p.y ), 18, BLACK );
			if ( s.clip >= 0 && set->Get( anim::Clip( s.clip ) ) == nullptr )
			{
				DrawText( "(missing: rest pose)", int( p.x ) - w / 2, int( p.y ) + 20, 14, MAROON );
			}
		}
		DrawText( TextFormat( "Animation viewer - %s", set->Description().c_str() ), 10, 10, 20, BLACK );
		DrawText( TextFormat( "Left/Right rotate   Up/Down speed x%.3g   Space %s", speedScale, paused ? "resume" : "pause" ), 10, 34,
				  18, DARKGRAY );
		EndDrawing();

		if ( options.autoSeconds > 0.0 && GetTime() - start >= options.autoSeconds )
		{
			if ( options.screenshot.empty() == false )
			{
				int w = GetRenderWidth();
				int h = GetRenderHeight();
				unsigned char* pixels = rlReadScreenPixels( w, h );
				Image image = { pixels, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
				ExportImage( image, options.screenshot.c_str() );
				RL_FREE( pixels );
			}
			break;
		}
	}
	return 0;
}

} // namespace cb::present
