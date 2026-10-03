#pragma once

// Joint-level operations on model-space poses (the layout PoseEvaluator produces: feet at the
// origin, facing +Z, the set's scale applied). Shared by the pose itself (aiming, which the server's
// hit tests see too) and by presentation (ragdolls, blends).

#include "anim_set.h"

#include "box3d/math_functions.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"

#include <utility>
#include <vector>

namespace cb::anim
{

using Models = ozz::vector<ozz::math::Float4x4>;

void Decompose( const ozz::math::Float4x4& m, b3Vec3& position, b3Quat& rotation, float& scale );
ozz::math::Float4x4 Compose( b3Vec3 position, b3Quat rotation, float scale );
// Normalized lerp along the shorter way.
b3Quat Nlerp( b3Quat a, b3Quat b, float t );
// The shortest rotation taking unit vector `from` onto unit vector `to`.
b3Quat Arc( b3Vec3 from, b3Vec3 to );

// Index of a joint by its humanoid-profile name (Mixamo names match too), -1 if absent.
int FindJoint( const AnimSet& set, const char* profileName );

// How high above the feet a first-person camera sits: the head joint of the rest pose plus
// kEyeUp. It does not follow the animation, so walking, landing and bowing do not move it.
float EyeHeight( const AnimSet& set );

// Rotates `joint` and everything below it about the joint's position.
void RotateSubtree( const AnimSet& set, Models& models, int joint, b3Quat turn );

// Rotates `joint` and everything below it about `pivot` (model space).
void RotateSubtreeAbout( const AnimSet& set, Models& models, int joint, b3Vec3 pivot, b3Quat turn );

// Moves `joint` and everything below it by `offset` (model space).
void TranslateSubtree( const AnimSet& set, Models& models, int joint, b3Vec3 offset );

// Turns each joint of `chain` in order, and everything below it about it, so the line from it to
// `tip` points along `direction` (model space, normalized) by its weight: 0 leaves the pose alone,
// 1 aims fully. A chain like { UpperChest 0.3, RightUpperArm 1 } leans the chest a little and
// then points the arm exactly.
void AimChain( const AnimSet& set, Models& models, const std::vector<std::pair<int, float>>& chain, int tip, b3Vec3 direction );

// An item carried in one hand that the other hand holds too (ItemShape::grip): where, in the
// item's frame, which is the carrying hand's socket frame (AnimSet::HandSocketOf).
struct HandGrip
{
	bool leftCarries = false; // the item is in the left hand: the right hand reaches for it
	bool align = false;		  // the reaching hand also takes `rotation`
	// The grip is not a place on the item: the other hand stays where the animation has it
	// relative to the carrying hand (AsAnimated says where that is, before the carrying arm is aimed).
	bool asAnimated = false;
	b3Vec3 position = { 0.0f, 0.0f, 0.0f };
	b3Quat rotation = { { 0.0f, 0.0f, 0.0f }, 1.0f };
};
// Bends the other arm at the elbow and turns it at the shoulder so that its wrist is on the grip
// (as far as the arm reaches), keeping the elbow on the side the pose had it. With `align` it is the
// hand's own socket (its palm) that is on the grip, turned as the grip is: the hand holds the grip
// the way it would hold an item placed there. The carrying arm and
// the item are untouched: the item follows the hand that carries it, the other hand follows the item.
void SolveGrip( const AnimSet& set, Models& models, const HandGrip& grip );
// The grip that says where the other hand is in this pose: solving it on a pose whose carrying arm
// has moved since puts the other hand back as the animation had the two (place and turn).
HandGrip AsAnimated( const AnimSet& set, const Models& models, bool leftCarries );

} // namespace cb::anim
