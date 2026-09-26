#pragma once

// How a scene reacts to its entity's state, with no code: put CbReaction nodes in any entity's scene
// (a character, a held item, a prop's prefab). The game drives them from what the server's mods
// declared, by name:
//
//   Bat
//   ├── Barrel
//   ├── Sparks
//   ├── CbReaction  while "melee.hot"         set  Barrel : surface_material_override/0:emission_energy_multiplier = 4
//   └── CbReaction  on event "melee.hit"      call Sparks.restart()      (subject: the holder)
//
// When:    on an event (sent to the subject), or while board conditions hold (the subject's board;
//          global fields read the global board).
// Subject: the entity the scene draws, or (for a held item) the player holding it.
// Do:      any of: play an animation on an AnimationPlayer (and another when a "while" ends); set a
//          property on a node (put back when a "while" ends); call a built-in method with no
//          arguments ("restart", "play"); add a scene as a child (removed when a "while" ends, or
//          after scene_lifetime seconds for an event).
//
// Presentation only: nothing here changes the simulation, so every screen can differ in looks but
// never in what happened. Anything that exists in the game (a prop, an item) is spawned by the
// server's mods. Node paths are relative to the reaction.

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

namespace cb::gd
{

class CbReaction : public godot::Node
{
	GDCLASS( CbReaction, godot::Node )

public:
	enum When
	{
		WHEN_EVENT = 0,
		WHEN_WHILE = 1,
	};
	enum Subject
	{
		SUBJECT_SELF = 0,
		SUBJECT_HOLDER = 1,
	};

	// An event arrived (the game checked the name, the subject and the conditions).
	void fire();
	// A "while" reaction's conditions: acts when this changes.
	void set_on( bool on );
	bool is_on() const
	{
		return m_on;
	}

	void set_when( int v )
	{
		m_when = v;
	}
	int get_when() const
	{
		return m_when;
	}
	void set_event( const godot::String& v )
	{
		m_event = v;
	}
	godot::String get_event() const
	{
		return m_event;
	}
	void set_conditions( const godot::PackedStringArray& v )
	{
		m_conditions = v;
	}
	godot::PackedStringArray get_conditions() const
	{
		return m_conditions;
	}
	void set_subject( int v )
	{
		m_subject = v;
	}
	int get_subject() const
	{
		return m_subject;
	}
	void set_animation_player( const godot::NodePath& v )
	{
		m_player = v;
	}
	godot::NodePath get_animation_player() const
	{
		return m_player;
	}
	void set_animation( const godot::String& v )
	{
		m_animation = v;
	}
	godot::String get_animation() const
	{
		return m_animation;
	}
	void set_animation_off( const godot::String& v )
	{
		m_animationOff = v;
	}
	godot::String get_animation_off() const
	{
		return m_animationOff;
	}
	void set_target( const godot::NodePath& v )
	{
		m_target = v;
	}
	godot::NodePath get_target() const
	{
		return m_target;
	}
	void set_property( const godot::String& v )
	{
		m_property = v;
	}
	godot::String get_property() const
	{
		return m_property;
	}
	void set_value( const godot::Variant& v )
	{
		m_value = v;
	}
	godot::Variant get_value() const
	{
		return m_value;
	}
	void set_method( const godot::String& v )
	{
		m_method = v;
	}
	godot::String get_method() const
	{
		return m_method;
	}
	void set_scene( const godot::String& v )
	{
		m_scene = v;
	}
	godot::String get_scene() const
	{
		return m_scene;
	}
	void set_scene_parent( const godot::NodePath& v )
	{
		m_sceneParent = v;
	}
	godot::NodePath get_scene_parent() const
	{
		return m_sceneParent;
	}
	void set_scene_lifetime( double v )
	{
		m_sceneLifetime = v;
	}
	double get_scene_lifetime() const
	{
		return m_sceneLifetime;
	}

	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	void Act( bool on );

	int m_when = WHEN_EVENT;
	godot::String m_event;
	godot::PackedStringArray m_conditions;
	int m_subject = SUBJECT_SELF;
	godot::NodePath m_player;
	godot::String m_animation;
	godot::String m_animationOff;
	godot::NodePath m_target;
	godot::String m_property;
	godot::Variant m_value;
	godot::String m_method;
	godot::String m_scene;
	godot::NodePath m_sceneParent;
	double m_sceneLifetime = 2.0;

	bool m_on = false;
	bool m_haveOriginal = false;
	godot::Variant m_original; // what the property was before a "while" set it
	godot::ObjectID m_spawned;
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbReaction::When );
VARIANT_ENUM_CAST( cb::gd::CbReaction::Subject );
