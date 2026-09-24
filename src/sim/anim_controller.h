#pragma once

// Deterministic locomotion state machine. Runs inside the simulation tick for every character.

#include "components.h"
#include "types.h"

namespace cb
{

namespace anim_tuning
{
// 1D blend points: idle at 0, walk clip at kWalkSpeed, run clip at kRunSpeed (m/s).
inline constexpr float kWalkSpeed = 3.0f;
inline constexpr float kRunSpeed = 6.5f;

// Duration of one full cycle (two steps) of the walk and run clips. Set these to your clip
// lengths so feet do not slide; they are simulation constants, so changing them changes hashes.
inline constexpr float kWalkCycleSeconds = 1.0f;
inline constexpr float kRunCycleSeconds = 0.7f;

inline constexpr float kModeFadeSeconds = 0.15f;  // crossfade between modes
inline constexpr float kJumpStartSeconds = 0.25f; // how long the jump-start pose plays
inline constexpr float kLandSeconds = 0.3f;		  // how long the landing pose plays
inline constexpr float kFallDelaySeconds = 0.12f; // airborne this long (without jumping) => fall
inline constexpr float kSpeedSmoothing = 12.0f;	  // 1/s
inline constexpr float kTimeWrap = 60.0f;		  // idle time wraps here to keep float precision
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

// Cycles per second of the synchronized walk/run phase at a given ground speed.
float LocomotionCycleRate( float groundSpeed );

} // namespace cb
