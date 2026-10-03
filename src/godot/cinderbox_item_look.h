#pragma once

// How a kind of item looks, as a node in a world reactions scene (res://vfx/reactions*.tscn): the
// scene drawn in its holder's socket, or where it lies in the world ("melee.bat" ->
// res://prefabs/bat.tscn), and what prompts call it ("Bat"). The scene is the item's frame (the
// socket's): the grip at the origin, pointing along -Z.

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/marker3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace cb::gd
{

class CbItemLook : public godot::Node
{
	GDCLASS( CbItemLook, godot::Node )

public:
	void set_kind( const godot::String& v )
	{
		m_kind = v;
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
	void set_scene( const godot::String& v )
	{
		m_scene = v;
	}
	godot::String get_scene() const
	{
		return m_scene;
	}

protected:
	static void _bind_methods();

private:
	godot::String m_kind;
	godot::String m_scene;
	godot::String m_displayName;
	godot::Vector3 m_viewOffset;
};

// The body an item has when it lies in the world, authored in the item's own scene: a box or a
// sphere (Godot's shape gizmo shows it), placed where the shape's centre is from the grip, and its
// mass, and what else the server should know about the item (`properties`: named numbers any mod
// may read, "pickup.hold_seconds" = 0.5). Bake (the button in the inspector, or
// addons/cinderbox_maps/bake_items.gd, which publishing a mod runs) writes it to items/<kind>.cfg,
// which the server reads from the mod's item. In the game the node does nothing: the simulation
// owns the physics.
class CbItemBody : public godot::CollisionShape3D
{
	GDCLASS( CbItemBody, godot::CollisionShape3D )

public:
	void set_mass( double v )
	{
		m_mass = v;
	}
	double get_mass() const
	{
		return m_mass;
	}
	// Named numbers about the item ("pickup.hold_seconds": 0.5), baked with the body. They replace
	// what the item's mod declared in code for the same names.
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
	// The inspector's button: writes res://items/<kind>.cfg for every kind whose CbItemLook (in
	// res://vfx/reactions*.tscn) draws this scene.
	void bake_to_project();
	godot::Callable get_bake_button();

	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	double m_mass = 1.0;
	godot::Dictionary m_properties;
};

// Where a hand holds an item: a marker in the item's scene, one per hand.
//
//   The carrying hand   the item is carried here: this point is in the hand's socket (whichever hand
//                       the item is in), the marker's -Z pointing along the fingers and +Y up. Without
//                       one, the item is carried at its scene's origin.
//   The other hand      the character's other arm is bent so that its hand is here, wherever the
//                       carrying hand and the animation take the item, while that hand is empty.
//
// Both are baked with the item's body (items/<kind>.cfg): the body and the other hand's grip are
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

protected:
	static void _bind_methods();

private:
	bool m_alignRotation = false;
	int m_hand = HAND_OTHER;
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbGrip::Hand );
