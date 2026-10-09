#pragma once

// Authoring a mod's motions: what its players can do besides walking and jumping, with no code.
//
// A CbReaction says: on a cue, or while conditions hold, do this to the look. A CbMotion says: on
// a press, on a cue, or while conditions hold, do this to the mover. Same shape, same expression
// language. The difference is where it runs: a reaction runs in the viewer; a motion is baked to
// text, read by the server from the mod's item, sent to every client, and run by every simulation
// (sim/motions.h). So a player's own dash starts on the tick of the press, in its own prediction,
// and a rollback replays it.
//
// A motion is a trigger, and its children are what it does:
//
//   Moves               CbMotionSet   set_name "grapple.moves"       (motion_sets/grapple_moves.tscn)
//   ├── Dash            CbMotion      action "dash"   conditions dash.charges > 0   changes dash.charges -= 1
//   │   └── Push        CbImpulse     11 m/s along the move input, replacing the horizontal velocity
//   └── Hook            CbMotion      action "grapple"   conditions not linked   until pressed.grapple
//       ├── Line        CbProbe       40 m along the look, flying at 60 m/s: the motion happens if it finds something
//       ├── Pull        CbForce       on the player, toward what the line found: 1900 N, ramped in, reacting
//       └── Rope        CbLink        to what the line found: a rope, reeled in at 4 m/s
//
// One family: every node here is a CbMotionPart (it bakes to lines of the set's file, and says what
// is wrong with it). The effects (CbImpulse, CbForce, CbLink) are CbMotionEffects: they share who
// they act on (the player, what the probe found, the entity a field names) and along what.
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

// What every motion node is: something that bakes to lines of its set's file.
class CbMotionPart : public godot::Node
{
	GDCLASS( CbMotionPart, godot::Node )

public:
	// This node's lines (its children's included), or "" with `error` when it cannot be baked.
	virtual std::string Bake( godot::String& error ) const;
	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods()
	{
	}
	// What is not wrong, but worth saying (a motion that does nothing yet).
	virtual void Advice( godot::PackedStringArray& ) const
	{
	}
	// Tells the editor to ask this node, and the ones above it, for their warnings again.
	void Changed();
	godot::String Fail( godot::String& error, const godot::String& why ) const;
};

#define CB_MOTION_FIELD( Type, name, member )                                                                                    \
	void set_##name( const Type& v )                                                                                             \
	{                                                                                                                            \
		member = v;                                                                                                              \
		Changed();                                                                                                               \
	}                                                                                                                            \
	Type get_##name() const                                                                                                      \
	{                                                                                                                            \
		return member;                                                                                                           \
	}

// The trigger: when it happens, for how long it is on, and what that changes about the player
// itself. Its children are its probe and its effects.
class CbMotion : public CbMotionPart
{
	GDCLASS( CbMotion, CbMotionPart )

public:
	enum When
	{
		WHEN_PRESS = 0,
		WHEN_WHILE = 1,
		WHEN_EVENT = 2,
	};
	enum Refill
	{
		REFILL_ON_GROUND = 0,
		REFILL_AFTER_SECONDS = 1,
	};

	std::string Bake( godot::String& error ) const override;

	CB_MOTION_FIELD( int, when, m_when )
	CB_MOTION_FIELD( godot::String, action, m_action )
	CB_MOTION_FIELD( godot::String, event, m_event )
	CB_MOTION_FIELD( godot::PackedStringArray, conditions, m_conditions )
	CB_MOTION_FIELD( double, cooldown, m_cooldown )
	CB_MOTION_FIELD( int, uses, m_uses )
	CB_MOTION_FIELD( int, refill, m_refill )
	CB_MOTION_FIELD( double, refill_seconds, m_refillSeconds )
	CB_MOTION_FIELD( double, duration, m_duration )
	CB_MOTION_FIELD( godot::PackedStringArray, until, m_until )
	CB_MOTION_FIELD( godot::Dictionary, parameters, m_parameters )
	CB_MOTION_FIELD( godot::PackedStringArray, changes, m_changes )
	CB_MOTION_FIELD( godot::String, emits, m_emits )

protected:
	static void _bind_methods();
	void Advice( godot::PackedStringArray& advice ) const override;

private:
	int m_when = WHEN_PRESS;
	godot::String m_action;
	godot::String m_event;
	godot::PackedStringArray m_conditions;
	double m_cooldown = 0.0;
	int m_uses = 0;
	int m_refill = REFILL_ON_GROUND;
	double m_refillSeconds = 1.0;
	double m_duration = 0.0;
	godot::PackedStringArray m_until;
	godot::Dictionary m_parameters;
	godot::PackedStringArray m_changes;
	godot::String m_emits;
};

// A line thrown along the look at what is under the crosshair. With one, its motion happens only
// if the line finds something, and is on from when it takes hold until that is gone.
class CbProbe : public CbMotionPart
{
	GDCLASS( CbProbe, CbMotionPart )

public:
	std::string Bake( godot::String& error ) const override;

	CB_MOTION_FIELD( double, range, m_range )
	CB_MOTION_FIELD( double, travel, m_travel )

protected:
	static void _bind_methods();

private:
	double m_range = 40.0;
	double m_travel = 60.0;
};

// Once, when the motion starts: an item of a kind is thrown into the world, in every simulation
// alike, so the thrower sees it leave on the tick of the press. A grenade, a ball, a rocket.
class CbLaunch : public CbMotionPart
{
	GDCLASS( CbLaunch, CbMotionPart )

public:
	enum Frame
	{
		FRAME_LOOK = 0,
		FRAME_MOVE = 1,
		FRAME_FACING = 2,
		FRAME_UP = 3,
		FRAME_WORLD = 4,
	};

	std::string Bake( godot::String& error ) const override;

	CB_MOTION_FIELD( godot::String, item_kind, m_itemKind )
	CB_MOTION_FIELD( double, speed, m_speed )
	CB_MOTION_FIELD( double, lift, m_lift )
	CB_MOTION_FIELD( int, frame, m_frame )
	CB_MOTION_FIELD( godot::Vector3, direction, m_direction )
	CB_MOTION_FIELD( double, ahead, m_ahead )
	CB_MOTION_FIELD( double, seconds, m_seconds )

protected:
	static void _bind_methods();

private:
	godot::String m_itemKind;
	double m_speed = 15.0;
	double m_lift = 0.0;
	int m_frame = FRAME_LOOK;
	godot::Vector3 m_direction = godot::Vector3( 0, 1, 0 );
	double m_ahead = 0.7;
	double m_seconds = 5.0;
};

// What a motion does to something: who it acts on, and along what. The base of the effects.
class CbMotionEffect : public CbMotionPart
{
	GDCLASS( CbMotionEffect, CbMotionPart )

public:
	enum Target
	{
		TARGET_SELF = 0,
		TARGET_HIT = 1,
		TARGET_FIELD = 2,
	};
	enum Frame
	{
		FRAME_LOOK = 0,
		FRAME_MOVE = 1,
		FRAME_FACING = 2,
		FRAME_UP = 3,
		FRAME_WORLD = 4,
		FRAME_TO = 5,
	};

	CB_MOTION_FIELD( int, target, m_target )
	CB_MOTION_FIELD( godot::String, target_field, m_targetField )
	CB_MOTION_FIELD( int, frame, m_frame )
	CB_MOTION_FIELD( godot::Vector3, direction, m_direction )

protected:
	static void _bind_methods();
	// "self", "hit" or "@field": "" with `error` when it is a field without a name.
	std::string TargetWord( godot::String& error ) const;
	// The frame's word, and for a world frame its direction to end the line with.
	std::string FrameWord() const;
	std::string WorldWords() const;

	int m_target = TARGET_SELF;
	godot::String m_targetField;
	int m_frame = FRAME_MOVE;
	godot::Vector3 m_direction = godot::Vector3( 0, 1, 0 );
};

// Once, when the motion starts: a change of the target's velocity. A dash, a jump, a launch.
class CbImpulse : public CbMotionEffect
{
	GDCLASS( CbImpulse, CbMotionEffect )

public:
	enum Replace
	{
		REPLACE_NOTHING = 0,
		REPLACE_VERTICAL = 1,
		REPLACE_HORIZONTAL = 2,
		REPLACE_ALL = 3,
	};

	std::string Bake( godot::String& error ) const override;

	CB_MOTION_FIELD( double, speed, m_speed )
	CB_MOTION_FIELD( int, replace, m_replace )

protected:
	static void _bind_methods();

private:
	double m_speed = 0.0;
	int m_replace = REPLACE_NOTHING;
};

// For as long as the motion is on: a push on the target, ramped in. A thrust, a pull, a current.
class CbForce : public CbMotionEffect
{
	GDCLASS( CbForce, CbMotionEffect )

public:
	enum Kind
	{
		KIND_ACCELERATION = 0,
		KIND_FORCE = 1,
		KIND_VELOCITY = 2,
	};

	std::string Bake( godot::String& error ) const override;

	CB_MOTION_FIELD( int, kind, m_kind )
	CB_MOTION_FIELD( double, strength, m_strength )
	CB_MOTION_FIELD( double, speed, m_speed )
	CB_MOTION_FIELD( double, ramp_in, m_ramp )
	CB_MOTION_FIELD( bool, react, m_react )

protected:
	static void _bind_methods();

private:
	int m_kind = KIND_ACCELERATION;
	double m_strength = 0.0;
	double m_speed = 0.0;
	double m_ramp = 0.0;
	bool m_react = false;
};

// For as long as the motion is on: a rope between the player and the target.
class CbLink : public CbMotionEffect
{
	GDCLASS( CbLink, CbMotionEffect )

public:
	CbLink()
	{
		m_target = TARGET_HIT;
	}
	std::string Bake( godot::String& error ) const override;
	void _validate_property( godot::PropertyInfo& property ) const;

	CB_MOTION_FIELD( double, length, m_length )
	CB_MOTION_FIELD( double, reel, m_reel )

protected:
	static void _bind_methods();

private:
	double m_length = 0.0;
	double m_reel = 0.0;
};

// The root: a mod's motions, baked to one file.
class CbMotionSet : public CbMotionPart
{
	GDCLASS( CbMotionSet, CbMotionPart )

public:
	// The whole file: its header, then every CbMotion under it in tree order.
	std::string Bake( godot::String& error ) const override;

	void set_set_name( const godot::String& v )
	{
		m_name = v;
		Changed();
	}
	godot::String get_set_name() const
	{
		return m_name;
	}

	// The set's file: { text, error, motions } (text is empty when it cannot be baked).
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

#undef CB_MOTION_FIELD

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
VARIANT_ENUM_CAST( cb::gd::CbMotion::Refill );
VARIANT_ENUM_CAST( cb::gd::CbLaunch::Frame );
VARIANT_ENUM_CAST( cb::gd::CbMotionEffect::Target );
VARIANT_ENUM_CAST( cb::gd::CbMotionEffect::Frame );
VARIANT_ENUM_CAST( cb::gd::CbImpulse::Replace );
VARIANT_ENUM_CAST( cb::gd::CbForce::Kind );
