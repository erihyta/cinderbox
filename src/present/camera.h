#pragma once

// Camera collision for a third-person camera: how far a camera can back away from the point it
// looks at before the map is in the way.
//
// It only knows the frame's static geometry (the map), so a prop rolling past never makes the
// camera jump. It is presentation only: nothing in the simulation depends on the answer.

#include "frame.h"

namespace cb::present
{

// The distance a ball of `radius` can travel from `origin` along `direction` (unit length) before it
// touches static geometry, at most `maxDistance`. Boxes are tested grown by the radius, which is a
// little early at their corners and never late.
float CameraFreeDistance( const PresentationFrame& frame, b3Vec3 origin, b3Vec3 direction, float maxDistance, float radius );

} // namespace cb::present
