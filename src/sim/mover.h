#pragma once

// The character mover: a kinematic capsule that walks, jumps and falls by its player's input.
// Move-and-slide with Box3D's mover casts and plane solver; a pogo spring keeps the capsule
// hovering above the ground, which carries it over steps and slopes; it pushes the dynamic bodies
// it touches. Everything it does is a function of the input, the state and the parameters
// (move_params.h), so it runs the same on the server and in every client's prediction.

#include "components.h"
#include "move_params.h"

#include "box3d/id.h"

namespace cb::mover
{

// The capsule every character has. Not a parameter: hitboxes, the camera and the pogo all go by it.
inline constexpr float kCapsuleRadius = 0.3f;
inline constexpr float kCapsuleHalfHeight = 0.5f; // center to sphere center

struct Body
{
	b3WorldId world;
	b3BodyId body;
	b3ShapeId shape; // the capsule: the mover does not collide with it
};

// One tick. `pressed` is the engine buttons that went down this tick; `tick` stamps a jump.
void Move( const Body& body, const MoveParams& params, float dt, uint32_t tick, const PlayerInput& in, uint8_t pressed, Character& c,
		   Transform& t );

} // namespace cb::mover
