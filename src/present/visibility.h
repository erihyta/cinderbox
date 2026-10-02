#pragma once

// A frame with what one viewer must not be shown taken out: what a server does before it streams
// a frame to a client that only draws (fog of war, or simply "not near you"). Only a client that
// is sent frames can be kept in the dark; one that simulates the world has the world.

#include "frame.h"

#include <functional>

namespace cb::present
{

// Removes every entity `visible` says no to, except the frame's local player. With it go: the
// items it holds, its ragdoll's parts, and what the event rings say about it (those records are
// blanked, not removed: the rings are counted).
void KeepVisible( PresentationFrame& frame, const std::function<bool( const FrameEntity& )>& visible );

} // namespace cb::present
