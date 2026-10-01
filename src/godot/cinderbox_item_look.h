#pragma once

// How a kind of item looks, as a node in a world reactions scene (res://vfx/reactions*.tscn): the
// scene drawn in its holder's socket, or where it lies in the world ("melee.bat" ->
// res://prefabs/bat.tscn), and what prompts call it ("Bat"). The scene is the item's frame (the
// socket's): the grip at the origin, pointing along -Z.

#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/node.hpp>

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
// mass. Bake writes it to items/<kind>.cfg (addons/cinderbox_maps/bake_items.gd; publishing a mod
// runs it), which the server reads from the mod's item. In the game the node does nothing: the
// simulation owns the physics.
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
	// The text of items/<kind>.cfg, or "" (and why in the returned { text, error }).
	godot::Dictionary bake() const;

	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	double m_mass = 1.0;
};

} // namespace cb::gd
