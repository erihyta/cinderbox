#pragma once

// Client presentation: a second flecs world that mirrors the simulation for rendering.
//
// The simulation world is authoritative and gets rolled back; this world is never rolled back.
// Each frame Sync() diffs the simulation against it: new NetIds spawn visuals, vanished NetIds
// start a destroy effect, and poses are interpolated between the last two ticks. The small
// "scripts" (flecs systems in scripts/) animate spawning, destroying and the player body.

#include "simulation.h"

#include "flecs.h"
#include "raylib.h"

#include <cstdint>
#include <unordered_map>

namespace cb
{

class GameClient;

namespace present
{

// Which simulation entity this visual follows. Removed when the sim entity disappears.
struct SimLink
{
	uint32_t netId = 0;
};

enum class VisualKind : uint8_t
{
	Static,
	Prop,
	Player,
};

struct Visual
{
	VisualKind kind = VisualKind::Prop;
	ShapeKind shape = ShapeKind::Box;
	b3Vec3 halfExtents = { 0.5f, 0.5f, 0.5f };
	Color color = WHITE;
	PlayerSlot slot = 0;
	bool isLocalPlayer = false;
};

// Poses at the two most recent ticks, for interpolation.
struct TickPoses
{
	b3Vec3 prevPosition = {};
	b3Quat prevRotation = { { 0, 0, 0 }, 1 };
	b3Vec3 position = {};
	b3Quat rotation = { { 0, 0, 0 }, 1 };
	b3Vec3 velocity = {};
};

// Final pose used for drawing, written by Sync() and the scripts.
struct RenderPose
{
	Vector3 position = {};
	Quaternion rotation = { 0, 0, 0, 1 };
	Vector3 correction = {}; // decaying visual offset that hides rollback corrections
	float scale = 1.0f;
};

// Player locomotion values the animation scripts read (M3 feeds these into ozz).
struct PlayerMotion
{
	float groundSpeed = 0.0f;
	float verticalSpeed = 0.0f;
	bool grounded = true;
	bool sprinting = false;
	uint32_t airTicks = 0;
	uint32_t groundTicks = 0;
	float phase = 0.0f; // walk cycle phase in [0, 1)
};

// Script state
struct SpawnEffect
{
	float time = 0.0f;
};

struct DestroyEffect
{
	float time = 0.0f;
};

class Presentation
{
public:
	Presentation();

	// Mirror the client's simulation into the presentation world and run the scripts.
	void Update( GameClient& client, float frameSeconds );

	void Render();

	// Interpolated position of the local player's capsule center, if it exists.
	bool LocalPlayerPosition( Vector3& out ) const;

	flecs::world& World()
	{
		return m_world;
	}

private:
	void Sync( GameClient& client, float frameSeconds );
	flecs::entity CreateVisual( Simulation& sim, flecs::entity simEntity, uint32_t netId, bool withEffect );

	flecs::world m_world;
	struct Entry
	{
		flecs::entity_t entity;
		uint64_t stamp;
	};
	std::unordered_map<uint32_t, Entry> m_byNetId; // presentation only, order never matters
	uint64_t m_resetGeneration = UINT64_MAX;
	uint32_t m_lastTick = 0;
	flecs::entity m_localPlayer;
	uint64_t m_syncStamp = 0;
};

} // namespace present
} // namespace cb
