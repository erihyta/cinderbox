#pragma once

// What makes a scene an item, and what a link looks like.
//
//   CbItem      the root of an item's own scene: its kind, its name, its weight, its first-person
//               view, where the hands hold it, and what else the server should know. Its body is
//               the CollisionShape3D under it, its grips are two markers it names, its look is
//               everything else in the scene. Baked to items/<kind>.cfg, which the server reads and the game finds the
//               scene by.
//   CbLinkLook  what the line of a motion's probe is drawn as

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/marker3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include "cinderbox_body.h"

#include <memory>
#include <string>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace cb
{
struct AnimGraph;
namespace anim
{
class AnimSet;
class PoseEvaluator;
} // namespace anim
} // namespace cb

namespace cb::gd
{

class CinderboxSkeleton;

class CbItem : public CbBody
{
	GDCLASS( CbItem, CbBody )

public:
	// (An item weighs what it says: a kilogram unless it says otherwise.)
	CbItem()
	{
		m_mass = 1.0;
	}
	void set_kind( const godot::String& v )
	{
		m_kind = v;
		update_configuration_warnings();
	}
	godot::String get_kind() const
	{
		return m_kind;
	}
	void set_display_name( const godot::String& v )
	{
		m_displayName = v;
	}
	godot::String get_display_name() const
	{
		return m_displayName;
	}
	// The animation pack the item brings: while one is held, the pack's layers play instead of the
	// holder's own (a CbAnimPack scene of this project, by its name: "gun.pistol_hold"). None: the
	// character's own animations. And the socket it hangs in while it is put away ("Hip", "Back");
	// none: out of sight. Both are the server's to read, from the baked file: a mod that takes its
	// items from its look (Declarations::ItemsOfLook) needs no code for either.
	void set_animation_pack( const godot::String& v )
	{
		m_animationPack = v.strip_edges();
	}
	godot::String get_animation_pack() const
	{
		return m_animationPack;
	}
	// Its own fields: each copy of the item has them, starting with these values, and keeps its own
	// ({"gun.rounds": 30}). The value's type is the field's: a whole number an Int, a decimal a
	// Float, true / false a Bool. Server mods read and write them; looks read the one in a player's
	// hand as "hand.<field>".
	void set_fields( const godot::Dictionary& v )
	{
		m_fields = v;
	}
	godot::Dictionary get_fields() const
	{
		return m_fields;
	}
	// When used: what must hold for a use to do anything ("dash.charges > 0"), what a use changes
	// ("dash.charges -= 1"), the seconds between two, and the event a use says. For the helper nodes
	// under the item (its probe, effects, moves): what using it does.
	void set_use_conditions( const godot::PackedStringArray& v )
	{
		m_useConditions = v;
	}
	godot::PackedStringArray get_use_conditions() const
	{
		return m_useConditions;
	}
	void set_use_changes( const godot::PackedStringArray& v )
	{
		m_useChanges = v;
	}
	godot::PackedStringArray get_use_changes() const
	{
		return m_useChanges;
	}
	void set_use_cooldown( double v )
	{
		m_useCooldown = v;
	}
	double get_use_cooldown() const
	{
		return m_useCooldown;
	}
	void set_use_emits( const godot::String& v )
	{
		m_useEmits = v.strip_edges();
	}
	godot::String get_use_emits() const
	{
		return m_useEmits;
	}
	// Its own key ("H"): uses the first one carried, wherever it is (the action "<kind>.use").
	// Empty: it is used from the hand only.
	void set_use_key( const godot::String& v )
	{
		m_useKey = v.strip_edges();
	}
	godot::String get_use_key() const
	{
		return m_useKey;
	}
	void set_holster( const godot::String& v )
	{
		m_holster = v.strip_edges();
	}
	godot::String get_holster() const
	{
		return m_holster;
	}
	// First person: how far the arms holding this are moved in the viewer's own view, in metres to
	// the right, up and ahead of where the body's pose has them. Only the viewer's own picture
	// changes: everyone else, the shadow and the hit tests keep the pose.
	void set_view_offset( const godot::Vector3& v )
	{
		m_viewOffset = v;
	}
	godot::Vector3 get_view_offset() const
	{
		return m_viewOffset;
	}
	// First person, the other way: a node in the scene that stands where the viewer's eye is while
	// this is held (any Node3D; a CbViewCamera shows the view in the editor: its Preview is the
	// picture). With one, the viewer's own arms are not drawn and the item floats in the view, held
	// by the camera: where the body's pose has the hand no longer matters. Only the viewer's own
	// picture changes.
	void set_view_camera( const godot::NodePath& v )
	{
		m_viewCamera = v;
		update_configuration_warnings();
	}
	godot::NodePath get_view_camera() const
	{
		return m_viewCamera;
	}
	// First person: how what is held is turned in the viewer's own view, about the carrying hand, in
	// degrees: x tips it up, y turns it to the left, z rolls it (the camera's axes). The hands turn
	// with it and the arms reach.
	void set_view_rotation( const godot::Vector3& v )
	{
		m_viewRotation = v;
	}
	godot::Vector3 get_view_rotation() const
	{
		return m_viewRotation;
	}
	// First person: the field of view the item and the arms are drawn with (they are drawn apart
	// from the world, over it). 0: the game's own (or the view camera's, if there is one).
	void set_view_fov( double v )
	{
		m_viewFov = v;
	}
	double get_view_fov() const
	{
		return m_viewFov;
	}
	// First person: whether each arm is drawn while this is held.
	void set_view_left_hand( bool v )
	{
		m_viewLeftHand = v;
	}
	bool get_view_left_hand() const
	{
		return m_viewLeftHand;
	}
	void set_view_right_hand( bool v )
	{
		m_viewRightHand = v;
	}
	bool get_view_right_hand() const
	{
		return m_viewRightHand;
	}
	// Named numbers about the item ("pickup.hold_seconds": 0.5), baked with it. They replace what
	// the item's mod declared in code for the same names.
	void set_properties( const godot::Dictionary& v )
	{
		m_properties = v;
	}
	godot::Dictionary get_properties() const
	{
		return m_properties;
	}

	// In a player's slots (sim/types.h). Any copy is used from the hand (its slot selected, then the
	// use button), or by its own key (use_key) wherever it is. How long a use lasts:
	//   USE_TOOL    while the button is down. A tool, a weapon.
	//   USE_PRESS   once a press. A consumable, an ability (a grenade, a dash, a hook's throw).
	//   USE_HOLD    a press, and in use for as long as it is held (a jetpack).
	// And the slot it would go to when a player comes to carry one (1 is the first), if that one is
	// free; 0, or taken: the first free one.
	enum Use
	{
		USE_TOOL = 0,
		USE_PRESS = 1,
		USE_HOLD = 2,
	};
	void set_use( int v )
	{
		m_use = v;
	}
	int get_use() const
	{
		return m_use;
	}
	void set_slot( int v )
	{
		m_slot = v;
	}
	int get_slot() const
	{
		return m_slot;
	}
	// Which hand it is in:
	//   HAND_SELECTED  the slots' hand (the right), while its slot is the selected one.
	//   HAND_LEFT      the left hand, for as long as it is carried: a torch beside a gun.
	//                  Put away while what is selected has its other hand at a marker (a rifle's
	//                  barrel), or while another such item of a lower slot is there. What has its
	//                  other hand as the animation has it (a pistol) is held with one hand then.
	enum Hand
	{
		HAND_SELECTED = 0,
		HAND_LEFT = 1,
	};
	// Thrown fast and small (a dart, a rocket): moved with continuous collision against what moves
	// too, so it does not pass a thin crate or door between two steps. The level's walls stop
	// anything; this costs a little more a step.
	// How many of it one slot holds: a stack (1: one). One given to a player who has a stack of it
	// with room goes onto it; a throw of the one carried takes one off.
	// What it looks like in an inventory's slot (a CbItemIcon shows it). None: a picture of this
	// scene, taken in the game.
	void set_icon( const godot::Ref<godot::Texture2D>& v )
	{
		m_icon = v;
	}
	godot::Ref<godot::Texture2D> get_icon() const
	{
		return m_icon;
	}
	void set_stack( int v )
	{
		m_stack = v;
	}
	int get_stack() const
	{
		return m_stack;
	}
	// Its slot's key uses it where it is, and does not select it (for one used once a press or while
	// held): a skill on a Roblox-style hotbar.
	void set_slot_key_uses( bool v )
	{
		m_slotKeyUses = v;
	}
	bool get_slot_key_uses() const
	{
		return m_slotKeyUses;
	}
	void set_bullet( bool v )
	{
		m_bullet = v;
	}
	bool get_bullet() const
	{
		return m_bullet;
	}
	void set_hand( int v )
	{
		m_hand = v;
	}
	int get_hand() const
	{
		return m_hand;
	}

	// Where the hands hold it: two markers in the scene (any Node3D; a Marker3D shows as a cross).
	//
	//   The carrying hand   the item is carried here: this point is in the hand's socket (whichever
	//                       hand the item is in), the marker's -Z pointing along the fingers and +Y
	//                       up. Without one, the item is carried at its scene's origin.
	//   The other hand      what the character's other hand does while it is empty (OtherHand).
	//
	// Both are baked with the item: the body and the other hand's grip are written in the
	// carrying hand's frame, so the server poses the same arms for its hit tests and every player
	// sees them. The viewer draws the scene moved so that the carrying grip is in the socket.
	enum OtherHand
	{
		OTHER_FREE = 0, // the animation's: nothing keeps it on the item
		OTHER_AT_MARKER = 1, // the arm is bent so that the hand is at the marker
		OTHER_AT_MARKER_TURNED = 2, // and turned as the marker is
		// It stays where the item's animations have it relative to the carrying hand (place and
		// turn); no marker. For animations made with both hands on the item: they are kept as they
		// are, and kept together when the carrying arm is aimed.
		OTHER_AS_ANIMATED = 3,
	};
	void set_carry_grip( const godot::NodePath& v )
	{
		m_carryGrip = v;
		update_configuration_warnings();
	}
	godot::NodePath get_carry_grip() const
	{
		return m_carryGrip;
	}
	void set_other_hand( int v )
	{
		m_otherHand = v;
		update_configuration_warnings();
	}
	int get_other_hand() const
	{
		return m_otherHand;
	}
	void set_other_grip( const godot::NodePath& v )
	{
		m_otherGrip = v;
		update_configuration_warnings();
	}
	godot::NodePath get_other_grip() const
	{
		return m_otherGrip;
	}
	// Where the item is held, in its scene's own frame (identity without a carrying grip). `root`
	// is the scene's root: an instance of it in the game, wherever it hangs.
	static godot::Transform3D CarryFrameUnder( const godot::Node* root );
	static godot::Transform3D carry_frame_under( godot::Node* root )
	{
		return root != nullptr ? CarryFrameUnder( root ) : godot::Transform3D();
	}

	// Where a built-in hand socket sits in the items' hand frame (AnimSet::AttachFrame): the grip a
	// little along the fingers, the item pointing where the hand points.
	static godot::Transform3D HandSocketFrame();

	// In the editor: the item in a hand. A stand-in figure (the bones of the SDK's skeleton, as
	// boxes) stands beside the item and holds a copy of it by its carrying grip, in the pose of one
	// of the project's baked animation packs (res://anim/<pack>/: the first there is, or the one
	// named), its other hand where the item's settings put it. It is posed by what poses players in
	// the game, so what is seen is what the game will do with this item on the placeholder
	// character. preview_aim: with the carrying arm aimed level, as a mod that makes its holder
	// face the camera has it. Nothing of it is saved.
	void set_preview_hold( bool v );
	bool get_preview_hold() const
	{
		return m_previewHold;
	}
	void set_preview_pack( const godot::String& v );
	godot::String get_preview_pack() const
	{
		return m_previewPack;
	}
	void set_preview_aim( bool v )
	{
		m_previewAim = v;
	}
	bool get_preview_aim() const
	{
		return m_previewAim;
	}
	void _process( double delta ) override;
	// A cue, in the preview: what happens when the server's mod says so (a swing, a shot), on the
	// figure and in the first-person picture. The pack's state machine runs for real here, so a
	// state entered on the cue plays; a CbPrediction of the item's scene with that cue sets its
	// stance for a moment, as it does for the player who presses; and the item in the hand is a
	// whole copy of the scene under a director of its own, so its CbReaction nodes play (flames,
	// a muzzle flash). preview_cue is the name the inspector's button fires.
	void set_preview_cue( const godot::String& v )
	{
		m_previewCue = v.strip_edges();
	}
	godot::String get_preview_cue() const
	{
		return m_previewCue;
	}
	void fire_preview_cue( const godot::String& name );
	void fire_preview();
	godot::Callable get_preview_fire_button();
	void _validate_property( godot::PropertyInfo& property ) const;
	// What the holder sees in first person, for a picture of it (the editor's panel): `arms` is
	// posed as the game poses the view's arms for this item on the figure of the Hold preview
	// (the same pose, steadied under the eye; the item's view_offset and view_rotation; the arms
	// it shows), and a copy of the item's meshes is put under `held`, in the hand. Both are in
	// one frame, which is theirs to put anywhere. Returns { "eye": where the camera is in that
	// frame, "fov": its field of view }, or an empty one while the Hold preview is not running.
	godot::Dictionary preview_view( godot::Node* arms, godot::Node* held );

	// The text of items/<kind>.cfg, or "" (and why in the returned { text, error }).
	godot::Dictionary bake() const;
	// The inspector's button, and what saving the scene does: writes res://items/<kind>.cfg.
	// The CbMotion nodes under the item, as a motion set named like the kind (motions/<kind>.cfg):
	// each with the condition that its player has the item. Empty with no error: it has none.
	std::string BakeMotions( godot::String& error ) const;
	void BakeMotionsToProject() const;
	void bake_to_project();
	godot::Callable get_bake_button();

	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	godot::String m_kind; // "melee.bat": the kind a server mod declares
	godot::String m_displayName; // what prompts and lists call it ("Bat")
	godot::String m_animationPack;
	godot::String m_holster;
	godot::String m_useKey;
	godot::PackedStringArray m_useConditions;
	godot::PackedStringArray m_useChanges;
	double m_useCooldown = 0.0;
	godot::String m_useEmits;
	godot::Vector3 m_viewOffset;
	godot::NodePath m_viewCamera;
	godot::Vector3 m_viewRotation;
	double m_viewFov = 0.0;
	bool m_viewLeftHand = true;
	bool m_previewHold = false;
	godot::String m_previewPack;
	double m_previewTime = 0.0;
	bool m_previewAim = false;
	godot::ObjectID m_previewCopy; // the copy of the item's meshes in the figure's hand
	int m_previewCopied = 0; // frames since the copy was made
	// Where the figure's right hand holds, in the hand joint's frame: the hand socket of the
	// character this checkout's server plays (godot/characters/<name>/anim.cfg), or the built-in
	// one. The preview's animation set has the same, for the other hand's reach.
	godot::Transform3D m_previewSocket;
	godot::ObjectID m_previewFigure;
	std::string m_previewLoaded; // the folder the preview's animations came from
	std::shared_ptr<const anim::AnimSet> m_previewSet;
	std::shared_ptr<const AnimGraph> m_previewGraph;
	std::shared_ptr<anim::PoseEvaluator> m_previewPose;
	void EndPreview();
	// The preview's pose now (the pack's starting state at the preview's time, the other hand
	// solved), aimed or not: left in m_previewPose.
	void PreviewEvaluate( bool aiming );
	godot::String m_previewCue;
	// What the preview runs with (its names, the state machine's state, the cues fired): in the
	// .cpp, so this header needs none of it.
	struct PreviewRun;
	std::shared_ptr<PreviewRun> m_previewRun;
	void PreviewAdvance( double delta );
	// The item in a hand of the preview: a whole copy of this scene under a director of its own
	// (PreviewStage / player_0 / RightHand / Item), a child of `under`, with the hand at `hand`
	// (in `under`'s frame). Made again twice a second while no cue is playing (the scene is being
	// edited); its reactions run.
	void PreviewStage( godot::Node3D* under, const godot::Transform3D& hand, bool firstPerson, bool internal );
	bool m_viewRightHand = true;
	godot::Dictionary m_properties;
	godot::Dictionary m_fields;
	int m_use = USE_TOOL;
	int m_hand = HAND_SELECTED;
	bool m_bullet = false;
	bool m_slotKeyUses = false;
	int m_stack = 1;
	godot::Ref<godot::Texture2D> m_icon;
	int m_slot = 0;
	godot::NodePath m_carryGrip;
	int m_otherHand = OTHER_FREE;
	godot::NodePath m_otherGrip;
};

// The eye of an item's first-person view (CbItem::view_camera), as a camera: in the editor its
// Preview shows what the holder will see, so the item is placed by looking. In the game it is a
// place only, and that place is baked: the node takes itself out of the scene the moment it
// enters one, so nothing a look does can make the game look through it.
class CbViewCamera : public godot::Camera3D
{
	GDCLASS( CbViewCamera, godot::Camera3D )

public:
	void _notification( int what );

protected:
	static void _bind_methods()
	{
	}
};

// What a link looks like (sim/motions.h: a line a motion throws, a grappling hook's rope): a scene
// one metre long along its -Z, which the game stretches from the player to the link's end for as
// long as the link is out, flying or holding. Put it in the mod's vfx/reactions_<name>.tscn, next
// to its CbReaction nodes.
class CbLinkLook : public godot::Node
{
	GDCLASS( CbLinkLook, godot::Node )

public:
	void set_motion( const godot::String& v )
	{
		m_motion = v;
	}
	godot::String get_motion() const
	{
		return m_motion;
	}
	void set_scene( const godot::String& v )
	{
		m_scene = v;
	}
	godot::String get_scene() const
	{
		return m_scene;
	}
	void set_from( const godot::String& v )
	{
		m_from = v;
	}
	godot::String get_from() const
	{
		return m_from;
	}

protected:
	static void _bind_methods();

private:
	godot::String m_motion; // "grapple.hook/Use": the set and the node; empty: any link
	godot::String m_scene;
	godot::String m_from = "RightHand"; // the player's socket it starts at; empty: its chest
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbItem::OtherHand );
VARIANT_ENUM_CAST( cb::gd::CbItem::Use );
VARIANT_ENUM_CAST( cb::gd::CbItem::Hand );
