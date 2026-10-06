#pragma once

// What makes a scene an item, and what a link looks like.
//
//   CbItem      the root of an item's own scene: its kind, its name, its weight, its first-person
//               view, and what else the server should know. Its body is the CollisionShape3D under
//               it, its hands are the CbGrip markers under it, its look is everything else in the
//               scene. Baked to items/<kind>.cfg, which the server reads and the game finds the
//               scene by.
//   CbGrip      where a hand holds the item
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

// Where a hand holds an item: a marker in the item's scene, one per hand.
//
//   The carrying hand   the item is carried here: this point is in the hand's socket (whichever hand
//                       the item is in), the marker's -Z pointing along the fingers and +Y up. Without
//                       one, the item is carried at its scene's origin.
//   The other hand      the character's other arm is bent so that its hand is here, wherever the
//                       carrying hand and the animation take the item, while that hand is empty.
//
// Both are baked with the item (items/<kind>.cfg): the body and the other hand's grip are
// written in the carrying hand's frame, so the server poses the same arms for its hit tests and
// every player sees them. The viewer draws the scene moved so that the carrying grip is in the
// socket.
class CbGrip : public godot::Marker3D
{
	GDCLASS( CbGrip, godot::Marker3D )

public:
	enum Hand
	{
		HAND_OTHER = 0,
		HAND_CARRYING = 1,
	};
	void set_hand( int v )
	{
		m_hand = v;
	}
	int get_hand() const
	{
		return m_hand;
	}
	// The scene's carrying grip in the scene's own frame (identity when it has none): where the
	// item is held. `in` is any node of the item's scene.
	static godot::Transform3D CarryFrame( const godot::Node* in );
	// The same for the scene whose root is `root` (an instance of it in the game, wherever it hangs).
	static godot::Transform3D CarryFrameUnder( const godot::Node* root );
	static godot::Transform3D carry_frame_under( godot::Node* root )
	{
		return root != nullptr ? CarryFrameUnder( root ) : godot::Transform3D();
	}

	// The other hand: it also turns as the marker is turned, as a hand carrying an item placed here
	// would be (the marker's -Z is where such an item would point). Off: the hand keeps the turn its
	// animation gives it.
	void set_align_rotation( bool v )
	{
		m_alignRotation = v;
	}
	bool get_align_rotation() const
	{
		return m_alignRotation;
	}
	// The other hand: it stays where the item's animations have it relative to the carrying hand
	// (place and turn), and the marker's own place is not used. For animations made with both hands
	// on the item: they are kept as they are, and kept together when the carrying arm is aimed.
	void set_as_animated( bool v )
	{
		m_asAnimated = v;
	}
	bool get_as_animated() const
	{
		return m_asAnimated;
	}

protected:
	static void _bind_methods();

private:
	bool m_alignRotation = false;
	bool m_asAnimated = false;
	int m_hand = HAND_OTHER;
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbGrip::Hand );
