#pragma once

// Shows an ozz pose inside a player prefab.
// - By default draws one box per bone (the blocky placeholder look), via a MultiMesh.
// - If `skeleton_path` points at a Skeleton3D, its bones are driven by name instead, so a
//   modded skinned character follows the same animation. Set `draw_bone_boxes` off then.
// The node's origin is the character's feet, facing +Z (the simulation's convention).

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>

#include <string>
#include <vector>

namespace cb::anim
{
class PoseEvaluator;
}

namespace cb::gd
{

class CinderboxSkeleton : public godot::Node3D
{
	GDCLASS( CinderboxSkeleton, godot::Node3D )

public:
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

	// Called by CinderboxClient every frame.
	void ApplyPose( const anim::PoseEvaluator& pose );

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

	// ozz joint index -> Skeleton3D bone index (-1: not present), rebuilt when the skeleton changes.
	std::vector<int> m_boneMap;
	uint64_t m_mappedSkeleton = 0;
};

} // namespace cb::gd
