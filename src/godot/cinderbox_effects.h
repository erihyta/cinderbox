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
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

// A property with a plain getter and setter.
#define CB_PROPERTY( Type, name, member )                                                                                        \
	void set_##name( const Type& value )                                                                                         \
	{                                                                                                                            \
		member = value;                                                                                                          \
	}                                                                                                                            \
	Type get_##name() const                                                                                                      \
	{                                                                                                                            \
		return member;                                                                                                           \
	}

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
		EVENT_FOOTSTEP = 4,
		EVENT_IMPACT = 5,
		// A server mod's event, by name ("pistol.fired"). Entity A is the one it is about.
		EVENT_MOD = 6,
		// The local player pressed a mod action ("fire"), before the server has answered: the
		// place for feedback that must not wait a round trip. Conditions decide whether it would
		// have worked (ammo left, the right slot out).
		EVENT_ACTION = 7,
	};

	enum Subject
	{
		SUBJECT_A = 0, // who a mod event is about (the shooter, the killer)
		SUBJECT_B = 1, // the other entity it names (what was hit, who died)
	};

	enum ValueFilter
	{
		VALUE_ANY = 0,
		VALUE_POSITIVE = 1, // e.g. a hit that did damage
		VALUE_ZERO = 2,		// e.g. a hit that did not
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
	void set_cooldown( float value )
	{
		m_cooldown = value;
	}
	float get_cooldown() const
	{
		return m_cooldown;
	}

	void set_min_strength( float value )
	{
		m_minStrength = value;
	}
	float get_min_strength() const
	{
		return m_minStrength;
	}

	void set_sound( const godot::String& value )
	{
		m_sound = value;
	}
	godot::String get_sound() const
	{
		return m_sound;
	}
	void set_volume_db( float value )
	{
		m_volumeDb = value;
	}
	float get_volume_db() const
	{
		return m_volumeDb;
	}
	void set_pitch_scale( float value )
	{
		m_pitchScale = value;
	}
	float get_pitch_scale() const
	{
		return m_pitchScale;
	}
	void set_pitch_jitter( float value )
	{
		m_pitchJitter = value;
	}
	float get_pitch_jitter() const
	{
		return m_pitchJitter;
	}
	void set_bus( const godot::String& value )
	{
		m_bus = value;
	}
	godot::String get_bus() const
	{
		return m_bus;
	}
	void set_max_distance( float value )
	{
		m_maxDistance = value;
	}
	float get_max_distance() const
	{
		return m_maxDistance;
	}

	void set_shake( float value )
	{
		m_shake = value;
	}
	float get_shake() const
	{
		return m_shake;
	}
	void set_shake_time( float value )
	{
		m_shakeTime = value;
	}
	float get_shake_time() const
	{
		return m_shakeTime;
	}
	void set_flash_color( const godot::Color& value )
	{
		m_flashColor = value;
	}
	godot::Color get_flash_color() const
	{
		return m_flashColor;
	}
	void set_flash_time( float value )
	{
		m_flashTime = value;
	}
	float get_flash_time() const
	{
		return m_flashTime;
	}

	CB_PROPERTY( godot::String, name, m_name )
	CB_PROPERTY( godot::PackedStringArray, conditions, m_conditions )
	CB_PROPERTY( int, subject, m_subject )
	CB_PROPERTY( int, value_filter, m_valueFilter )
	CB_PROPERTY( godot::String, bone, m_bone )
	CB_PROPERTY( bool, at_end, m_atEnd )
	CB_PROPERTY( bool, beam, m_beam )

protected:
	static void _bind_methods();

private:
	int m_event = EVENT_SPAWNED;
	// Mod events and actions: which one, by name.
	godot::String m_name;
	// Board conditions on the subject ("pistol.ammo > 0", "!combat.dead"); all must hold.
	godot::PackedStringArray m_conditions;
	// Mod events: whose kind, template, "who", conditions and bone are checked.
	int m_subject = SUBJECT_A;
	int m_valueFilter = VALUE_ANY;
	// Play at this joint of the subject's skeleton (a humanoid-profile name, "RightHand").
	godot::String m_bone;
	// Mod events: play at the event's vector (where a shot ended) instead of its point.
	bool m_atEnd = false;
	// Stretch the scene from where it plays to the event's end point along its local -Z, like a
	// tracer. The scene should be one metre long.
	bool m_beam = false;
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
	// Shortest gap between two plays of this binding, in seconds. 0 lets it fire every time.
	float m_cooldown = 0.0f;
	// Impacts only: ignore anything softer than this approach speed, in m/s.
	float m_minStrength = 0.0f;

	// Sound played at the event, positional unless max_distance is 0.
	godot::String m_sound;
	float m_volumeDb = 0.0f;
	float m_pitchScale = 1.0f;
	// Randomises the pitch by +/- this much. Presentation only, so it never has to be repeatable.
	float m_pitchJitter = 0.0f;
	godot::String m_bus;
	float m_maxDistance = 0.0f;

	// Screen effects. They only make sense for what the viewer can feel, so bindings that use them
	// normally set who = Local player.
	float m_shake = 0.0f;
	float m_shakeTime = 0.3f;
	godot::Color m_flashColor = godot::Color( 1.0f, 1.0f, 1.0f, 0.0f );
	float m_flashTime = 0.15f;
};

// A look that holds while a condition does: "while loadout.slot == 2, hold a pistol in the right hand
// and aim that arm". Checked every frame for each matching entity, so it follows the board as the
// server's mods change it.
class CbStateBinding : public godot::Resource
{
	GDCLASS( CbStateBinding, godot::Resource )

public:
	CB_PROPERTY( godot::PackedStringArray, conditions, m_conditions )
	CB_PROPERTY( godot::String, kind, m_kind )
	CB_PROPERTY( int, who, m_who )
	CB_PROPERTY( godot::String, attach_scene, m_attachScene )
	CB_PROPERTY( godot::String, attach_bone, m_attachBone )
	CB_PROPERTY( godot::Vector3, attach_offset, m_attachOffset )
	CB_PROPERTY( godot::Vector3, attach_rotation, m_attachRotation )
	CB_PROPERTY( godot::String, tree_parameter, m_treeParameter )

protected:
	static void _bind_methods();

private:
	godot::PackedStringArray m_conditions;
	// "player" (the default), "ragdoll", "prop" or "any".
	godot::String m_kind = "player";
	int m_who = 0; // CbEffect::Who
	// A scene kept at a joint while the conditions hold (a held item).
	godot::String m_attachScene;
	godot::String m_attachBone = "RightHand";
	godot::Vector3 m_attachOffset;
	godot::Vector3 m_attachRotation; // degrees
	// An AnimationTree parameter set to whether the conditions hold, for prefabs animated by a
	// CinderboxAnimator (e.g. "parameters/conditions/armed").
	godot::String m_treeParameter;
};

// How a kind of held item looks: the scene drawn in its holder's socket ("melee.bat" ->
// res://prefabs/bat.tscn). The scene is the item's frame (the socket's): the grip at the origin,
// pointing along -Z. It may carry an AnimationPlayer (events sent to the item play its animation of
// the same name, and the character's animations can play its animations) and an AnimationTree
// (the item's board fields are its advance conditions, "parameters/conditions/<field>").
class CbItemLook : public godot::Resource
{
	GDCLASS( CbItemLook, godot::Resource )

public:
	CB_PROPERTY( godot::String, kind, m_kind )
	CB_PROPERTY( godot::String, scene, m_scene )

protected:
	static void _bind_methods();

private:
	godot::String m_kind;
	godot::String m_scene;
};

class CbEffectTable : public godot::Resource
{
	GDCLASS( CbEffectTable, godot::Resource )

public:
	void set_items( const godot::TypedArray<CbItemLook>& items )
	{
		m_items = items;
	}
	godot::TypedArray<CbItemLook> get_items() const
	{
		return m_items;
	}
	void set_effects( const godot::TypedArray<CbEffect>& effects )
	{
		m_effects = effects;
	}
	godot::TypedArray<CbEffect> get_effects() const
	{
		return m_effects;
	}
	void set_states( const godot::TypedArray<CbStateBinding>& states )
	{
		m_states = states;
	}
	godot::TypedArray<CbStateBinding> get_states() const
	{
		return m_states;
	}

protected:
	static void _bind_methods();

private:
	godot::TypedArray<CbEffect> m_effects;
	godot::TypedArray<CbStateBinding> m_states;
	godot::TypedArray<CbItemLook> m_items;
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbEffect::Event );
VARIANT_ENUM_CAST( cb::gd::CbEffect::Who );
VARIANT_ENUM_CAST( cb::gd::CbEffect::Subject );
VARIANT_ENUM_CAST( cb::gd::CbEffect::ValueFilter );
