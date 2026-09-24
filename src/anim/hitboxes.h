#pragma once

// A character's hitboxes: shapes attached to joints of its skeleton, each with a zone ("head",
// "torso", ...) that mods read when a ray hits it.
//
// Only the server tests them (gameplay mods run on the server, clients only present), and it poses
// the skeleton from the simulation's AnimState at the moment a mod casts a ray. So hit tests follow
// the pose players see, cost nothing per tick, and never enter the rolled-back simulation.

#include "anim_set.h"

#include "box3d/math_functions.h"

#include <string>
#include <vector>

namespace cb::anim
{

enum class HitShape : uint8_t
{
	Sphere,
	Capsule, // along the local Y axis, like Godot's CapsuleShape3D
	Box,
};

struct Hitbox
{
	std::string zone;
	std::string bone;
	int joint = -1; // set by BindHitboxes

	HitShape shape = HitShape::Sphere;
	// Placement in the joint's space, in skeleton units (the set's scale applies on top).
	b3Vec3 translation = {};
	b3Quat rotation = { { 0.0f, 0.0f, 0.0f }, 1.0f };
	float radius = 0.0f;		// sphere, capsule
	float height = 0.0f;		// capsule: total height, caps included
	b3Vec3 halfExtents = {};	// box
};

struct HitboxSet
{
	std::vector<Hitbox> boxes;
};

// hitboxes.cfg: one hitbox per line, '#' starts a comment.
//   <zone> <bone> sphere  tx ty tz  qx qy qz qw  radius
//   <zone> <bone> capsule tx ty tz  qx qy qz qw  radius height
//   <zone> <bone> box     tx ty tz  qx qy qz qw  hx hy hz
bool ParseHitboxes( const std::string& text, HitboxSet& out, std::string& error );
std::string FormatHitboxes( const HitboxSet& set );

// Resolves bone names to joints of `set`'s skeleton. Hitboxes on unknown bones are dropped and
// reported in `warnings`.
void BindHitboxes( HitboxSet& hitboxes, const AnimSet& set, std::string& warnings );

// Head, torso, arms and legs of the procedural placeholder rig (AnimSet::CreateProcedural).
HitboxSet DefaultHitboxes();

struct HitboxHit
{
	float fraction = 1.0f; // along the ray's translation
	b3Vec3 point = {};
	b3Vec3 normal = {};
	const Hitbox* box = nullptr;
};

// The nearest hitbox the ray origin + t * translation (t in [0, maxFraction]) enters. `models` are
// the pose's model-space joints (PoseEvaluator::Models()), placed with the feet at `feet` and turned
// by `rotation`, the way presentation places the skeleton.
bool RayHitboxes( const HitboxSet& hitboxes, const ozz::vector<ozz::math::Float4x4>& models, b3Vec3 feet, b3Quat rotation,
				  b3Vec3 origin, b3Vec3 translation, float maxFraction, HitboxHit& hit );

} // namespace cb::anim
