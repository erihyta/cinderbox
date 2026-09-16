#pragma once

// Client presentation: a second flecs world that mirrors the simulation for rendering.
//
// The simulation world is authoritative and gets rolled back; this world is never rolled back.
// Each frame Sync() diffs the simulation against it: new NetIds spawn visuals, vanished NetIds
// start a destroy effect, and poses are interpolated between the last two ticks. The small
// "scripts" (flecs systems in scripts/) animate spawning, destroying and the player body.

#include "pose.h"
#include "simulation.h"

#include "flecs.h"
#include "raylib.h"

#include <cstdint>
#include <memory>
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

// Player animation: the simulation's AnimState at the last two ticks, and the ozz evaluator
// that turns the interpolated state into a pose (see scripts/player_animation.cpp).
struct PlayerAnim
{
	AnimState previous;
	AnimState current;
	std::shared_ptr<anim::PoseEvaluator> evaluator;
};

// World singletons read by the scripts.
struct AnimLibrary
{
	std::shared_ptr<const anim::AnimSet> set;
};

struct FrameTiming
{
	float tickAlpha = 0.0f; // interpolation factor between the last two ticks
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

// Draws an evaluated pose as one box per bone. `feet` is where the skeleton origin goes.
void DrawSkeleton( Vector3 feet, Quaternion rotation, float scale, const anim::PoseEvaluator* eval, Color color );

class Presentation
{
public:
	explicit Presentation( std::shared_ptr<const anim::AnimSet> animSet );

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
	std::shared_ptr<const anim::AnimSet> m_animSet;
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
