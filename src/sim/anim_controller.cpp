#include "anim_controller.h"

#include "detmath.h"

#include <algorithm>

namespace cb
{

using namespace anim_tuning;

float LocomotionCycleRate( float groundSpeed )
{
	const float walkRate = 1.0f / kWalkCycleSeconds;
	const float runRate = 1.0f / kRunCycleSeconds;
	if ( groundSpeed <= kWalkSpeed )
	{
		// Slower than a walk: same stride length, fewer steps.
		return walkRate * ( groundSpeed / kWalkSpeed );
	}
	if ( groundSpeed >= kRunSpeed )
	{
		return runRate * ( groundSpeed / kRunSpeed );
	}
	float t = ( groundSpeed - kWalkSpeed ) / ( kRunSpeed - kWalkSpeed );
	return walkRate + ( runRate - walkRate ) * t;
}

void UpdateAnimState( AnimState& s, const Character& c, const PlayerInput& input, uint32_t tick, float dt )
{
	// Where the camera looks, relative to the body (kept up to date even while not aiming, so an
	// Aim command takes effect at once).
	s.aimYaw = detmath::WrapAngle( detmath::YawToRadians( input.cameraYaw ) - c.facingYaw );
	s.aimPitch = float( input.cameraPitch ) * ( detmath::kTwoPi / 65536.0f );

	float speed = b3Length( b3Vec3{ c.velocity.x, 0.0f, c.velocity.z } );
	if ( c.grounded == 0 )
	{
		// Keep the blend where it was while airborne so landing resumes smoothly.
		speed = s.groundSpeed;
	}
	s.groundSpeed += ( speed - s.groundSpeed ) * std::min( 1.0f, kSpeedSmoothing * dt );

	// Legs toward the direction of travel, relative to the facing; backwards past ~100 degrees.
	float legTarget = 0.0f;
	if ( c.grounded != 0 && speed > kLegMinSpeed )
	{
		float travel = detmath::WrapAngle( detmath::Atan2( c.velocity.x, c.velocity.z ) - c.facingYaw );
		float away = travel < 0.0f ? -travel : travel;
		if ( away > kBackwardAbove )
		{
			s.legsBackward = 1;
		}
		else if ( away < kForwardBelow )
		{
			s.legsBackward = 0;
		}
		legTarget = s.legsBackward != 0 ? detmath::WrapAngle( travel + detmath::kPi ) : travel;
		legTarget = std::clamp( legTarget, -0.5f * detmath::kPi, 0.5f * detmath::kPi );
	}
	else if ( c.grounded != 0 )
	{
		s.legsBackward = 0;
	}
	for ( float& t : s.layerTime )
	{
		t = std::min( t + dt, kMaxModeTime );
	}

	float legStep = kLegTurnRate * dt;
	s.legYaw += std::clamp( legTarget - s.legYaw, -legStep, legStep );

	bool jumpedRecently = c.lastJumpTick != 0 && float( tick - c.lastJumpTick ) * dt < kJumpStartSeconds;

	AnimMode next = s.mode;
	if ( c.grounded )
	{
		switch ( s.mode )
		{
			case AnimMode::Fall:
				next = AnimMode::Land;
				break;
			case AnimMode::JumpStart:
				// A real hop lands; a jump that got cancelled on the same tick does not.
				next = s.modeTime >= 0.1f ? AnimMode::Land : AnimMode::Locomotion;
				break;
			case AnimMode::Land:
				next = s.modeTime < kLandSeconds ? AnimMode::Land : AnimMode::Locomotion;
				break;
			case AnimMode::Locomotion:
				next = AnimMode::Locomotion;
				break;
		}
	}
	else
	{
		if ( jumpedRecently && s.mode != AnimMode::JumpStart )
		{
			next = AnimMode::JumpStart;
		}
		else if ( s.mode == AnimMode::JumpStart )
		{
			next = s.modeTime >= kJumpStartSeconds ? AnimMode::Fall : AnimMode::JumpStart;
		}
		else if ( s.mode == AnimMode::Locomotion || s.mode == AnimMode::Land )
		{
			// Short air time (walking off a step) keeps the locomotion pose.
			next = float( c.airTicks ) * dt >= kFallDelaySeconds ? AnimMode::Fall : s.mode;
		}
	}

	if ( next != s.mode )
	{
		s.previousMode = s.mode;
		s.mode = next;
		s.modeTime = 0.0f;
	}
	else if ( s.modeTime < kMaxModeTime )
	{
		s.modeTime += dt;
	}

	s.locomotionPhase += LocomotionCycleRate( s.groundSpeed ) * dt;
	while ( s.locomotionPhase >= 1.0f )
	{
		s.locomotionPhase -= 1.0f;
	}

	s.idleTime += dt;
	if ( s.idleTime >= kTimeWrap )
	{
		s.idleTime -= kTimeWrap;
	}
}

} // namespace cb
