#pragma once

// Renderer-independent client presentation: a flecs world that mirrors presentation frames.
//
// The simulation is authoritative and gets rolled back; this world never is. Each Update() diffs a
// frame against it: new NetIds get visuals, vanished NetIds start a destroy effect, poses are
// interpolated between the last two ticks and rollback corrections fade out. The "scripts" (flecs
// systems in scripts/) animate spawning and destroying and evaluate ozz poses. Renderers (raylib,
// Godot) read the result through ForEach() and react to Events() for VFX and node lifetime.

#include "frame.h"
#include "pose.h"

#include "flecs.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace cb::present
{

// Which frame entity this visual follows. Removed when the entity disappears from the frames.
struct SimLink
{
	uint32_t netId = 0;
};

struct Visual
{
	uint32_t netId = 0; // kept after the SimLink is gone (destroy effect)
	VisualKind kind = VisualKind::Prop;
	ShapeKind shape = ShapeKind::Box;
	PlayerSlot slot = 0;
	bool isLocalPlayer = false;
	b3Vec3 halfExtents = { 0.5f, 0.5f, 0.5f };
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

// Final pose for drawing, written by the sync and the scripts.
struct RenderPose
{
	b3Vec3 position = {};
	b3Quat rotation = { { 0, 0, 0 }, 1 };
	b3Vec3 correction = {}; // decaying offset that hides rollback corrections
	float scale = 1.0f;
};

// Player animation: AnimState at the last two ticks and the evaluator producing the pose.
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
	float tickAlpha = 0.0f;
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

enum class EventType : uint8_t
{
	Spawned,	// a visual appeared (withEffect: it was not just part of a world reset)
	Destroying, // its entity left the simulation; the visual plays out its destroy effect
	Removed,	// the visual is gone; renderers free their node
	Jumped,		// a player entered the jump-start pose (predicted, may be rolled back)
	Landed,		// a player entered the landing pose
};

struct Event
{
	EventType type = EventType::Spawned;
	uint64_t visual = 0; // stable id of the visual for its whole lifetime
	uint32_t netId = 0;
	VisualKind kind = VisualKind::Prop;
	bool withEffect = false;
	b3Vec3 position = {};
};

// Capsule center to the ground while standing (the skeleton origin is at the feet).
inline constexpr float kFeetOffset = 1.38f;

class Mirror
{
public:
	explicit Mirror( std::shared_ptr<const anim::AnimSet> animSet );
	~Mirror();
	Mirror( const Mirror& ) = delete;
	Mirror& operator=( const Mirror& ) = delete;

	// `tickAlpha`: how far between frame.tick - 1 and frame.tick to draw.
	void Update( const PresentationFrame& frame, float tickAlpha, float frameSeconds );

	// Events produced by the last Update().
	const std::vector<Event>& Events() const
	{
		return m_events;
	}

	// fn( uint64_t visual, const Visual&, const RenderPose&, const PlayerAnim* )
	template <typename Fn>
	void ForEach( Fn&& fn ) const
	{
		m_query.each( [&]( flecs::entity e, const Visual& v, const RenderPose& p ) { fn( e.id(), v, p, e.try_get<PlayerAnim>() ); } );
	}

	// The local player's render pose, if there is one.
	bool LocalPlayer( RenderPose& out ) const;

	const anim::AnimSet& AnimSet() const
	{
		return *m_animSet;
	}
	flecs::world& World()
	{
		return m_world;
	}

	// Used by the removal observer.
	void PushEvent( const Event& e )
	{
		m_events.push_back( e );
	}

private:
	void Sync( const PresentationFrame& frame, float tickAlpha, float frameSeconds );
	flecs::entity CreateVisual( const FrameEntity& f, bool withEffect );

	std::vector<Event> m_events; // declared before the world: observers write to it on teardown
	std::shared_ptr<const anim::AnimSet> m_animSet;
	flecs::world m_world;
	flecs::query<const Visual, const RenderPose> m_query;

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

} // namespace cb::present
