#pragma once

// How a kind of held item looks, as a node in a world reactions scene (res://vfx/reactions*.tscn):
// the scene drawn in its holder's socket ("melee.bat" -> res://prefabs/bat.tscn). The scene is the
// item's frame (the socket's): the grip at the origin, pointing along -Z.

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
};

} // namespace cb::gd
