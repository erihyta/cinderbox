#pragma once

// Authoring a character in the editor, and baking it for the game.
//
// A character scene (characters/<name>/character.tscn in a character item's project) has a
// CbCharacter at its root, the imported model under it (Skeleton3D and AnimationPlayer, bones named
// after Godot's SkeletonProfileHumanoid), a CinderboxSkeleton driving that Skeleton3D, and CbHitbox
// shapes under BoneAttachment3D nodes.
//
// The inspector's "Bake" writes, next to the scene, what the game reads at runtime:
//   skeleton.ozz and one .ozz per clip (idle, walk, run, jump_start, fall, land), anim.cfg,
//   hitboxes.cfg.
// Exporting the item ships them in its pack. Nothing is imported or converted when a player joins:
// the client and the server read these baked files.

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace cb::gd
{

// A hit zone on a bone: put it under a BoneAttachment3D. Sphere, capsule and box shapes.
class CbHitbox : public godot::CollisionShape3D
{
	GDCLASS( CbHitbox, godot::CollisionShape3D )

public:
	void set_zone( const godot::String& v )
	{
		m_zone = v;
	}
	godot::String get_zone() const
	{
		return m_zone;
	}

	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	godot::String m_zone = "torso";
};

class CbCharacter : public godot::Node3D
{
	GDCLASS( CbCharacter, godot::Node3D )

public:
	// Writes the baked files into `folder` (res://characters/<name>/ when empty). Returns
	// { ok, error, folder, joints, clips, hitboxes, warnings }.
	godot::Dictionary bake_to( const godot::String& folder );
	// The inspector button: bake_to() the default folder and report.
	void bake();
	godot::Callable get_bake_button();

	void set_character_name( const godot::String& v )
	{
		m_name = v;
	}
	godot::String get_character_name() const
	{
		return m_name;
	}
	void set_skeleton_path( const godot::NodePath& v )
	{
		m_skeleton = v;
	}
	godot::NodePath get_skeleton_path() const
	{
		return m_skeleton;
	}
	void set_animation_player_path( const godot::NodePath& v )
	{
		m_player = v;
	}
	godot::NodePath get_animation_player_path() const
	{
		return m_player;
	}
	void set_clip_idle( const godot::String& v )
	{
		m_clips[0] = v;
	}
	godot::String get_clip_idle() const
	{
		return m_clips[0];
	}
	void set_clip_walk( const godot::String& v )
	{
		m_clips[1] = v;
	}
	godot::String get_clip_walk() const
	{
		return m_clips[1];
	}
	void set_clip_run( const godot::String& v )
	{
		m_clips[2] = v;
	}
	godot::String get_clip_run() const
	{
		return m_clips[2];
	}
	void set_clip_jump_start( const godot::String& v )
	{
		m_clips[3] = v;
	}
	godot::String get_clip_jump_start() const
	{
		return m_clips[3];
	}
	void set_clip_fall( const godot::String& v )
	{
		m_clips[4] = v;
	}
	godot::String get_clip_fall() const
	{
		return m_clips[4];
	}
	void set_clip_land( const godot::String& v )
	{
		m_clips[5] = v;
	}
	godot::String get_clip_land() const
	{
		return m_clips[5];
	}
	void set_sample_rate( double v )
	{
		m_sampleRate = v;
	}
	double get_sample_rate() const
	{
		return m_sampleRate;
	}
	void set_stance_clips( const godot::Dictionary& v )
	{
		m_stanceClips = v;
	}
	godot::Dictionary get_stance_clips() const
	{
		return m_stanceClips;
	}
	void set_masks( const godot::Dictionary& v )
	{
		m_masks = v;
	}
	godot::Dictionary get_masks() const
	{
		return m_masks;
	}
	void set_aim_chain( const godot::String& v )
	{
		m_aimChain = v;
	}
	godot::String get_aim_chain() const
	{
		return m_aimChain;
	}
	void set_aim_tip( const godot::String& v )
	{
		m_aimTip = v;
	}
	godot::String get_aim_tip() const
	{
		return m_aimTip;
	}
	void set_lock_root_xz( bool v )
	{
		m_lockRootXZ = v;
	}
	bool get_lock_root_xz() const
	{
		return m_lockRootXZ;
	}

	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	godot::String m_name;
	godot::NodePath m_skeleton;
	godot::NodePath m_player;
	godot::String m_clips[6] = { "idle", "walk", "run", "jump_start", "fall", "land" };
	double m_sampleRate = 30.0;
	bool m_lockRootXZ = true;
	// What the pose turns toward where the player looks while a mod has it aim: bones (profile
	// names) with weights, in order, and the bone that ends up on the line of sight.
	// Stance clips the mods' stances use: "pistol" (one loop) or "melee_walk" (one of the six) ->
	// the name of an animation of the AnimationPlayer.
	godot::Dictionary m_stanceClips;
	// Layer masks: "upper" -> "Spine" (the default), "arms" -> "LeftShoulder RightShoulder", ...
	godot::Dictionary m_masks;
	godot::String m_aimChain = "RightUpperArm:1";
	godot::String m_aimTip = "RightHand";
};

} // namespace cb::gd
