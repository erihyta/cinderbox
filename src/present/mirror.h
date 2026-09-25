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
#include "pose_tools.h"

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
	uint32_t templateIndex = kNoTemplate;
	// Last step count seen for this character, to notice new ones.
	uint32_t stepCount = 0;
	b3Vec3 halfExtents = { 0.5f, 0.5f, 0.5f };
	// A dead player is not drawn; its ragdoll is.
	bool dead = false;
	// What the server's mods published about this entity, as of the newest frame.
	bool hasBoard = false;
	Blackboard board;
	// Ragdolls: the player it came from.
	uint32_t owner = 0;
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

// A ragdoll's parts at the last two ticks, and the pose built from them.
struct RagdollAnim
{
	float yaw = 0.0f;
	Transform previous[kRagdollParts];
	Transform current[kRagdollParts];
	float age = 0.0f; // seconds since it appeared
	// The pose its player was last drawn in, blended out over the first moments, so a character
	// never snaps from its animation into the ragdoll's standing start.
	bool hasStart = false;
	Models start;
	Models models;
};

// World singletons read by the scripts.
struct AnimLibrary
{
	std::shared_ptr<const anim::AnimSet> set;
	std::shared_ptr<const RagdollRig> ragdoll;
	std::shared_ptr<const anim::StanceTable> stances; // the server's layers and stances, for this set
	std::shared_ptr<const AnimGraph> graph;			  // the server's state machine for the character, or null
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
	Footstep,	// a player completed a stride
	Impact,		// two bodies hit hard enough for the simulation to record it
	Mod,		// a server mod announced something (modType indexes the schema's event names)
};

struct Event
{
	EventType type = EventType::Spawned;
	uint64_t visual = 0; // stable id of the visual for its whole lifetime
	uint32_t netId = 0;
	VisualKind kind = VisualKind::Prop;
	bool withEffect = false;
	b3Vec3 position = {};
	// Impact only: how fast the two bodies were approaching, so an effect can be picked by force.
	float strength = 0.0f;
	// Impact and Mod: the other entity, 0 when there is none.
	uint32_t otherNetId = 0;
	// Mod only: the event's type, value and vector.
	uint16_t modType = 0;
	int32_t value = 0;
	b3Vec3 vector = {};
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

	// Switch to another character's skeleton and clips (the server's character is known only once
	// it has welcomed us). Every player gets a new pose evaluator; the ragdoll rig follows.
	void SetAnimSet( std::shared_ptr<const anim::AnimSet> animSet, std::shared_ptr<const anim::StanceTable> stances = nullptr,
					 std::shared_ptr<const AnimGraph> graph = nullptr );

	// `tickAlpha`: how far between frame.tick - 1 and frame.tick to draw.
	void Update( const PresentationFrame& frame, float tickAlpha, float frameSeconds );

	// Events produced by the last Update().
	const std::vector<Event>& Events() const
	{
		return m_events;
	}

	// fn( uint64_t visual, const Visual&, const RenderPose&, const PlayerAnim*, const RagdollAnim* )
	template <typename Fn>
	void ForEach( Fn&& fn ) const
	{
		m_query.each( [&]( flecs::entity e, const Visual& v, const RenderPose& p ) {
			fn( e.id(), v, p, e.try_get<PlayerAnim>(), e.try_get<RagdollAnim>() );
		} );
	}

	// The global board, as of the newest frame.
	const int32_t* GlobalBoard() const
	{
		return m_board;
	}
	// The visual of an entity, if it has one.
	flecs::entity VisualOf( uint32_t netId ) const;
	// The newest ragdoll left by a player, if it still exists.
	flecs::entity RagdollOf( uint32_t playerNetId ) const;

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
	void SyncImpacts( const PresentationFrame& frame, bool reset );
	void SyncModEvents( const PresentationFrame& frame, bool reset );
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
	// Impacts already played, so a rollback or a skipped frame neither replays nor drops one.
	uint32_t m_impactCount = 0;
	uint32_t m_modEventCount = 0;
	int32_t m_board[kBoardSlots] = {};
	uint32_t m_lastTick = 0;
	flecs::entity m_localPlayer;
	uint64_t m_syncStamp = 0;
	// The frame's ragdolls while it is being synced (CreateVisual reads their owners).
	std::vector<FrameRagdoll> m_pendingRagdolls;
};

} // namespace cb::present
