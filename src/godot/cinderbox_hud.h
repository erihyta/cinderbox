#pragma once

// HUD nodes that read the server mods' board, so a HUD is a scene and never a script.
//
// Everything here is written against names the server's mods declared ("combat.health",
// "combat.killed"), never against what a pistol is, and carries no code: a client mod or a mod's
// workshop item can build a whole HUD out of ordinary Godot controls and these.
//
//   CbFieldLabel    a Label: "AMMO {pistol.ammo}", shown while its conditions hold
//   CbFieldBinding  writes a field into any property of any node (a bar's value, a panel's
//                   visibility, a colour's alpha), so any 2D asset can show mod state
//   CbEventFeed     a line per mod event ("{a} > {b}" for combat.killed), fading after a while
//   CbScoreboard    a table of players: name and field columns, sorted, shown while a key is held

#include <godot_cpp/classes/grid_container.hpp>
#include <godot_cpp/classes/label.hpp>
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

class CbFieldLabel : public godot::Label
{
	GDCLASS( CbFieldLabel, godot::Label )

public:
	void _ready() override;
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

protected:
	static void _bind_methods();

private:
	// "{name}" is replaced by the field's value; empty leaves the label's own text alone.
	godot::String m_format;
	godot::PackedStringArray m_conditions;
	godot::ObjectID m_client;
};

// field -> target.property, every frame, for the local player (or the global board, if the field
// is global): value = field * multiply + add. Bool properties get "not zero". With conditions, the
// target is hidden while they do not hold (when it has a "visible" property).
class CbFieldBinding : public godot::Node
{
	GDCLASS( CbFieldBinding, godot::Node )

public:
	void _ready() override;
	void _process( double delta ) override;

	void set_field( const godot::String& v )
	{
		m_field = v;
	}
	godot::String get_field() const
	{
		return m_field;
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
	void set_multiply( float v )
	{
		m_multiply = v;
	}
	float get_multiply() const
	{
		return m_multiply;
	}
	void set_add( float v )
	{
		m_add = v;
	}
	float get_add() const
	{
		return m_add;
	}
	void set_conditions( const godot::PackedStringArray& v )
	{
		m_conditions = v;
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
	float m_multiply = 1.0f;
	float m_add = 0.0f;
	godot::PackedStringArray m_conditions;
	godot::ObjectID m_client;
};

class CbEventFeed : public godot::VBoxContainer
{
	GDCLASS( CbEventFeed, godot::VBoxContainer )

public:
	void _ready() override;
	void _process( double delta ) override;

	void set_event( const godot::String& v )
	{
		m_event = v;
	}
	godot::String get_event() const
	{
		return m_event;
	}
	void set_text_format( const godot::String& v )
	{
		m_format = v;
	}
	godot::String get_text_format() const
	{
		return m_format;
	}
	void set_nobody_text( const godot::String& v )
	{
		m_nobody = v;
	}
	godot::String get_nobody_text() const
	{
		return m_nobody;
	}
	void set_max_lines( int v )
	{
		m_maxLines = v;
	}
	int get_max_lines() const
	{
		return m_maxLines;
	}
	void set_line_seconds( float v )
	{
		m_lineSeconds = v;
	}
	float get_line_seconds() const
	{
		return m_lineSeconds;
	}
	void set_label_settings( const godot::Ref<godot::LabelSettings>& v )
	{
		m_labelSettings = v;
	}
	godot::Ref<godot::LabelSettings> get_label_settings() const
	{
		return m_labelSettings;
	}

	// Connected to the client's mod_event signal.
	void on_mod_event( const godot::String& name, int64_t a, int64_t b, int64_t value, const godot::Vector3& position,
					   const godot::Vector3& vector );

protected:
	static void _bind_methods();

private:
	godot::String m_event = "combat.killed";
	// {a} and {b}: the two entities' player names ({nobody} when there is none), {value}: the value.
	godot::String m_format = "{a}  >  {b}";
	godot::String m_nobody = "the world";
	int m_maxLines = 5;
	float m_lineSeconds = 5.0f;
	godot::Ref<godot::LabelSettings> m_labelSettings;
	godot::ObjectID m_client;
	bool m_connected = false;
	std::vector<double> m_expires; // one per line, oldest first
};

class CbScoreboard : public godot::GridContainer
{
	GDCLASS( CbScoreboard, godot::GridContainer )

public:
	void _ready() override;
	void _process( double delta ) override;

	void set_headers( const godot::PackedStringArray& v )
	{
		m_headers = v;
	}
	godot::PackedStringArray get_headers() const
	{
		return m_headers;
	}
	void set_cells( const godot::PackedStringArray& v )
	{
		m_cells = v;
	}
	godot::PackedStringArray get_cells() const
	{
		return m_cells;
	}
	void set_sort_field( const godot::String& v )
	{
		m_sortField = v;
	}
	godot::String get_sort_field() const
	{
		return m_sortField;
	}
	void set_show_action( const godot::String& v )
	{
		m_showAction = v;
	}
	godot::String get_show_action() const
	{
		return m_showAction;
	}
	void set_label_settings( const godot::Ref<godot::LabelSettings>& v )
	{
		m_labelSettings = v;
	}
	godot::Ref<godot::LabelSettings> get_label_settings() const
	{
		return m_labelSettings;
	}
	void set_conditions( const godot::PackedStringArray& v )
	{
		m_conditions = v;
	}
	godot::PackedStringArray get_conditions() const
	{
		return m_conditions;
	}
	void set_local_settings( const godot::Ref<godot::LabelSettings>& v )
	{
		m_localSettings = v;
	}
	godot::Ref<godot::LabelSettings> get_local_settings() const
	{
		return m_localSettings;
	}

protected:
	static void _bind_methods();

private:
	godot::Label* Cell( int index );

	godot::PackedStringArray m_headers;
	// One format per column, filled per player: "{name}", "{combat.kills}".
	godot::PackedStringArray m_cells;
	// Highest first; empty keeps join order.
	godot::String m_sortField;
	// Shown while this input action is held; empty shows it always.
	godot::String m_showAction = "scoreboard";
	godot::Ref<godot::LabelSettings> m_labelSettings;
	godot::Ref<godot::LabelSettings> m_localSettings; // the local player's row
	// Local-player conditions for showing it at all (e.g. "!?deathmatch.score": step aside when a
	// game mode brings its own scoreboard).
	godot::PackedStringArray m_conditions;
	godot::ObjectID m_client;
};

} // namespace cb::gd
