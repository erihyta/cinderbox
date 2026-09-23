#pragma once

// Simulation components. These are snapshotted as raw bytes for rollback and hashing, so:
// - trivially copyable, no pointers (Box3D ids are plain indices and are fine),
// - no implicit padding (padding bytes are not guaranteed to be copied, which would corrupt the hash).
// Every component must be registered in Simulation::RegisterComponents().

#include "types.h"

#include "box3d/id.h"
#include "box3d/math_functions.h"

#include <cstdint>
#include <type_traits>

namespace cb
{

// Stable identity across server, clients and rollbacks. Never use flecs entity ids for this.
struct NetId
{
	uint32_t value = 0;
};

struct Transform
{
	b3Vec3 position = { 0.0f, 0.0f, 0.0f };
	b3Quat rotation = { { 0.0f, 0.0f, 0.0f }, 1.0f };
};

struct Velocity
{
	b3Vec3 linear = { 0.0f, 0.0f, 0.0f };
	b3Vec3 angular = { 0.0f, 0.0f, 0.0f };
};

enum class ShapeKind : uint8_t
{
	Box = 0,
	Sphere = 1,
	Capsule = 2,
};

// Collision and presentation geometry.
// Box: halfExtents. Sphere: x = radius. Capsule: x = radius, y = half distance between centers (along Y).
struct Shape
{
	ShapeKind kind = ShapeKind::Box;
	uint8_t reserved[3] = {};
	b3Vec3 halfExtents = { 0.5f, 0.5f, 0.5f };
};

struct PhysicsBody
{
	b3BodyId body = {};
	b3ShapeId shape = {};
};

struct Character
{
	b3Vec3 velocity = { 0.0f, 0.0f, 0.0f };
	float pogoVelocity = 0.0f;
	float facingYaw = 0.0f;
	uint8_t slot = 0;
	uint8_t grounded = 0;
	uint8_t prevButtons = 0;
	uint8_t sprinting = 0;
	// Ticks since the character last left the ground (0 while grounded). Drives jump/fall/land.
	uint32_t airTicks = 0;
	// Ticks since landing. Lets presentation play a land clip deterministically.
	uint32_t groundTicks = 0;
	// Tick of the most recent jump, 0 if never.
	uint32_t lastJumpTick = 0;
	// Distance walked since the last footstep, and how many steps this character has taken.
	// Presentation plays a step whenever the count changes.
	float stepDistance = 0.0f;
	uint32_t stepCount = 0;
	// Set by a Kill command, cleared by Respawn. A dead character takes no input, has its body
	// disabled, and is not drawn (its ragdoll, if it left one, is a separate entity).
	uint8_t dead = 0;
	// Set by a Freeze command: movement and jump inputs are ignored (between rounds, in a cutscene).
	uint8_t frozen = 0;
	uint8_t reserved[2] = {};
	// Times this character fell below the kill plane and was put back. Mods watch it to count the
	// fall as a death; the engine only rescues the character.
	uint32_t fallCount = 0;
};

struct Prop
{
	uint32_t owner = 0; // NetId of the spawning player, 0 for level props
	uint32_t spawnTick = 0;
	uint32_t despawnTick = 0; // 0 = never
};

enum class AnimMode : uint8_t
{
	Locomotion = 0, // idle / walk / run, blended by groundSpeed
	JumpStart = 1,
	Fall = 2,
	Land = 3,
};

// Deterministic animation controller state. The simulation only decides *what* plays and at which
// time; poses are sampled from it with ozz (client now, server too once gameplay needs them).
// Times are in seconds and independent of clip data, so the sim never depends on asset files.
struct AnimState
{
	AnimMode mode = AnimMode::Locomotion;
	AnimMode previousMode = AnimMode::Locomotion; // faded out over the first moments of `mode`
	uint8_t reserved[2] = {};
	float modeTime = 0.0f;		  // seconds since `mode` started
	float locomotionPhase = 0.0f; // [0, 1), shared by walk and run so their feet stay in sync
	float idleTime = 0.0f;		  // seconds, wraps every kAnimTimeWrap
	float groundSpeed = 0.0f;	  // smoothed horizontal speed (m/s) that drives the 1D blend
};

// Values a server mod published about an entity, for presentation to read by name. The schema
// (which slot is which field, and its type) travels to clients when they join; the simulation only
// stores what SetField commands write. Floats are stored as their bits.
struct Blackboard
{
	int32_t values[kBoardSlots] = {};
};

// A ragdoll left behind by a Kill command. One entity holds every body part: the bodies live in
// RagdollBodies, and their poses are mirrored into RagdollPose each tick like any other body.
inline constexpr int kRagdollParts = 11;

struct Ragdoll
{
	uint32_t owner = 0; // NetId of the player it came from (may no longer exist)
	PlayerSlot slot = 0;
	uint8_t reserved[3] = {};
	uint32_t spawnTick = 0;
	uint32_t despawnTick = 0; // 0 = never
	float yaw = 0.0f;		  // facing when it was created: the parts' rest orientation
};

struct RagdollBodies
{
	b3BodyId body[kRagdollParts] = {};
	b3ShapeId shape[kRagdollParts] = {};
};

struct RagdollPose
{
	Transform part[kRagdollParts];
	b3Vec3 linear[kRagdollParts] = {};
};

// Tag: part of the static level.
struct StaticGeometry
{
};

// "This entity did not come from a template."
inline constexpr uint32_t kNoTemplate = 0xFFFFFFFFu;

// The map template this entity was created from (see reflect.h). The simulation only carries it so
// that presentation can look up which prefab to draw.
struct TemplateRef
{
	uint32_t index = kNoTemplate;
};

#define CB_CHECK_COMPONENT( T, size )                                                                                            \
	static_assert( std::is_trivially_copyable_v<T> );                                                                            \
	static_assert( sizeof( T ) == size, #T " layout changed: check for padding" )

CB_CHECK_COMPONENT( NetId, 4 );
CB_CHECK_COMPONENT( Transform, 28 );
CB_CHECK_COMPONENT( Velocity, 24 );
CB_CHECK_COMPONENT( Shape, 16 );
CB_CHECK_COMPONENT( PhysicsBody, 16 );
CB_CHECK_COMPONENT( Character, 52 );
CB_CHECK_COMPONENT( Prop, 12 );
CB_CHECK_COMPONENT( AnimState, 20 );
CB_CHECK_COMPONENT( TemplateRef, 4 );
CB_CHECK_COMPONENT( Blackboard, 4 * kBoardSlots );
CB_CHECK_COMPONENT( Ragdoll, 20 );
CB_CHECK_COMPONENT( RagdollBodies, 16 * kRagdollParts );
CB_CHECK_COMPONENT( RagdollPose, 40 * kRagdollParts );

#undef CB_CHECK_COMPONENT

// Collision categories.
enum CollisionCategory : uint64_t
{
	CatStatic = 1 << 0,
	CatProp = 1 << 1,
	CatPlayer = 1 << 2,
	CatRagdoll = 1 << 3,
};

} // namespace cb
