#pragma once

// A character as the game draws it (M260): one way for every body, the viewer's own too.
//
// CharacterLook is what the game takes from a character's scene, once per character: the meshes
// (skinned to its skeleton, or riding on a bone), their materials, layers and shadows, the
// skeleton's bones and how the ozz pose drives them (exactly, or retargeted onto the bones' own
// rests), and the sockets. The placeholder rig (no scene skeleton) is its boxes, one skinned mesh.
// What it does not take is not drawn in the game: scripts, cameras, other nodes, Godot animation
// on the skeleton, modifiers (the editor's scene and preview stay as they are). NotDrawn() names
// what was left out; the bake says so.
//
// DrawnBody draws one body with it through the RenderingServer: a skeleton and an instance per
// mesh, no node. Its bones come from the ozz pose when the body is posed (M259: far off and out
// of view, now and then); its place, every frame. The viewer's own body in first person casts its
// shadow and is not drawn; the viewer sees copies: the legs (below the waist) and the arms (on the
// view model's layer), as CinderboxSkeleton did with copies of its Skeleton3D.

#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <memory>
#include <string>
#include <vector>

namespace cb::anim
{
class AnimSet;
}

namespace cb::gd
{

// One box per bone of the placeholder rig, the way it is drawn: by the bone's name (Mixamo or
// profile), or by its length.
struct BoneStyle
{
	float width;
	float depth;
	bool body;
};
BoneStyle BoneStyleFor( const char* jointName, float length );
// A unit box stretched from `a` to `b`, its x along `refX`.
godot::Transform3D BoneBoxTransform( const godot::Vector3& a, const godot::Vector3& b, const godot::Vector3& refX, float width, float depth );

class CharacterLook
{
public:
	struct Bone
	{
		godot::String name;
		int parent = -1;
		godot::Transform3D rest;   // local
		godot::Transform3D pose;   // local, as the scene has it (what a bone the pose does not drive keeps)
		godot::Transform3D global; // the global rest
	};
	struct Mesh
	{
		godot::Ref<godot::Mesh> mesh;
		// Skinned: each bind's bone and bind pose. Rigid: on `bone` (-1: the skeleton's frame).
		bool skinned = false;
		std::vector<int> binds;
		std::vector<godot::Transform3D> bindPoses;
		int bone = -1;
		godot::Transform3D local; // skinned: in the skeleton's frame; rigid: in its bone's
		std::vector<godot::Ref<godot::Material>> surfaces; // each surface's override (null: the mesh's own)
		godot::Ref<godot::Material> materialOverride;
		godot::Ref<godot::Material> overlay;
		uint32_t layers = 1;
		int castShadows = 1; // RenderingServer::ShadowCastingSetting
		float lodBias = 1.0f;
		bool tinted = false; // the placeholder's boxes: drawn in the body's colour
	};
	struct Socket
	{
		godot::String name;
		godot::String bone; // a joint of the pose, by its profile name
		godot::Transform3D local;
		bool itemFrame = false; // `local` is in AnimSet::AttachFrame's frame (the hands made for a rig without)
	};

	// From an instance of a character's scene (or a prefab with a CinderboxSkeleton), for `set`'s
	// pose. Null when it has nothing to draw with.
	static std::shared_ptr<CharacterLook> FromScene( godot::Node* root, const anim::AnimSet& set );

	const std::vector<Bone>& Bones() const
	{
		return m_bones;
	}
	const std::vector<Mesh>& Meshes() const
	{
		return m_meshes;
	}
	const std::vector<Socket>& Sockets() const
	{
		return m_sockets;
	}
	// The skeleton's frame, and the pose's (CinderboxSkeleton's: ozz models are in it), in the root's.
	const godot::Transform3D& SkeletonInRoot() const
	{
		return m_skeletonInRoot;
	}
	const godot::Transform3D& PoseInRoot() const
	{
		return m_poseInRoot;
	}
	bool UsesSlotColor() const
	{
		return m_useSlotColor;
	}
	const godot::Color& BodyColor() const
	{
		return m_bodyColor;
	}
	// The bone a profile name ("Hips") is driven onto, or -1.
	int BoneOf( const char* profileName ) const;
	// What of the scene is not drawn in the game (for the bake to say).
	const godot::PackedStringArray& NotDrawn() const
	{
		return m_notDrawn;
	}

	// The bones' globals (the skeleton's frame) for a pose: driven bones where the pose has them,
	// the rest after their parents. `locals` gets each bone's local pose.
	void Drive( const ozz::vector<ozz::math::Float4x4>& models, std::vector<godot::Transform3D>& locals,
				std::vector<godot::Transform3D>& globals ) const;
	// The globals again from `locals`, parents first.
	void Globals( const std::vector<godot::Transform3D>& locals, std::vector<godot::Transform3D>& globals ) const;

private:
	void Bind( const anim::AnimSet& set );
	void AddBoxes( const anim::AnimSet& set );

	std::vector<Bone> m_bones;
	std::vector<int> m_order; // parents first
	std::vector<int> m_jointToBone;
	std::vector<std::string> m_jointNames; // the pose's joints
	mutable std::vector<int> m_driverScratch;
	mutable std::vector<godot::Quaternion> m_rotationScratch;
	bool m_retarget = true;
	std::vector<godot::Quaternion> m_restBridge; // by joint
	int m_hips = -1;
	float m_hipScale = 1.0f;
	godot::Vector3 m_sourceHipsRest;
	godot::Vector3 m_targetHipsRest;
	std::vector<Mesh> m_meshes;
	std::vector<Socket> m_sockets;
	godot::Transform3D m_skeletonInRoot;
	godot::Transform3D m_poseInRoot;
	bool m_useSlotColor = false;
	godot::Color m_bodyColor = godot::Color( 0.9f, 0.35f, 0.3f );
	godot::PackedStringArray m_notDrawn;
};

class DrawnBody
{
public:
	DrawnBody( std::shared_ptr<const CharacterLook> look, const godot::RID& scenario );
	~DrawnBody();
	DrawnBody( const DrawnBody& ) = delete;
	DrawnBody& operator=( const DrawnBody& ) = delete;

	const CharacterLook& Look() const
	{
		return *m_look;
	}
	// Where the body is (its feet, facing +Z, its scale), every frame.
	void Place( const godot::Transform3D& world );
	const godot::Transform3D& Global() const
	{
		return m_world;
	}
	// The pose's frame in the world: where JointTransform's joints are from.
	godot::Transform3D PoseGlobal() const
	{
		return m_world * m_look->PoseInRoot();
	}
	void SetVisible( bool visible );
	// Its shadow (none far off, M260); the viewer's own in first person casts one still.
	void SetShadows( bool on );
	void SetBodyColor( const godot::Color& color );

	// A pose (model space: the pose's frame, the feet at the origin, facing +Z).
	void ApplyPose( const anim::AnimSet& set, const ozz::vector<ozz::math::Float4x4>& models );
	// Where a joint of the last pose is, in the body's frame (forItems: in AnimSet::AttachFrame's
	// frame; view: of the view's pose, with its hands moved). False: no such joint.
	bool JointTransform( const godot::String& profileName, godot::Transform3D& out, bool forItems = false, bool view = false ) const;

	// The viewer's own body, looked out of (see the header, and CinderboxSkeleton's).
	void SetFirstPerson( bool on );
	bool FirstPerson() const
	{
		return m_firstPerson;
	}
	void SetViewPose( const ozz::vector<ozz::math::Float4x4>* models );
	void SetViewMove( const godot::Transform3D& move )
	{
		m_viewMove = move;
	}
	void SetViewHands( bool left, bool right )
	{
		m_viewHandShown[0] = left;
		m_viewHandShown[1] = right;
	}
	void SetViewArms( bool value )
	{
		m_viewArms = value;
	}
	bool ViewArms() const
	{
		return m_viewArms;
	}
	void PlaceViewRig( const godot::Transform3D& stitch );
	void PlaceViewLegs( const godot::Vector3& shift );
	void PlaceViewArms( const godot::Transform3D& camera, const godot::Transform3D& left, const godot::Transform3D& right, bool holdsLeft,
						bool holdsRight );
	godot::Transform3D ViewHand( int side ) const
	{
		return m_viewHands[side != 0 ? 1 : 0];
	}
	static constexpr uint32_t kViewLayer = 1u << 19; // Godot's layer 20 (CinderboxSkeleton::kViewLayer)

private:
	// One drawing of the look: the body, or a copy of it for the view.
	struct Copy
	{
		std::vector<godot::RID> skeletons; // by mesh (an empty RID for a rigid one)
		std::vector<godot::RID> instances; // by mesh
		std::vector<godot::Transform3D> locals, globals;
		godot::Transform3D frame; // the skeleton's frame in the world
		bool made = false;
		bool shown = false;
	};
	void Make( Copy& copy, uint32_t layers, bool castShadows );
	void Free( Copy& copy );
	void Show( Copy& copy, bool shown );
	void PushBones( Copy& copy );
	void PushPlaces( Copy& copy );
	void SetGlobal( Copy& copy, int bone, const godot::Transform3D& global );
	void UpdateViewLegs();
	void UpdateViewArms();
	void KeepArms( Copy& rig, bool left, bool right );
	void BendArm( Copy& rig, int upper, int lower, int end, const godot::Transform3D& hand, const godot::Vector3& pole );

	std::shared_ptr<const CharacterLook> m_look;
	godot::RID m_scenario;
	godot::Transform3D m_world;
	bool m_placed = false;
	bool m_visible = true;
	bool m_shadows = true;
	godot::Ref<godot::StandardMaterial3D> m_tintMaterial; // the placeholder's boxes, in the body's colour
	Copy m_body;
	Copy m_legs;
	Copy m_arms;

	const anim::AnimSet* m_set = nullptr;
	ozz::vector<ozz::math::Float4x4> m_models;
	bool m_firstPerson = false;
	bool m_viewArms = false;
	bool m_viewHandShown[2] = { true, true };
	godot::Transform3D m_viewMove;
	godot::Vector3 m_viewShort[2];
	godot::Transform3D m_viewHands[2];
	ozz::vector<ozz::math::Float4x4> m_viewModels;
	godot::Vector3 m_legsShift;
};

} // namespace cb::gd
