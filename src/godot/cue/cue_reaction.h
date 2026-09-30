#pragma once

// A scene's reaction to cues and state, with no code. Put CbReaction nodes anywhere under a
// CbDirector: inside an entity's scene (a held item, a character, a prop) or in a scene of their own
// (world reactions: sounds at every gunshot). The director runs them from what it is told: cues
// ("melee.hit" at an entity) and entity state ("melee.hot" = true).
//
//   Bat                                (an entity: the director knows it)
//   ├── Barrel
//   ├── Sparks
//   ├── CbReaction  while "melee.hot"                      set  Barrel : surface_material_override/0:emission_energy_multiplier = 4
//   └── CbReaction  on "melee.hit"   subject ^^            call Sparks.restart()
//
// Every node path is a cue path (cue_paths.h): an ordinary Godot path from the reaction, or an
// anchor and a path from it: ^ (my entity), ^^ (the entity above it: a held item's holder), $at,
// $other (the cue's entities), $local, $world.
//
// When:    on a cue, or while conditions hold. The subject (default ^) is whose state plain
//          condition names read; a cue must name the subject on its event_side (A: the cue is at
//          it, B: it is the other one), unless the subject starts from $at or $other (then every
//          cue of the name is about whoever it names).
// Timing:  a cue reaction may wait (delay), act only sometimes (chance) and not too often (cooldown).
// Do:      any of: play an animation (and another, or a stop, when a "while" ends); set a property,
//          at once or blended over blend_time (put back when a "while" ends); call a method with
//          method_args; add a scene (freed after scene_lifetime for a cue, when a "while" ends);
//          play a sound; shake the camera or flash the screen (the director's screen_effect
//          signal: pair them with is_local).
// Place:   where a scene or sound goes: under its parent, at the cue's point or end, a beam from
//          place_node (or the point) to the end, at place_node, or following the subject.
//
// Presentation only, and contained: paths never leave the director's tree, and free, queue_free
// and script are refused.

#include "cue_paths.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/tween.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <vector>

namespace cb::gd
{

class CbDirector;

class CbReaction : public godot::Node
{
	GDCLASS( CbReaction, godot::Node )

public:
	enum When
	{
		WHEN_EVENT = 0,
		WHEN_WHILE = 1,
	};
	enum EventSide
	{
		SIDE_A = 0,
		SIDE_B = 1,
		SIDE_EITHER = 2,
	};
	enum Place
	{
		PLACE_PARENT = 0,
		PLACE_EVENT_POINT = 1,
		PLACE_EVENT_END = 2,
		PLACE_BEAM = 3,
		PLACE_NODE = 4,
		PLACE_FOLLOW = 5,
	};

	// Without a director: resolved from this node alone (anchors to cues find nothing).
	void fire();
	void set_on( bool on );
	bool is_on() const
	{
		return m_on;
	}

	// For the director.
	bool IsWhile() const
	{
		return m_when == WHEN_WHILE;
	}
	const godot::String& Event() const
	{
		return m_event;
	}
	// Whether this cue is for this reaction, and its conditions hold: then it acts (now, or after its
	// delay). True when it did.
	bool Fire( const cue::Context& context, double now );
	// Why this cue would or would not make it act ("acts", "condition melee.hot is false", ...),
	// without acting. For the Cue Preview.
	godot::String Explain( const cue::Context& context, double now ) const;
	// Re-checks a "while" (no cue).
	void Update( const cue::Context& context );
	bool HasScreenEffect() const
	{
		return m_shake > 0.0 || m_flashColor.a > 0.0f;
	}

#define CB_REACTION_FIELD( Type, name, member )                                                                                  \
	void set_##name( const Type& v )                                                                                             \
	{                                                                                                                            \
		member = v;                                                                                                              \
		Changed();                                                                                                               \
	}                                                                                                                            \
	Type get_##name() const                                                                                                      \
	{                                                                                                                            \
		return member;                                                                                                           \
	}
	CB_REACTION_FIELD( int, when, m_when )
	CB_REACTION_FIELD( godot::String, event, m_event )
	CB_REACTION_FIELD( int, event_side, m_eventSide )
	CB_REACTION_FIELD( godot::NodePath, subject, m_subject )
	CB_REACTION_FIELD( godot::String, subject_kind, m_subjectKind )
	CB_REACTION_FIELD( godot::String, subject_template, m_subjectTemplate )
	CB_REACTION_FIELD( godot::PackedStringArray, conditions, m_conditions )
	CB_REACTION_FIELD( double, cooldown, m_cooldown )
	CB_REACTION_FIELD( double, delay, m_delay )
	CB_REACTION_FIELD( double, chance, m_chance )
	CB_REACTION_FIELD( godot::NodePath, animation_player, m_player )
	CB_REACTION_FIELD( godot::String, animation, m_animation )
	CB_REACTION_FIELD( godot::String, animation_off, m_animationOff )
	CB_REACTION_FIELD( godot::NodePath, target, m_target )
	CB_REACTION_FIELD( godot::String, property, m_property )
	CB_REACTION_FIELD( godot::Variant, value, m_value )
	CB_REACTION_FIELD( double, blend_time, m_blendTime )
	CB_REACTION_FIELD( godot::String, method, m_method )
	CB_REACTION_FIELD( godot::Array, method_args, m_methodArgs )
	CB_REACTION_FIELD( godot::String, scene, m_scene )
	CB_REACTION_FIELD( godot::NodePath, scene_parent, m_sceneParent )
	CB_REACTION_FIELD( double, scene_lifetime, m_sceneLifetime )
	CB_REACTION_FIELD( int, place, m_place )
	CB_REACTION_FIELD( godot::NodePath, place_node, m_placeNode )
	CB_REACTION_FIELD( godot::Vector3, offset, m_offset )
	CB_REACTION_FIELD( godot::String, sound, m_sound )
	CB_REACTION_FIELD( double, volume_db, m_volumeDb )
	CB_REACTION_FIELD( double, pitch_scale, m_pitchScale )
	CB_REACTION_FIELD( double, pitch_jitter, m_pitchJitter )
	CB_REACTION_FIELD( godot::String, bus, m_bus )
	CB_REACTION_FIELD( double, max_distance, m_maxDistance )
	CB_REACTION_FIELD( double, shake, m_shake )
	CB_REACTION_FIELD( double, shake_time, m_shakeTime )
	CB_REACTION_FIELD( godot::Color, flash_color, m_flashColor )
	CB_REACTION_FIELD( double, flash_time, m_flashTime )
#undef CB_REACTION_FIELD

	void _notification( int what );
	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	void Changed();
	bool Parse();
	bool Holds( const cue::Context& context, godot::Node* subject, godot::String* why = nullptr ) const;
	// The part of Fire that decides; "" when it acts.
	godot::String Refusal( const cue::Context& context, double now, bool rolled ) const;
	void Delayed( uint64_t at, uint64_t other, godot::Dictionary args );
	void SetProperty( godot::Node* target, const godot::NodePath& path, const godot::Variant& value );
	godot::Node* Subject( const cue::Context& context ) const;
	void Act( bool on, const cue::Context& context );
	void PlaySound( godot::Node* parent, bool global, const godot::Vector3& where );
	static bool Refused( const godot::String& method, const godot::String& property );
	void End(); // a "while" that is on ends where it acted (leaving the tree)

	int m_when = WHEN_EVENT;
	godot::String m_event;
	int m_eventSide = SIDE_A;
	godot::NodePath m_subject;
	godot::String m_subjectKind;
	godot::String m_subjectTemplate;
	godot::PackedStringArray m_conditions;
	double m_cooldown = 0.0;
	double m_delay = 0.0;
	double m_chance = 1.0;
	godot::NodePath m_player;
	godot::String m_animation;
	godot::String m_animationOff;
	godot::NodePath m_target;
	godot::String m_property;
	godot::Variant m_value;
	double m_blendTime = 0.0;
	godot::String m_method;
	godot::Array m_methodArgs;
	godot::String m_scene;
	godot::NodePath m_sceneParent;
	double m_sceneLifetime = 2.0;
	int m_place = PLACE_PARENT;
	godot::NodePath m_placeNode;
	godot::Vector3 m_offset;
	godot::String m_sound;
	double m_volumeDb = 0.0;
	double m_pitchScale = 1.0;
	double m_pitchJitter = 0.0;
	godot::String m_bus;
	double m_maxDistance = 0.0;
	double m_shake = 0.0;
	double m_shakeTime = 0.3;
	godot::Color m_flashColor = godot::Color( 1, 1, 1, 0 );
	double m_flashTime = 0.15;

	CbDirector* m_director = nullptr; // while in its tree
	bool m_parsed = false;
	bool m_valid = false;
	std::vector<cue::Condition> m_tests;
	double m_lastFired = -1e9;
	bool m_warned = false; // said once that a path or condition does not parse
	godot::Ref<godot::Tween> m_tween;

	bool m_on = false;
	bool m_haveOriginal = false;
	godot::Variant m_original;	// what the property was before a "while" set it
	godot::ObjectID m_onTarget; // where a "while" set it
	godot::ObjectID m_onPlayer; // where a "while" played its animation
	godot::ObjectID m_spawned;
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbReaction::When );
VARIANT_ENUM_CAST( cb::gd::CbReaction::EventSide );
VARIANT_ENUM_CAST( cb::gd::CbReaction::Place );
