#pragma once

// HUD nodes that read the server mods' board, so a HUD is a scene and never a script.
//
// Everything here is written against names the server's mods declared ("combat.health",
// "combat.killed"), never against what a pistol is, and carries no code: a client mod or a mod's
// workshop item can build a whole HUD out of ordinary Godot controls and these.
//
//   CbFieldLabel    a Label: "AMMO {pistol.ammo}", shown while its conditions hold; or one of the
//                   texts written on it (choices), picked by a number the server sets
//   CbFieldBinding  writes a field into any property of any node (a bar's value, a panel's
//                   visibility, a colour's alpha, a part of one: "scale:x"), or formatted
//                   words into its text, so any asset can show mod state
//   CbList          a row per player, item, slot or event that happened: its first child is the
//                   row, copied for each, sorted and filtered; the HUD nodes in a row read that
//                   row's entity (a scoreboard, a team list, the items a player carries, a kill
//                   feed)
//   CbKey           a key of the viewer's own (Tab): a value of the viewer's ("ui.scores") is 1
//                   while it is held, or switched by each press; whoever reads the value shows
//                   something. It can free the cursor meanwhile (an inventory screen)
//   CbClick         what a click on its parent does: sets values of the viewer's own ("ui."
//                   names), asks something of the player's slots (select, move, drop)
//   CbPromptLabel   a label in the world, upright above its parent: "[{key:pickup}] Pick up
//                   {look:pickup.target}" (a proximity prompt, placed by a CbReaction)
//
// Whose fields a node reads is its subject: the entity of the CbList row it is in, or the local
// player.
//
// Formats: {field} is the subject's value, {name} what it is called, {rank} its place in its list, {name:field} the name of the
// player the field points at, {look:field} what the entity the field points at is called (an
// item's display name), {key:action} the key the action is bound to now.

#include <godot_cpp/classes/box_container.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/label_settings.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <vector>

namespace cb::gd
{

class CinderboxClient;

// The client these nodes read from: the scene tree's CinderboxClient (group "cinderbox_client").
CinderboxClient* FindClient( godot::Node* from, godot::ObjectID& cache );
// Whose fields a HUD node reads: the entity of the CbList row it is in (the nearest one), or the
// local player. `rank`, when given: its place in that list, from 1 (0 outside a list).
int64_t SubjectOf( const godot::Node* node, const CinderboxClient* client, int* rank = nullptr );
// Names of the row a HUD node is in, for its conditions and formats (a slot row's "slot.number",
// "slot.selected", "slot.empty"): name -> number. Empty outside such a row.
godot::Dictionary RowNames( const godot::Node* node );

class CbPromptLabel : public godot::Label3D
{
	GDCLASS( CbPromptLabel, godot::Label3D )

public:
	CbPromptLabel();
	void _ready() override;
	// What it names that nobody declares (cue_names.h).
	godot::PackedStringArray _get_configuration_warnings() const override;
	void _process( double delta ) override;

	void set_text_format( const godot::String& value )
	{
		m_format = value;
	}
	godot::String get_text_format() const
	{
		return m_format;
	}
	void set_height( double value )
	{
		m_height = value;
	}
	double get_height() const
	{
		return m_height;
	}
	// A field that runs from 0 to 1 ("pickup.progress"): while it is above 0, a bar under the text
	// fills with it (hold to use).
	void set_progress_field( const godot::String& value )
	{
		m_progressField = value;
	}
	godot::String get_progress_field() const
	{
		return m_progressField;
	}
	// Or two fields that change only when a hold starts and ends: the tick it began ("pickup.since",
	// 0: none) and how many seconds it takes ("pickup.hold"). The bar fills from the game's clock.
	void set_since_field( const godot::String& value )
	{
		m_sinceField = value;
	}
	godot::String get_since_field() const
	{
		return m_sinceField;
	}
	void set_duration_field( const godot::String& value )
	{
		m_durationField = value;
	}
	godot::String get_duration_field() const
	{
		return m_durationField;
	}
	void set_bar_color( const godot::Color& value )
	{
		m_barColor = value;
	}
	godot::Color get_bar_color() const
	{
		return m_barColor;
	}

protected:
	static void _bind_methods();

private:
	godot::String m_format = "[{key:pickup}]  Pick up {look:pickup.target}";
	double m_height = 0.35;
	godot::String m_progressField;
	godot::String m_sinceField;
	godot::String m_durationField;
	godot::Color m_barColor = godot::Color( 1, 1, 1, 1 );
	godot::MeshInstance3D* m_barBack = nullptr;
	godot::MeshInstance3D* m_barFill = nullptr;
	godot::Ref<godot::QuadMesh> m_fillMesh;
	void ShowBar( float progress );
	godot::ObjectID m_client;
};

class CbFieldLabel : public godot::Label
{
	GDCLASS( CbFieldLabel, godot::Label )

public:
	void _ready() override;
	// What it names that nobody declares (cue_names.h).
	godot::PackedStringArray _get_configuration_warnings() const override;
	void _process( double delta ) override;

	void set_text_format( const godot::String& value )
	{
		m_format = value;
	}
	godot::String get_text_format() const
	{
		return m_format;
	}
	void set_conditions( const godot::PackedStringArray& value )
	{
		m_conditions = value;
	}
	godot::PackedStringArray get_conditions() const
	{
		return m_conditions;
	}
	void set_choice_field( const godot::String& value )
	{
		m_choiceField = value;
	}
	godot::String get_choice_field() const
	{
		return m_choiceField;
	}
	void set_choices( const godot::PackedStringArray& value )
	{
		m_choices = value;
	}
	godot::PackedStringArray get_choices() const
	{
		return m_choices;
	}

protected:
	static void _bind_methods();

private:
	// "{name}" is replaced by the field's value; empty leaves the label's own text alone.
	godot::String m_format;
	godot::PackedStringArray m_conditions;
	// The words are the look's: the server says a number ("deathmatch.ending" = 2, or any
	// expression), the label shows that line of `choices` (0 is the first). A line is a format
	// like any other. An empty line, or a number past the last, hides the label: leave line 0
	// empty for "nothing to say". "{choice}" in text_format is the line, when there is more
	// around it.
	godot::String m_choiceField;
	godot::PackedStringArray m_choices;
	godot::ObjectID m_client;
};

// field -> target.property, every frame, for its subject (or the global board, if the field is
// global): value = field * multiply + add. Bool properties get "not zero". With conditions, the
// target is hidden while they do not hold (when it has a "visible" property).
//
// The property may be a part of one ("scale:x", "modulate:a", "position:y"). With a text_format
// the property gets words instead of a number ("[{key:pickup}]  Pick up {look:pickup.target}"
// into a Label3D's text), so anything that shows text can show the game's.
class CbFieldBinding : public godot::Node
{
	GDCLASS( CbFieldBinding, godot::Node )

public:
	void _ready() override;
	// What it names that nobody declares (cue_names.h).
	godot::PackedStringArray _get_configuration_warnings() const override;
	void _process( double delta ) override;

	void set_field( const godot::String& v )
	{
		m_field = v;
		update_configuration_warnings();
	}
	godot::String get_field() const
	{
		return m_field;
	}
	void set_target( const godot::NodePath& v )
	{
		m_target = v;
		update_configuration_warnings();
	}
	godot::NodePath get_target() const
	{
		return m_target;
	}
	void set_property( const godot::String& v )
	{
		m_property = v;
		update_configuration_warnings();
	}
	godot::String get_property() const
	{
		return m_property;
	}
	void set_text_format( const godot::String& v )
	{
		m_textFormat = v;
		update_configuration_warnings();
	}
	godot::String get_text_format() const
	{
		return m_textFormat;
	}
	void set_multiply( float v )
	{
		m_multiply = v;
		update_configuration_warnings();
	}
	float get_multiply() const
	{
		return m_multiply;
	}
	void set_add( float v )
	{
		m_add = v;
		update_configuration_warnings();
	}
	float get_add() const
	{
		return m_add;
	}
	void set_conditions( const godot::PackedStringArray& v )
	{
		m_conditions = v;
		update_configuration_warnings();
	}
	godot::PackedStringArray get_conditions() const
	{
		return m_conditions;
	}

protected:
	static void _bind_methods();

private:
	godot::String m_field;
	godot::NodePath m_target = godot::NodePath( ".." );
	godot::String m_property = "value";
	godot::String m_textFormat;
	float m_multiply = 1.0f;
	float m_add = 0.0f;
	godot::PackedStringArray m_conditions;
	godot::ObjectID m_client;
};

// A row per entity. Its first child (any Control) is the row as the modder designed it; in the game
// that one is hidden and a copy of it is shown for each entry, in order. The HUD nodes inside a row
// read that row's entity.
//
// Or a row per mod event that happened (a kill feed): the newest last, each for `seconds`. A row is
// about the event's first entity, and its names are {a} and {b} (what the two are called),
// {event.value}, and event.a / event.b (their ids, for conditions).
class CbList : public godot::BoxContainer
{
	GDCLASS( CbList, godot::BoxContainer )

public:
	enum Of
	{
		OF_PLAYERS = 0,
		OF_ITEMS = 1,
		OF_HELD_ITEMS = 2,
		OF_SLOTS = 3,
		OF_EVENTS = 4,
	};

	CbList();
	void _ready() override;
	void _process( double delta ) override;
	godot::PackedStringArray _get_configuration_warnings() const override;
	void _validate_property( godot::PropertyInfo& property ) const;

	void set_of( int v )
	{
		m_of = v;
		update_configuration_warnings();
	}
	int get_of() const
	{
		return m_of;
	}
	void set_item_kind( const godot::String& v )
	{
		m_itemKind = v;
		update_configuration_warnings();
	}
	godot::String get_item_kind() const
	{
		return m_itemKind;
	}
	void set_where( const godot::PackedStringArray& v )
	{
		m_where = v;
		update_configuration_warnings();
	}
	godot::PackedStringArray get_where() const
	{
		return m_where;
	}
	void set_sort_by( const godot::String& v )
	{
		m_sortBy = v;
		update_configuration_warnings();
	}
	godot::String get_sort_by() const
	{
		return m_sortBy;
	}
	void set_descending( bool v )
	{
		m_descending = v;
		update_configuration_warnings();
	}
	bool get_descending() const
	{
		return m_descending;
	}
	void set_max_rows( int v )
	{
		m_maxRows = v;
		update_configuration_warnings();
	}
	int get_max_rows() const
	{
		return m_maxRows;
	}
	void set_conditions( const godot::PackedStringArray& v )
	{
		m_conditions = v;
		update_configuration_warnings();
	}
	godot::PackedStringArray get_conditions() const
	{
		return m_conditions;
	}

	void set_event( const godot::String& v )
	{
		m_event = v;
		update_configuration_warnings();
	}
	godot::String get_event() const
	{
		return m_event;
	}
	void set_seconds( float v )
	{
		m_seconds = v;
		update_configuration_warnings();
	}
	float get_seconds() const
	{
		return m_seconds;
	}
	void set_nobody_text( const godot::String& v )
	{
		m_nobody = v;
		update_configuration_warnings();
	}
	godot::String get_nobody_text() const
	{
		return m_nobody;
	}
	void on_mod_event( const godot::String& name, int64_t a, int64_t b, int64_t value, const godot::Vector3& position,
					   const godot::Vector3& vector );

	// The entities it lists now, in order (for tools and checks).
	godot::PackedInt64Array get_entries() const;

protected:
	static void _bind_methods();

private:
	godot::Control* Template() const;

	int m_of = OF_PLAYERS;
	// OF_ITEMS, OF_HELD_ITEMS: only this kind ("pistol.gun"); empty: any.
	godot::String m_itemKind;
	// Conditions on an entry: only those that hold are listed ("team.id == 1").
	godot::PackedStringArray m_where;
	// An expression over an entry's fields ("deathmatch.score"); empty keeps the world's order.
	godot::String m_sortBy;
	bool m_descending = true;
	int m_maxRows = 0; // 0: all
	// Conditions on the list's own subject for showing it at all.
	godot::PackedStringArray m_conditions;
	// OF_EVENTS: the mod event it lists ("combat.killed"), how long a row stays, and what an event's
	// entity is called when there is none (a fall: nobody did it).
	godot::String m_event;
	float m_seconds = 5.0f;
	godot::String m_nobody = "the world";
	struct Happened
	{
		int64_t a = 0;
		int64_t b = 0;
		int64_t value = 0;
		godot::String aName;
		godot::String bName;
		double expires = 0.0;
	};
	std::vector<Happened> m_happened; // oldest first
	bool m_connected = false;
	std::vector<godot::ObjectID> m_rows;
	std::vector<int64_t> m_entries;
	godot::ObjectID m_client;
};

// A key of the viewer's own: while it is held, or switched by each press, a value of the viewer's is
// 1 ("ui.scores"), and 0 otherwise. What that shows is up to whoever reads the value: a
// CbFieldBinding with the condition "ui.scores" shows its target, a label says something else, a
// CbClick asks for it. The key is an input action of its own (not one of the server's: the
// simulation never hears of it), added to the game's with a default key when no one has it yet,
// so "{key:scores}" names it and rebinding moves it.
class CbKey : public godot::Node
{
	GDCLASS( CbKey, godot::Node )

public:
	enum Mode
	{
		MODE_HOLD = 0,
		MODE_TOGGLE = 1,
	};

	void _ready() override;
	void _process( double delta ) override;
	godot::PackedStringArray _get_configuration_warnings() const override;

	void set_action( const godot::String& v )
	{
		m_action = v;
		update_configuration_warnings();
	}
	godot::String get_action() const
	{
		return m_action;
	}
	void set_key( const godot::String& v )
	{
		m_key = v;
		update_configuration_warnings();
	}
	godot::String get_key() const
	{
		return m_key;
	}
	void set_mode( int v )
	{
		m_mode = v;
		update_configuration_warnings();
	}
	int get_mode() const
	{
		return m_mode;
	}
	void set_value( const godot::String& v )
	{
		m_value = v;
		update_configuration_warnings();
	}
	godot::String get_value() const
	{
		return m_value;
	}
	void set_cursor( bool v )
	{
		m_cursor = v;
		update_configuration_warnings();
	}
	bool get_cursor() const
	{
		return m_cursor;
	}
	void _exit_tree() override;
	void set_conditions( const godot::PackedStringArray& v )
	{
		m_conditions = v;
		update_configuration_warnings();
	}
	godot::PackedStringArray get_conditions() const
	{
		return m_conditions;
	}

protected:
	static void _bind_methods();

private:
	// Conditions on its subject that must hold too, or the value is 0 ("!combat.dead").
	godot::PackedStringArray m_conditions;
	godot::ObjectID m_client;
	godot::String m_action = "scores";
	godot::String m_key = "Tab"; // a key's name, as Godot writes it: "Tab", "M", "F1"
	int m_mode = MODE_HOLD;
	// The viewer's value it keeps ("ui.bag"); empty: "ui." and the action's name.
	godot::String m_value;
	godot::String ValueName() const;
	bool m_on = false;
	int m_said = -1; // what the value was last set to
	// While the value is 1 the cursor is free, and the mouse is the screen's, not the game's.
	bool m_cursor = false;
};

// What a click on its parent (any Control) does. No scripts and no wired signals: the node listens
// itself. Several under one Control are asked in order, and the first whose conditions hold acts.
//
//   sets     values of the viewer's own: "ui.picked = slot.number", "ui.tab = 2", "ui.picked = 0".
//            The right side is an expression, read as the node's conditions are.
//   intent   what it asks of the player's slots, through the player's input, so it is predicted:
//            select this row's slot, move the slot `from` names onto this row's slot, or drop it.
//
// A click-to-move inventory is two of them in a slot's row: one that picks the slot up (while
// nothing is picked and the slot is not empty), one that puts the picked slot down here.
class CbClick : public godot::Node
{
	GDCLASS( CbClick, godot::Node )

public:
	enum Intent
	{
		INTENT_NONE = 0,
		INTENT_SELECT = 1,
		INTENT_MOVE_HERE = 2,
		INTENT_DROP = 3,
	};

	void _ready() override;
	godot::PackedStringArray _get_configuration_warnings() const override;
	void on_gui_input( const godot::Ref<godot::InputEvent>& event );

	void set_conditions( const godot::PackedStringArray& v )
	{
		m_conditions = v;
		update_configuration_warnings();
	}
	godot::PackedStringArray get_conditions() const
	{
		return m_conditions;
	}
	void set_sets( const godot::PackedStringArray& v )
	{
		m_sets = v;
		update_configuration_warnings();
	}
	godot::PackedStringArray get_sets() const
	{
		return m_sets;
	}
	void set_intent( int v )
	{
		m_intent = v;
		update_configuration_warnings();
	}
	int get_intent() const
	{
		return m_intent;
	}
	void set_from( const godot::String& v )
	{
		m_from = v;
		update_configuration_warnings();
	}
	godot::String get_from() const
	{
		return m_from;
	}

protected:
	static void _bind_methods();

private:
	godot::PackedStringArray m_conditions;
	godot::PackedStringArray m_sets;
	int m_intent = INTENT_NONE;
	// INTENT_MOVE_HERE: the slot that moves, as a number from 1 ("ui.picked").
	godot::String m_from = "ui.picked";
	godot::ObjectID m_client;
};

} // namespace cb::gd

VARIANT_ENUM_CAST( cb::gd::CbClick::Intent );
VARIANT_ENUM_CAST( cb::gd::CbList::Of );
VARIANT_ENUM_CAST( cb::gd::CbKey::Mode );
