#pragma once

// A character's skeleton, the clips its state machine plays and the machine itself (as text): baked
// from a Godot scene and loaded from ozz files, or the placeholder rig, generated in code.

#include "box3d/math_functions.h"
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

// Reads a file of a character folder by its name relative to the folder ("anim.cfg", "run.ozz").
// Returns false when it does not exist. Lets the same loader read a folder on disk, a mounted Godot
// pack (res://) or a workshop item's zip on the server.
using FileReader = std::function<bool( const std::string& name, std::string& bytes )>;

// A FileReader over a folder on disk.
FileReader DiskReader( const std::string& dir );

class AnimSet
{
public:
	// The placeholder rig: what a server without a character plays, with no assets. Humanoid-profile
	// joint names, six procedural clips (idle, walk, run, jump_start, fall, land) and a state
	// machine of one layer that plays them.
	static std::unique_ptr<AnimSet> CreateProcedural();

	// Loads `<dir>/anim.cfg` and what it names: the skeleton, the clips ("clip.<name> = file") and
	// graph.cfg, as the character bake writes them. Returns null and sets `error` on failure. A
	// clip that does not load is reported in `warnings` (states that play it rest).
	static std::unique_ptr<AnimSet> Load( const std::string& dir, std::string& error, std::string& warnings );

	// The same, reading through `read`; `label` names the source in the description and messages.
	static std::unique_ptr<AnimSet> Load( const FileReader& read, const std::string& label, std::string& error,
										  std::string& warnings );

	// Writes the skeleton, the clips, anim.cfg and graph.cfg to `dir` (used to test the file pipeline).
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
	// Turn the hips toward the direction of travel and the spine back (AnimState::legYaw), so a
	// forward walk goes sideways. Off for characters with their own directional clips (strafes):
	// anim.cfg "turn_legs = false".
	bool TurnLegs() const
	{
		return m_turnLegs;
	}
	// Turn the spine back by however much the clips turned the hips, so the chest faces where the
	// body faces while the legs run where they run (strafe clips turn the hips toward the travel),
	// and give the head back that turn where the base layer set it. anim.cfg "face_forward = true".
	bool FaceForward() const
	{
		return m_faceForward;
	}
	int NeckJoint() const
	{
		return m_neckJoint;
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
	// What bends with the camera's pitch while the character faces the camera (AnimState::look):
	// joints with the share of the pitch each one turns by, about the body's side-to-side axis.
	// The shares add up along the chain: with the default the chest has turned by 0.6 of the pitch
	// and the head by all of it, so the head looks exactly where the camera does. anim.cfg:
	//   look = Spine:0.2 Chest:0.2 UpperChest:0.2 Neck:0.2 Head:0.2
	// Joints the skeleton lacks are left out; "look =" (nothing) turns it off.
	const std::vector<std::pair<int, float>>& LookJoints() const
	{
		return m_lookJoints;
	}
	const std::string& LookConfig() const
	{
		return m_lookConfig;
	}
	// A clip the character's state machine plays, by its Godot animation's name ("Walk"); null if the
	// character has none. anim.cfg:
	//   clip.Walk = clip_Walk.ozz
	const ozz::animation::Animation* NamedClip( const std::string& name ) const
	{
		auto it = m_namedClips.find( name );
		return it != m_namedClips.end() ? it->second.get() : nullptr;
	}
	// The character's state machine (graph.cfg, see sim/anim_graph.h) as text. The server puts it in
	// the schema; the simulation runs it and the pose follows it. Empty: the character rests.
	const std::string& GraphText() const
	{
		return m_graphText;
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

	// An arm's joints, shoulder to wrist (upper arm, lower arm, hand), by humanoid-profile name; -1
	// where the skeleton lacks one.
	struct Arm
	{
		int upper = -1;
		int lower = -1;
		int hand = -1;
	};
	const Arm& ArmJoints( bool left ) const
	{
		return m_arms[left ? 1 : 0];
	}

	// Where an item sits in a hand: the socket's frame in the hand joint's own frame (position in
	// metres of model space, and a rotation). From the character's CbSocket named RightHand / LeftHand
	// (anim.cfg "socket.RightHand = x y z qx qy qz qw", baked from the scene), or the built-in one:
	// AttachFrame, turned so the item points along the fingers, 6 cm into the palm.
	struct HandSocket
	{
		b3Vec3 position = { 0.0f, 0.0f, 0.0f };
		b3Quat rotation = { { 0.0f, 0.0f, 0.0f }, 1.0f };
	};
	const HandSocket& HandSocketOf( bool left ) const
	{
		return m_handSockets[left ? 1 : 0];
	}
	void SetHandSocket( bool left, const HandSocket& socket )
	{
		m_handSockets[left ? 1 : 0] = socket;
		m_handSocketSet[left ? 1 : 0] = true;
	}

	// Fills AttachFrame from the rest; the loaders call it.
	void ComputeAttachFrames();

	// Resolves the names against the skeleton; unknown joints go to `warnings`.
	void SetAim( const std::string& chain, const std::string& tip, std::string& warnings );
	void SetLook( const std::string& chain, std::string& warnings );

private:
	ozz::unique_ptr<ozz::animation::Skeleton> m_skeleton;
	ozz::vector<ozz::math::Float4x4> m_restModels;
	ozz::vector<ozz::math::Float4x4> m_attachFrames;
	float m_scale = 1.0f;
	bool m_lockRootXZ = true;
	bool m_turnLegs = true;
	bool m_faceForward = false;
	int m_neckJoint = -1;
	std::string m_description;
	std::map<std::string, ozz::unique_ptr<ozz::animation::Animation>> m_namedClips;
	std::string m_graphText;
	std::vector<std::pair<int, float>> m_aimJoints;
	int m_hipsJoint = -1;
	Arm m_arms[2]; // right, left
	HandSocket m_handSockets[2];
	bool m_handSocketSet[2] = { false, false }; // the character said where (else: the built-in frame)
	void DefaultHandSockets();
	int m_spineJoint = -1;
	int m_aimTip = -1;
	std::string m_aimConfig = "RightUpperArm:1";
	std::string m_aimTipName = "RightHand";
	std::vector<std::pair<int, float>> m_lookJoints;
	std::string m_lookConfig = "Spine:0.2 Chest:0.2 UpperChest:0.2 Neck:0.2 Head:0.2";
};

} // namespace cb::anim
