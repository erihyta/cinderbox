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

// Rotates `joint` and everything below it about the joint's position.
void RotateSubtree( const AnimSet& set, Models& models, int joint, b3Quat turn );

// Turns each joint of `chain` in order, and everything below it about it, so the line from it to
// `tip` points along `direction` (model space, normalized) by its weight: 0 leaves the pose alone,
// 1 aims fully. A chain like { UpperChest 0.3, RightUpperArm 1 } leans the chest a little and
// then points the arm exactly.
void AimChain( const AnimSet& set, Models& models, const std::vector<std::pair<int, float>>& chain, int tip, b3Vec3 direction );

} // namespace cb::anim
