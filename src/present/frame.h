#pragma once

// A self-contained copy of what presentation needs from one simulation state. The simulation can
// live on another thread (Godot client) or in the same one (raylib client, replays); renderers
// only ever see frames.

#include "components.h"
#include "simulation.h"
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
	Ragdoll,
	Item, // held by a player, drawn in its socket
};

struct FrameEntity
{
	uint32_t netId = 0;
	VisualKind kind = VisualKind::Prop;
	ShapeKind shape = ShapeKind::Box;
	PlayerSlot slot = 0;
	bool hasAnim = false;
	// Map template this came from, or kNoTemplate. Presentation uses it to pick the prefab.
	uint32_t templateIndex = kNoTemplate;
	// Steps this character has taken; presentation plays one whenever it changes.
	uint32_t stepCount = 0;
	b3Vec3 halfExtents = {};
	Transform transform; // a ragdoll's is its pose frame (see RagdollFrame)
	b3Vec3 velocity = {};
	AnimState anim;
	// A dead character is not drawn (its ragdoll, if it left one, is its own entity).
	bool dead = false;
	// Values the server's mods published about this entity (names in the mod schema).
	bool hasBoard = false;
	Blackboard board;
	// Index into PresentationFrame::ragdolls, for ragdolls.
	uint32_t ragdoll = UINT32_MAX;
	// Items: who holds it, in which socket, and what it is (schema indices).
	uint32_t holder = 0;
	uint16_t itemKind = 0;
	uint8_t socket = 0;
};

struct FrameRagdoll
{
	uint32_t netId = 0;
	uint32_t owner = 0; // the player it came from
	PlayerSlot slot = 0;
	float yaw = 0.0f;
	Transform parts[kRagdollParts];
};

struct PresentationFrame
{
	uint32_t tick = 0;
	uint64_t resetGeneration = 0; // changes when the world was replaced (snap instead of smooth)
	bool rolledBack = false;	  // a rollback happened since the previous frame
	uint32_t localNetId = 0;	  // 0: no local player (e.g. replay free camera)
	float tickSeconds = 1.0f / 60.0f;
	std::vector<FrameEntity> entities; // sorted by netId
	// Impacts the simulation has recorded, and how many in total, so presentation can tell what it
	// missed between two frames. Copied straight out of the simulation's ring.
	uint32_t impactCount = 0;
	std::vector<ImpactRecord> impacts;
	// Same for mod events.
	uint32_t modEventCount = 0;
	std::vector<ModEventRecord> modEvents;
	std::vector<FrameRagdoll> ragdolls;
	// The global board.
	int32_t board[kBoardSlots] = {};
	// Inputs of the last simulated tick (predicted for other players), when the caller has them:
	// presentation aims arms from them.
	bool hasInputs = false;
	std::array<PlayerInput, kMaxPlayers> inputs{};
};

// Fills everything the simulation knows; resetGeneration, rolledBack, localNetId and inputs are the
// caller's.
void CaptureFrame( Simulation& sim, PresentationFrame& out );

} // namespace cb::present
