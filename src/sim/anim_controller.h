#pragma once

// What a character's state machine reads about how it moves, computed in the simulation tick for
// every character: its smoothed speed, which way the legs go, where it looks, and whether it is on
// the ground, jumping, falling or landing.

#include "components.h"
#include "types.h"

namespace cb
{

namespace anim_tuning
{
inline constexpr float kJumpStartSeconds = 0.25f; // how long a jump is "starting"
inline constexpr float kLandSeconds = 0.3f;		  // how long a landing lasts
inline constexpr float kFallDelaySeconds = 0.12f; // airborne this long (without jumping) => fall
inline constexpr float kSpeedSmoothing = 12.0f;	  // 1/s
inline constexpr float kMaxModeTime = 600.0f;
// Legs: turn toward the direction of travel at this rate (rad/s), from this ground speed (m/s).
// Past kBackwardAbove from the facing they walk backwards; they walk forwards again below
// kForwardBelow (the gap keeps a sideways walk from flipping every tick).
inline constexpr float kLegTurnRate = 10.0f;
inline constexpr float kLegMinSpeed = 0.3f;
inline constexpr float kBackwardAbove = 1.75f; // ~100 degrees
inline constexpr float kForwardBelow = 1.40f;  // ~80 degrees
} // namespace anim_tuning

// Advance `state` by one tick. `c` is the character after this tick's movement.
void UpdateAnimState( AnimState& state, const Character& c, const PlayerInput& input, uint32_t tick, float dt );

} // namespace cb
