#pragma once

// Shows an ozz pose inside a player prefab.
// - By default draws one box per bone (the blocky placeholder look), via a MultiMesh.
// - If `skeleton_path` points at a Skeleton3D, its bones are driven by name instead, so a
//   modded skinned character follows the same animation. Set `draw_bone_boxes` off then.
// The node's origin is the character's feet, facing +Z (the simulation's convention).
//
// The ozz pose is the only thing that places a player's body bones: it is what the server's hit
// tests pose too. A driven Skeleton3D gets a CbPoseModifier as its first modifier, which applies
// the pose again after any AnimationPlayer or AnimationTree has run, so Godot animation on a player
// can only add what the pose leaves alone (faces, fingers, props, materials) and modifiers placed
// after it (look-at, springs) run on top of the pose.

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/skeleton_modifier3d.hpp>

#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"

#include <memory>
#include <string>
#include <vector>

namespace cb::anim
{
class AnimSet;
class PoseEvaluator;
}

namespace cb::gd
{

class CinderboxSkeleton;

// Created by CinderboxSkeleton under the skeleton it drives; never saved with a scene.
class CbPoseModifier : public godot::SkeletonModifier3D
{
	GDCLASS( CbPoseModifier, godot::SkeletonModifier3D )

public:
	void set_driver( CinderboxSkeleton* driver );
	void _process_modification() override;

protected:
	static void _bind_methods()
	{
	}

private:
	uint64_t m_driver = 0;
};

class CinderboxSkeleton : public godot::Node3D
{
	GDCLASS( CinderboxSkeleton, godot::Node3D )

public:
	// Called by the CbPoseModifier: apply the last pose to `target` again.
	void ReapplyPose( godot::Skeleton3D* target );

	void _ready() override;

	void set_draw_bone_boxes( bool value );
	bool get_draw_bone_boxes() const
	{
		return m_drawBoneBoxes;
	}
	void set_skeleton_path( const godot::NodePath& path );
	godot::NodePath get_skeleton_path() const
	{
		return m_skeletonPath;
	}
	void set_body_color( const godot::Color& color );
	godot::Color get_body_color() const
	{
		return m_bodyColor;
	}
	void set_use_slot_color( bool value )
	{
		m_useSlotColor = value;
	}
	bool get_use_slot_color() const
	{
		return m_useSlotColor;
	}

	void set_retarget( bool value );
	bool get_retarget() const
	{
		return m_retarget;
	}

	// Called by CinderboxClient every frame, with an evaluated pose or one built from it (aimed, or
	// a ragdoll's). Model space: feet at the origin, facing +Z.
	void ApplyPose( const anim::PoseEvaluator& pose );
	void ApplyPose( const anim::AnimSet& set, const ozz::vector<ozz::math::Float4x4>& models );

	// Where a joint of the last applied pose is, relative to this node. False when the rig has no
	// joint by that humanoid-profile name.
	bool JointTransform( const godot::String& profileName, godot::Transform3D& out ) const;
	// The same in world space, for scripts: where a hand or the head is right now.
	godot::Transform3D get_joint_global_transform( const godot::String& profile_name ) const;

	// Poses the rig from a script with the placeholder clips, for previewing a character or
	// checking a retarget without a server. mode: 0 locomotion, 1 jump, 2 fall, 3 land.
	void apply_anim_state( int mode, float mode_time, float locomotion_phase, float ground_speed );

protected:
	static void _bind_methods();

private:
	void EnsureMultiMesh( int instances );

	bool m_drawBoneBoxes = true;
	bool m_useSlotColor = true;
	godot::NodePath m_skeletonPath;
	godot::Color m_bodyColor = godot::Color( 0.9f, 0.35f, 0.3f );

	godot::MultiMeshInstance3D* m_boxes = nullptr;
	godot::Ref<godot::MultiMesh> m_multimesh;

	// Drive the target by rotation relative to its own rest (retargeting), instead of forcing
	// each bone to the rig's exact position. Off only makes sense for a rig built like ours.
	bool m_retarget = true;

	// ozz joint index -> Skeleton3D bone index (-1: not present), rebuilt when the skeleton changes.
	std::vector<int> m_boneMap;
	uint64_t m_mappedSkeleton = 0;
	// The target's bones, parents first, and scratch space for exact driving.
	std::vector<int> m_boneOrder;
	std::vector<godot::Transform3D> m_globals;
	std::vector<uint8_t> m_known;

	// Per mapped joint: the constant that carries our rig's pose onto the target's rest, so the
	// target keeps its own proportions and its own rest posture. See Bind().
	std::vector<godot::Quaternion> m_restBridge;
	// Target bone index of the hips, and how much taller or shorter the target is than our rig.
	int m_targetHips = -1;
	float m_hipScale = 1.0f;
	godot::Vector3 m_sourceHipsRest;
	godot::Vector3 m_targetHipsRest;

	std::shared_ptr<const anim::AnimSet> m_previewSet;
	std::unique_ptr<anim::PoseEvaluator> m_previewPose;

	void Bind( godot::Skeleton3D* target, const anim::AnimSet& set );
	void EnsureModifier( godot::Skeleton3D* target );
	void DriveSkeleton( godot::Skeleton3D* target, const ozz::vector<ozz::math::Float4x4>& models );

	// The last applied pose, for joint lookups.
	const anim::AnimSet* m_lastSet = nullptr;
	ozz::vector<ozz::math::Float4x4> m_lastModels;
};

} // namespace cb::gd
