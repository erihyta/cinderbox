#pragma once

// What makes a scene an item, and what a link looks like.
//
//   CbItem      the root of an item's own scene: its kind, its name, its weight, its first-person
//               view, where the hands hold it, and what else the server should know. Its body is
//               the CollisionShape3D under it, its grips are two markers it names, its look is
//               everything else in the scene. Baked to items/<kind>.cfg, which the server reads and the game finds the
//               scene by.
//   CbGrip      a marker for a hand, kept for hand placements to come; items do not use it
//   CbLinkLook  what the line of a motion's probe is drawn as

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/marker3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace cb::gd
{

class CbItem : public godot::Node3D
{
	GDCLASS( CbItem, godot::Node3D )

public:
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
	void set_mass( double v )
	{
		m_mass = v;
	}
	double get_mass() const
	{
		return m_mass;
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

	// The text of items/<kind>.cfg, or "" (and why in the returned { text, error }).
	godot::Dictionary bake() const;
	// The inspector's button, and what saving the scene does: writes res://items/<kind>.cfg.
	void bake_to_project();
	godot::Callable get_bake_button();

	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	godot::String m_kind; // "melee.bat": the kind a server mod declares
	godot::String m_displayName; // what prompts and lists call it ("Bat")
	double m_mass = 1.0;
	godot::Vector3 m_viewOffset;
	godot::Dictionary m_properties;
	godot::NodePath m_carryGrip;
	int m_otherHand = OTHER_FREE;
	godot::NodePath m_otherGrip;
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
	godot::String m_motion; // "grapple.moves/Hook": the set and the node; empty: any link
	godot::String m_scene;
	godot::String m_from = "RightHand"; // the player's socket it starts at; empty: its chest
};

// A marker for a hand. Items name their grips themselves (CbItem::carry_grip, other_grip) and do
// not use it; it is kept for hand placements to come (a ledge, a wheel, another player).
class CbGrip : public godot::Marker3D
{
	GDCLASS( CbGrip, godot::Marker3D )

protected:
	static void _bind_methods()
	{
	}
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbItem::OtherHand );
