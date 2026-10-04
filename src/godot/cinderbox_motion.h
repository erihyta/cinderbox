#pragma once

// Authoring a mod's motions: what its players can do besides walking and jumping, with no code.
//
// A CbReaction says: on a cue, or while conditions hold, do this to the look. A CbMotion says: on
// a press, on a cue, or while conditions hold, do this to the mover. Same shape, same expression language. The
// difference is where it runs: a reaction runs in the viewer; a motion is baked to text, read by
// the server from the mod's item, sent to every client, and run by every simulation
// (sim/motions.h). So a player's own dash starts on the tick of the press, in its own prediction,
// and a rollback replays it.
//
//   Moves                 CbMotionSet   set_name "dash.moves"      (motion_sets/dash_moves.tscn)
//   ├── Dash              CbMotion      action "dash"   conditions dash.charges > 0
//   │                                   impulse 12 m/s along the move input, replacing the horizontal velocity
//   │                                   duration 0.2 s with parameters { friction: 0 }
//   │                                   changes dash.charges -= 1      emits dash.started
//   └── DoubleJump        CbMotion      action "jump"   conditions not grounded
//                                       uses 1, back on the ground     impulse 6.5 m/s up, replacing the vertical speed
//
// The set bakes to res://motions/<set_name>.cfg when its scene is saved (and when the mod is
// published); the server mod names it: declare.Motions( "dash.moves" ). The rules stay the mod's:
// it declares the action, the fields and the events, and decides who may (a field a condition
// reads). In the game these nodes do nothing.

#include <godot_cpp/classes/editor_inspector_plugin.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <string>

namespace cb::gd
{

class CbMotion : public godot::Node
{
	GDCLASS( CbMotion, godot::Node )

public:
	enum When
	{
		WHEN_PRESS = 0,
		WHEN_WHILE = 1,
		WHEN_EVENT = 2,
	};
	enum Frame
	{
		FRAME_LOOK = 0,
		FRAME_MOVE = 1,
		FRAME_FACING = 2,
		FRAME_UP = 3,
		FRAME_WORLD = 4,
	};
	enum Replace
	{
		REPLACE_NOTHING = 0,
		REPLACE_VERTICAL = 1,
		REPLACE_HORIZONTAL = 2,
		REPLACE_ALL = 3,
	};
	enum Refill
	{
		REFILL_ON_GROUND = 0,
		REFILL_AFTER_SECONDS = 1,
	};

	godot::PackedStringArray _get_configuration_warnings() const override;

	// This motion's lines of the set's file, or "" with `error` when it cannot be baked.
	std::string Bake( godot::String& error ) const;

#define CB_MOTION_FIELD( Type, name, member )                                                                                    \
	void set_##name( const Type& v )                                                                                             \
	{                                                                                                                            \
		member = v;                                                                                                              \
		update_configuration_warnings();                                                                                         \
	}                                                                                                                            \
	Type get_##name() const                                                                                                      \
	{                                                                                                                            \
		return member;                                                                                                           \
	}
	CB_MOTION_FIELD( int, when, m_when )
	CB_MOTION_FIELD( godot::String, action, m_action )
	CB_MOTION_FIELD( godot::String, event, m_event )
	CB_MOTION_FIELD( godot::PackedStringArray, conditions, m_conditions )
	CB_MOTION_FIELD( double, cooldown, m_cooldown )
	CB_MOTION_FIELD( int, uses, m_uses )
	CB_MOTION_FIELD( int, refill, m_refill )
	CB_MOTION_FIELD( double, refill_seconds, m_refillSeconds )
	CB_MOTION_FIELD( double, impulse, m_impulse )
	CB_MOTION_FIELD( int, impulse_frame, m_frame )
	CB_MOTION_FIELD( godot::Vector3, impulse_direction, m_direction )
	CB_MOTION_FIELD( int, replace, m_replace )
	CB_MOTION_FIELD( double, duration, m_duration )
	CB_MOTION_FIELD( godot::Dictionary, parameters, m_parameters )
	CB_MOTION_FIELD( godot::PackedStringArray, changes, m_changes )
	CB_MOTION_FIELD( godot::String, emits, m_emits )
#undef CB_MOTION_FIELD

protected:
	static void _bind_methods();

private:
	int m_when = WHEN_PRESS;
	godot::String m_action;
	godot::String m_event;
	godot::PackedStringArray m_conditions;
	double m_cooldown = 0.0;
	int m_uses = 0;
	int m_refill = REFILL_ON_GROUND;
	double m_refillSeconds = 1.0;
	double m_impulse = 0.0;
	int m_frame = FRAME_MOVE;
	godot::Vector3 m_direction = godot::Vector3( 0, 1, 0 );
	int m_replace = REPLACE_NOTHING;
	double m_duration = 0.0;
	godot::Dictionary m_parameters;
	godot::PackedStringArray m_changes;
	godot::String m_emits;
};

class CbMotionSet : public godot::Node
{
	GDCLASS( CbMotionSet, godot::Node )

public:
	godot::PackedStringArray _get_configuration_warnings() const override;

	void set_set_name( const godot::String& v )
	{
		m_name = v;
		update_configuration_warnings();
	}
	godot::String get_set_name() const
	{
		return m_name;
	}

	// The set's file: { text, error } (text is empty when it cannot be baked).
	godot::Dictionary bake() const;
	// What the Bake button and saving the scene do: writes res://motions/<set_name>.cfg, only when
	// its bytes change.
	void bake_to_project();
	godot::Callable get_bake_button();

protected:
	static void _bind_methods();

private:
	godot::String m_name;
};

// An info row at the top of the motion nodes' inspector groups, like a reaction's.
class CbMotionInspector : public godot::EditorInspectorPlugin
{
	GDCLASS( CbMotionInspector, godot::EditorInspectorPlugin )

public:
	bool _can_handle( godot::Object* object ) const override;
	void _parse_begin( godot::Object* object ) override;
	void _parse_group( godot::Object* object, const godot::String& group ) override;

protected:
	static void _bind_methods()
	{
	}
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbMotion::When );
VARIANT_ENUM_CAST( cb::gd::CbMotion::Frame );
VARIANT_ENUM_CAST( cb::gd::CbMotion::Replace );
VARIANT_ENUM_CAST( cb::gd::CbMotion::Refill );
