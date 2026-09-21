#pragma once

// Authoring nodes for entities made of simulation components.
//
//   CbTemplate   defines a named entity: what it looks like and which components it has.
//   CbComponent  one component on a template, its fields taken from the simulation's registry.
//   CbEntity     places a template in the level, or defines one inline from its own children.
//
// A CbComponent builds its inspector from cb::AuthorableComponents() (src/sim/reflect.h), so a
// field added to the simulation shows up in the editor with no code on this side. That is what
// keeps map authoring drag-and-drop: the schema travels from the simulation, the author only
// picks values, and the baker writes them into the .cbmap.
//
// None of these nodes do anything at runtime. The entities they describe are created by the
// simulation, on the server and inside every client's prediction.

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace cb::gd
{

// A component is not spatial: it describes its entity, so it has no transform of its own.
class CbComponent : public godot::Node
{
	GDCLASS( CbComponent, godot::Node )

public:
	void set_component( int index );
	int get_component() const
	{
		return m_component;
	}

	// Field values by name, in the registry's units. Read by the baker.
	godot::Dictionary get_values() const;
	// Name of the selected component ("Shape", "Body", ...), "" when none is selected.
	godot::String component_name() const;

protected:
	static void _bind_methods();
	void _get_property_list( godot::List<godot::PropertyInfo>* list ) const;
	bool _set( const godot::StringName& name, const godot::Variant& value );
	bool _get( const godot::StringName& name, godot::Variant& out ) const;
	bool _property_can_revert( const godot::StringName& name ) const;
	bool _property_get_revert( const godot::StringName& name, godot::Variant& out ) const;

private:
	int m_component = 0;
	godot::Dictionary m_values;
};

class CbTemplate : public godot::Node3D
{
	GDCLASS( CbTemplate, godot::Node3D )

public:
	void set_template_name( const godot::String& name )
	{
		m_name = name;
	}
	godot::String get_template_name() const
	{
		return m_name;
	}
	void set_visual( const godot::String& visual )
	{
		m_visual = visual;
	}
	godot::String get_visual() const
	{
		return m_visual;
	}
	void set_spawnable( bool value )
	{
		m_spawnable = value;
	}
	bool get_spawnable() const
	{
		return m_spawnable;
	}

protected:
	static void _bind_methods();

private:
	godot::String m_name;
	// Prefab clients draw for it: res://prefabs/<visual>.tscn. Empty falls back to the shape.
	godot::String m_visual;
	// True for the template the spawn button creates. One per map.
	bool m_spawnable = false;
};

class CbEntity : public godot::Node3D
{
	GDCLASS( CbEntity, godot::Node3D )

public:
	void set_template_name( const godot::String& name )
	{
		m_template = name;
	}
	godot::String get_template_name() const
	{
		return m_template;
	}
	void set_visual( const godot::String& visual )
	{
		m_visual = visual;
	}
	godot::String get_visual() const
	{
		return m_visual;
	}

protected:
	static void _bind_methods();

private:
	// Empty uses this node's own CbComponent children as an inline template.
	godot::String m_template;
	godot::String m_visual;
};

} // namespace cb::gd
