#pragma once

// Simulation components. These are snapshotted as raw bytes for rollback and hashing, so:
// - trivially copyable, no pointers (Box3D ids are plain indices and are fine),
// - no implicit padding (padding bytes are not guaranteed to be copied, which would corrupt the hash).
// Every component must be registered in Simulation::RegisterComponents().

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
};

struct Prop
{
	uint32_t owner = 0; // NetId of the spawning player, 0 for level props
	uint32_t spawnTick = 0;
	uint32_t despawnTick = 0; // 0 = never
};

// Tag: part of the static level.
struct StaticGeometry
{
};

#define CB_CHECK_COMPONENT( T, size )                                                                                            \
	static_assert( std::is_trivially_copyable_v<T> );                                                                            \
	static_assert( sizeof( T ) == size, #T " layout changed: check for padding" )

CB_CHECK_COMPONENT( NetId, 4 );
CB_CHECK_COMPONENT( Transform, 28 );
CB_CHECK_COMPONENT( Velocity, 24 );
CB_CHECK_COMPONENT( Shape, 16 );
CB_CHECK_COMPONENT( PhysicsBody, 16 );
CB_CHECK_COMPONENT( Character, 36 );
CB_CHECK_COMPONENT( Prop, 12 );

#undef CB_CHECK_COMPONENT

// Collision categories.
enum CollisionCategory : uint64_t
{
	CatStatic = 1 << 0,
	CatProp = 1 << 1,
	CatPlayer = 1 << 2,
};

} // namespace cb
