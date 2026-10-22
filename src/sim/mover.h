#pragma once

// The character mover: a kinematic capsule that walks, jumps and falls by its player's input.
// Move-and-slide with Box3D's mover casts and plane solver; a pogo spring keeps the capsule
// hovering above the ground, which carries it over steps and slopes; it pushes the dynamic bodies
// it touches. Everything it does is a function of the input, the state and the parameters
// (move_params.h), so it runs the same on the server and in every client's prediction.

#include "components.h"
#include "mod_schema.h"
#include "move_params.h"

#include "box3d/id.h"

namespace cb::mover
{

// The capsule a character has unless its scene says another (ModSchema::characters). Not a movement
// parameter: hitboxes, the camera and the pogo all go by it.
inline constexpr float kCapsuleRadius = kDefaultCapsuleRadius;
inline constexpr float kCapsuleHalfHeight = kDefaultCapsuleHalfHeight; // center to sphere center

struct Body
{
	b3WorldId world;
	b3BodyId body;
	b3ShapeId shape; // the capsule: the mover does not collide with it
	float radius = kCapsuleRadius;
	float halfHeight = kCapsuleHalfHeight;
};

// How far below a capsule's centre its feet are: the pogo holds the bottom sphere's centre three
// radii above the ground (a little less, for the spring's sag). The default capsule's is the number
// the game always had (ragdoll::kFeetBelowCenter), to the bit.
inline float FeetBelowCenter( float radius, float halfHeight )
{
	if ( radius == kCapsuleRadius && halfHeight == kCapsuleHalfHeight )
	{
		return 1.38f;
	}
	return halfHeight + 3.0f * radius - 0.02f;
}

// One tick. `pressed` is the engine buttons that went down this tick; `tick` stamps a jump.
void Move( const Body& body, const MoveParams& params, float dt, uint32_t tick, const PlayerInput& in, uint8_t pressed, Character& c,
		   Transform& t );

} // namespace cb::mover
