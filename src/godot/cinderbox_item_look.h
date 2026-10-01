#pragma once

// How a kind of item looks, as a node in a world reactions scene (res://vfx/reactions*.tscn): the
// scene drawn in its holder's socket, or where it lies in the world ("melee.bat" ->
// res://prefabs/bat.tscn), and what prompts call it ("Bat"). The scene is the item's frame (the
// socket's): the grip at the origin, pointing along -Z.

#include <godot_cpp/classes/collision_shape3d.hpp>
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

} // namespace cb::gd
