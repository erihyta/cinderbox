#pragma once

// How a scene reacts to its entity's state, with no code: put CbReaction nodes in any entity's scene
// (a character, a held item, a prop's prefab). The game drives them from what the server's mods
// declared, by name:
//
//   Bat
//   ├── Barrel
//   ├── Sparks
//   ├── CbReaction  while "melee.hot"         set  Barrel : surface_material_override/0:emission_energy_multiplier = 4
//   └── CbReaction  on event "melee.hit"      call Sparks.restart()      (subject: holder)
//
// Entities are named by entity paths (entity_path.h): self, holder, item:<socket>, event.a,
// event.b, local, world, chained with '/' ("holder/item:LeftHand").
//
// World reactions: a mod's res://vfx/reactions*.tscn is loaded once, not per entity; its reactions
// have no self and name what they are about from the event ("event.a"). They replace effect
// tables: a sound at every gunshot, a camera shake for the local player.
//
// Events: a mod's by name ("melee.hit"); the simulation's own: spawned, destroying, jumped, landed,
// footstep, impact; and pressed:<action> ("pressed:fire"), the local player's press, before the
// server has answered. Conditions may also read event.value, event.strength (impacts) and
// is_local (the entity whose board is read is the local player).
//
// When:    on an event, or while board conditions hold. Conditions read the subject's board, or
//          another entity's with a path ("holder:combat.dead", "event.b:combat.health < 20").
// Subject: an entity path (default: self). An event must name the subject on its event_side: A
//          (who it is about: "melee.hit" is at the attacker), B (the other one: the victim), or
//          either. A subject that starts with event.a or event.b matches every event of the name.
// Act on:  an entity path whose scene the node paths below are resolved in, from its root node;
//          empty: this reaction's own scene, relative to the reaction.
// Do:      any of: play an animation on an AnimationPlayer (and another when a "while" ends); set a
//          property on a node (put back when a "while" ends); call a built-in method with no
//          arguments ("restart", "play"); add a scene as a child (removed when a "while" ends, or
//          after scene_lifetime seconds for an event); play a sound; shake the camera or flash the
//          screen (the viewer's: pair them with subject "local" or the condition is_local).
// Place:   where a scene or sound goes: under its parent (default), at the event's point or end,
//          stretched from point to end (a tracer), at a joint of the subject, or following it.
//
// Presentation only: nothing here changes the simulation, so every screen can differ in looks but
// never in what happened. Anything that exists in the game (a prop, an item) is spawned by the
// server's mods. In the game, node paths stay inside the entity scene they are resolved in (a
// player's scene holds its items), and "free", "queue_free" and "script" are refused.

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/transform3d.hpp>
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
		PLACE_BONE = 4,
		PLACE_FOLLOW = 5,
	};
	// Where the game puts this firing's scene and sound (null: under the parent, as authored).
	struct Placement
	{
		bool global = false; // `transform` is where, in the world
		godot::Transform3D transform;
		godot::Node* parent = nullptr;
	};
	// An event arrived (the game checked the name, the subject and the conditions).
	void fire();
	// A "while" reaction's conditions: acts when this changes.
	void set_on( bool on );
	// The same for the game: node paths resolve from `root` (null: from this reaction), and only
	// nodes inside `limit` are touched (null: anywhere).
	void FireIn( godot::Node* root, godot::Node* limit );
	void FireAt( godot::Node* root, godot::Node* limit, const Placement* place );
	void SetOnIn( bool on, godot::Node* root, godot::Node* limit );
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
	void set_subject( const godot::String& v )
	{
		m_subject = v;
	}
	godot::String get_subject() const
	{
		return m_subject;
	}
	void set_event_side( int v )
	{
		m_eventSide = v;
	}
	int get_event_side() const
	{
		return m_eventSide;
	}
	void set_act_on( const godot::String& v )
	{
		m_actOn = v;
	}
	godot::String get_act_on() const
	{
		return m_actOn;
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

#define CB_REACTION_FIELD( Type, name, member )                                                                                  \
	void set_##name( const Type& v )                                                                                             \
	{                                                                                                                            \
		member = v;                                                                                                              \
	}                                                                                                                            \
	Type get_##name() const                                                                                                      \
	{                                                                                                                            \
		return member;                                                                                                           \
	}
	CB_REACTION_FIELD( godot::String, subject_kind, m_subjectKind )
	CB_REACTION_FIELD( godot::String, subject_template, m_subjectTemplate )
	CB_REACTION_FIELD( double, cooldown, m_cooldown )
	CB_REACTION_FIELD( int, place, m_place )
	CB_REACTION_FIELD( godot::String, bone, m_bone )
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

	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	void Act( bool on, godot::Node* root, godot::Node* limit, const Placement* place );
	void PlaySound( godot::Node* parent, const Placement* place );
	godot::Node* Find( const godot::NodePath& path, godot::Node* root, godot::Node* limit ) const;
	static bool Refused( const godot::String& method, const godot::String& property );

	int m_when = WHEN_EVENT;
	godot::String m_event;
	godot::PackedStringArray m_conditions;
	godot::String m_subject;
	int m_eventSide = SIDE_A;
	godot::String m_actOn;
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
	godot::String m_subjectKind;
	godot::String m_subjectTemplate;
	double m_cooldown = 0.0;
	int m_place = PLACE_PARENT;
	godot::String m_bone;
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

	bool m_on = false;
	bool m_haveOriginal = false;
	godot::Variant m_original; // what the property was before a "while" set it
	godot::ObjectID m_onTarget; // where a "while" set it
	godot::ObjectID m_onPlayer; // where a "while" played its animation
	godot::ObjectID m_spawned;
};

// How a kind of held item looks, as a node in a world reactions scene: the scene drawn in its
// holder's socket ("melee.bat" -> res://prefabs/bat.tscn). The scene is the item's frame (the
// socket's): the grip at the origin, pointing along -Z.
class CbItemLook : public godot::Node
{
	GDCLASS( CbItemLook, godot::Node )

public:
	CB_REACTION_FIELD( godot::String, kind, m_kind )
	CB_REACTION_FIELD( godot::String, scene, m_scene )

protected:
	static void _bind_methods();

private:
	godot::String m_kind;
	godot::String m_scene;
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbReaction::When );
VARIANT_ENUM_CAST( cb::gd::CbReaction::Place );
VARIANT_ENUM_CAST( cb::gd::CbReaction::EventSide );
