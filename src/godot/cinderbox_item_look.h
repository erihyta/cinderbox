#pragma once

// How a kind of item looks, as a node in a world reactions scene (res://vfx/reactions*.tscn): the
// scene drawn in its holder's socket, or where it lies in the world ("melee.bat" ->
// res://prefabs/bat.tscn), and what prompts call it ("Bat"). The scene is the item's frame (the
// socket's): the grip at the origin, pointing along -Z.

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

} // namespace cb::gd
