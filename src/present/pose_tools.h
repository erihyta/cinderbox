#pragma once

// Pose operations presentation needs beyond sampling clips: a skeleton pose from a ragdoll's body
// parts, a blend between two poses, and turning an arm to aim. All work on model-space matrices in
// the layout PoseEvaluator produces (feet at the origin, facing +Z, the set's scale applied), so
// renderers draw the result exactly like an evaluated pose.

#include "anim_set.h"
#include "components.h"

#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"

#include <vector>

namespace cb::present
{

using Models = ozz::vector<ozz::math::Float4x4>;

// How a skeleton hangs off the ragdoll's parts. Built once per AnimSet: each joint follows the
// part of its nearest ancestor that one of the parts drives (sim/ragdoll.h names them), keeping
// its own rest offset from that part's joint, so a skeleton with extra joints or other proportions
// still comes out whole.
struct RagdollRig
{
	std::vector<int> partOf;	  // per joint: the part it follows, -1 to stay at rest
	std::vector<int> anchorJoint; // per part: the joint it drives, -1 if the skeleton lacks it
	std::vector<b3Quat> restRotation;
	std::vector<b3Vec3> restPosition;
	float scale = 1.0f;
};

RagdollRig BuildRagdollRig( const anim::AnimSet& set );

// The frame a ragdoll's pose is expressed in: facing its rest yaw, placed where its feet would be
// if the pelvis still stood at rest. It follows the pelvis, so the pose stays near the origin.
Transform RagdollFrame( const Transform& pelvis, float yaw );

// Model-space pose of the skeleton for the given part transforms (world space), in `frame`.
void RagdollModels( const RagdollRig& rig, const Transform* parts, const Transform& frame, Models& out );

// out = a blended toward b by t in [0, 1] (translation lerp, rotation nlerp, per joint).
void BlendModels( const Models& a, const Models& b, float t, Models& out );

// Rotates `joint` and everything below it about the joint, so the line from it to `tip` points
// along `direction` (model space, normalized). `weight` 0 leaves the pose alone, 1 aims fully.
void AimChain( const anim::AnimSet& set, Models& models, int joint, int tip, b3Vec3 direction, float weight );

// Index of a joint by its humanoid-profile name (Mixamo names match too), -1 if absent.
int FindJoint( const anim::AnimSet& set, const char* profileName );

} // namespace cb::present
