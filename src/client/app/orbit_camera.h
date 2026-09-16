#pragma once

// Third-person orbit camera. Yaw follows the detmath convention: 0 looks down +Z, positive turns left.

#include "raylib.h"
#include "raymath.h"

#include <cmath>

namespace cb::present
{

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

} // namespace cb::present
