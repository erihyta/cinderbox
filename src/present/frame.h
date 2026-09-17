#pragma once

// A self-contained copy of what presentation needs from one simulation state. The simulation can
// live on another thread (Godot client) or in the same one (raylib client, replays); renderers
// only ever see frames.

#include "components.h"
#include "types.h"

#include <cstdint>
#include <vector>

namespace cb
{
class Simulation;
}

namespace cb::present
{

enum class VisualKind : uint8_t
{
	Static,
	Prop,
	Player,
};

struct FrameEntity
{
	uint32_t netId = 0;
	VisualKind kind = VisualKind::Prop;
	ShapeKind shape = ShapeKind::Box;
	PlayerSlot slot = 0;
	bool hasAnim = false;
	b3Vec3 halfExtents = {};
	Transform transform;
	b3Vec3 velocity = {};
	AnimState anim;
};

struct PresentationFrame
{
	uint32_t tick = 0;
	uint64_t resetGeneration = 0; // changes when the world was replaced (snap instead of smooth)
	bool rolledBack = false;	  // a rollback happened since the previous frame
	uint32_t localNetId = 0;	  // 0: no local player (e.g. replay free camera)
	float tickSeconds = 1.0f / 60.0f;
	std::vector<FrameEntity> entities; // sorted by netId
};

// Fills `out.tick` and `out.entities`; the other fields are the caller's.
void CaptureFrame( Simulation& sim, PresentationFrame& out );

} // namespace cb::present
