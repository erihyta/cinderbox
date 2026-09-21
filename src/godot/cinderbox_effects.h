#pragma once

// Effect bindings: which effect plays, on which event, for which entity — as data.
//
// A CbEffect says "when this happens, play that scene". A CbEffectTable is a list of them, saved
// as a .tres a mod can ship. The client loads every res://vfx/bindings*.tres it can find, so a mod
// adds effects by adding a file instead of replacing the game's, and never needs code.
//
// These are plain data. The scene a binding names is an ordinary Godot scene, and everything that
// decides *when* an event happens lives in the simulation.

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace cb::gd
{

class CbEffect : public godot::Resource
{
	GDCLASS( CbEffect, godot::Resource )

public:
	// Kept in sync with the strings the client emits; see game.gd.
	enum Event
	{
		EVENT_SPAWNED = 0,
		EVENT_DESTROYING = 1,
		EVENT_JUMPED = 2,
		EVENT_LANDED = 3,
	};

	enum Who
	{
		WHO_ANYONE = 0,
		WHO_LOCAL = 1,
		WHO_REMOTE = 2,
	};

	void set_event( int value )
	{
		m_event = value;
	}
	int get_event() const
	{
		return m_event;
	}
	void set_template_name( const godot::String& value )
	{
		m_templateName = value;
	}
	godot::String get_template_name() const
	{
		return m_templateName;
	}
	void set_kind( const godot::String& value )
	{
		m_kind = value;
	}
	godot::String get_kind() const
	{
		return m_kind;
	}
	void set_scene( const godot::String& value )
	{
		m_scene = value;
	}
	godot::String get_scene() const
	{
		return m_scene;
	}
	void set_offset( const godot::Vector3& value )
	{
		m_offset = value;
	}
	godot::Vector3 get_offset() const
	{
		return m_offset;
	}
	void set_lifetime( float value )
	{
		m_lifetime = value;
	}
	float get_lifetime() const
	{
		return m_lifetime;
	}
	void set_follow( bool value )
	{
		m_follow = value;
	}
	bool get_follow() const
	{
		return m_follow;
	}
	void set_who( int value )
	{
		m_who = value;
	}
	int get_who() const
	{
		return m_who;
	}

protected:
	static void _bind_methods();

private:
	int m_event = EVENT_SPAWNED;
	// Empty matches any map template; otherwise the template's name.
	godot::String m_templateName;
	// "", "any", "prop", "player" or "static".
	godot::String m_kind;
	// Scene to play, for example "res://vfx/prop_spawn.tscn".
	godot::String m_scene;
	godot::Vector3 m_offset;
	float m_lifetime = 3.0f;
	// Parent the effect to the entity's node so it follows it, instead of staying where it started.
	bool m_follow = false;
	int m_who = WHO_ANYONE;
};

class CbEffectTable : public godot::Resource
{
	GDCLASS( CbEffectTable, godot::Resource )

public:
	void set_effects( const godot::TypedArray<CbEffect>& effects )
	{
		m_effects = effects;
	}
	godot::TypedArray<CbEffect> get_effects() const
	{
		return m_effects;
	}

protected:
	static void _bind_methods();

private:
	godot::TypedArray<CbEffect> m_effects;
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbEffect::Event );
VARIANT_ENUM_CAST( cb::gd::CbEffect::Who );
