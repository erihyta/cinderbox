#pragma once

// A simulation's state as a frame a viewer can be shown. The only place presentation touches a
// simulation: it is a library of its own (cb_capture), so a viewer that is only ever handed frames
// links no simulation at all.

#include "frame.h"

namespace cb
{
class Simulation;
}

namespace cb::present
{

// Fills everything the simulation knows; resetGeneration, rolledBack, localNetId and inputs are the
// caller's.
void CaptureFrame( Simulation& sim, PresentationFrame& out );

} // namespace cb::present
