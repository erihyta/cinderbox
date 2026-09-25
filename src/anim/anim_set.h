#pragma once

// A skeleton and the six locomotion clips, either loaded from ozz files or generated in code.

#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/memory/unique_ptr.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cb::anim
{

enum Clip : int
{
	ClipIdle,
	ClipWalk,
	ClipRun,
	ClipJumpStart,
	ClipFall,
	ClipLand,
	ClipCount,
};

// Names used in anim.cfg and for the converted file names.
const char* ClipName( Clip clip );

// Reads a file of a character folder by its name relative to the folder ("anim.cfg", "run.ozz").
// Returns false when it does not exist. Lets the same loader read a folder on disk, a mounted Godot
// pack (res://) or a workshop item's zip on the server.
using FileReader = std::function<bool( const std::string& name, std::string& bytes )>;

// A FileReader over a folder on disk.
FileReader DiskReader( const std::string& dir );

class AnimSet
{
public:
	// Blocky placeholder: Mixamo joint names, procedural clips.
	static std::unique_ptr<AnimSet> CreateProcedural();

	// Loads `<dir>/anim.cfg` (see assets/anim/README.md). Returns null and sets `error` on failure.
	// Missing clips are allowed (that layer falls back to the rest pose) and reported in `warnings`.
	static std::unique_ptr<AnimSet> Load( const std::string& dir, std::string& error, std::string& warnings );

	// The same, reading through `read`; `label` names the source in the description and messages.
	static std::unique_ptr<AnimSet> Load( const FileReader& read, const std::string& label, std::string& error,
										  std::string& warnings );

	// Writes skeleton, clips and anim.cfg to `dir` (used to test the file pipeline).
	bool Save( const std::string& dir ) const;

	const ozz::animation::Skeleton& Skeleton() const
	{
		return *m_skeleton;
	}
	// The skeleton's rest in model space, with the set's scale applied. Retargeting needs it as
	// the baseline a pose is a deviation from.
	const ozz::vector<ozz::math::Float4x4>& RestModels() const
	{
		return m_restModels;
	}
	// Null if the clip is not available.
	const ozz::animation::Animation* Get( Clip clip ) const
	{
		return m_clips[clip].get();
	}
	float Duration( Clip clip ) const
	{
		return m_clips[clip] ? m_clips[clip]->duration() : 1.0f;
	}

	// Per joint, what turns its model-space frame into the frame items attach to (a pistol in the
	// RightHand): the placeholder rig's frames, whatever axes this skeleton's bones use. The
	// placeholder rig's arms hang down with identity joints, so a hand's -Y runs along the fingers;
	// on another rig the frame is that one, swung from the placeholder's bone direction onto this
	// rig's rest bone direction (a T-pose's hand gets it turned out to the side, palm down).
	// Rotation only; identity on the placeholder rig. Applied as model * AttachFrame(j).
	const ozz::math::Float4x4& AttachFrame( int joint ) const
	{
		return m_attachFrames[size_t( joint )];
	}

	// Uniform scale applied to model-space poses (e.g. 0.01 for centimetre rigs).
	float Scale() const
	{
		return m_scale;
	}
	// Keep the root joint from drifting horizontally (clips exported without "In Place").
	bool LockRootXZ() const
	{
		return m_lockRootXZ;
	}
	const std::string& Description() const
	{
		return m_description;
	}

	// What the pose turns toward where the player looks while it aims (AnimState::aiming): joints
	// with weights, applied in order, and the joint that ends up on the line of sight. anim.cfg:
	//   aim = RightUpperArm:1          (joints by humanoid-profile name, "name:weight" each)
	//   aim_tip = RightHand
	// Those are the defaults. Empty when the skeleton has none of them.
	const std::vector<std::pair<int, float>>& AimJoints() const
	{
		return m_aimJoints;
	}
	int AimTip() const
	{
		return m_aimTip;
	}
	const std::string& AimConfig() const
	{
		return m_aimConfig;
	}
	const std::string& AimTipName() const
	{
		return m_aimTipName;
	}
	// A stance clip by name ("pistol", "melee_walk"), null if the character has none. anim.cfg:
	//   stance.pistol = pistol.ozz
	const ozz::animation::Animation* StanceClip( const std::string& name ) const
	{
		auto it = m_stanceClips.find( name );
		return it != m_stanceClips.end() ? it->second.get() : nullptr;
	}
	const std::map<std::string, ozz::unique_ptr<ozz::animation::Animation>>& StanceClips() const
	{
		return m_stanceClips;
	}
	// A layer's mask ("Spine", "Spine:0.5 RightShoulder"), "" when the character defines none. anim.cfg:
	//   mask.upper = Spine
	// "upper" defaults to "Spine"; "full" (every bone) needs no mask.
	std::string Mask( const std::string& layer ) const
	{
		auto it = m_masks.find( layer );
		if ( it != m_masks.end() )
		{
			return it->second;
		}
		return layer == "upper" ? "Spine" : "";
	}
	const std::map<std::string, std::string>& Masks() const
	{
		return m_masks;
	}

	// The joints the legs turn about (the hips) and the upper body turns back about (the spine),
	// by humanoid-profile name; -1 when the skeleton lacks them (the legs then stay straight).
	int HipsJoint() const
	{
		return m_hipsJoint;
	}
	int SpineJoint() const
	{
		return m_spineJoint;
	}

	// Fills AttachFrame from the rest; the loaders call it.
	void ComputeAttachFrames();

	// Resolves the names against the skeleton; unknown joints go to `warnings`.
	void SetAim( const std::string& chain, const std::string& tip, std::string& warnings );

private:
	ozz::unique_ptr<ozz::animation::Skeleton> m_skeleton;
	ozz::vector<ozz::math::Float4x4> m_restModels;
	ozz::vector<ozz::math::Float4x4> m_attachFrames;
	std::array<ozz::unique_ptr<ozz::animation::Animation>, ClipCount> m_clips;
	float m_scale = 1.0f;
	bool m_lockRootXZ = true;
	std::string m_description;
	std::map<std::string, ozz::unique_ptr<ozz::animation::Animation>> m_stanceClips;
	std::map<std::string, std::string> m_masks;
	std::vector<std::pair<int, float>> m_aimJoints;
	int m_hipsJoint = -1;
	int m_spineJoint = -1;
	int m_aimTip = -1;
	std::string m_aimConfig = "RightUpperArm:1";
	std::string m_aimTipName = "RightHand";
};

} // namespace cb::anim
