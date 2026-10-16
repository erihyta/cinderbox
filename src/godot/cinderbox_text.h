#pragma once

// Typed text in a HUD, with no code: a box to type into and a log of what was said, on a channel a
// server mod declared ("chat"). The engine only carries the words; who hears what is the mod's
// (mod_api.h: Context::Typed, Context::Say), so a server without such a mod has no box: the HUD
// that holds these comes with the mod's look.
//
//   CbTextEntry   a LineEdit, hidden until its key (T) is pressed in the game: Enter sends what
//                 was typed to the server, Esc (or a click elsewhere) closes it. While it is open
//                 the game keeps the keys from the player and frees the cursor.
//   CbTextLog     a Label: the channel's last lines, "{from}: {text}", each for a while (all of
//                 them while a box is open).

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line_edit.hpp>

namespace cb::gd
{

class CbTextEntry : public godot::LineEdit
{
	GDCLASS( CbTextEntry, godot::LineEdit )

public:
	void _ready() override;
	void _process( double delta ) override;
	void _gui_input( const godot::Ref<godot::InputEvent>& event ) override;
	void _exit_tree() override;

	void set_channel( const godot::String& v )
	{
		m_channel = v;
	}
	godot::String get_channel() const
	{
		return m_channel;
	}
	void set_action( const godot::String& v )
	{
		m_action = v;
	}
	godot::String get_action() const
	{
		return m_action;
	}
	void set_key( const godot::String& v )
	{
		m_key = v;
	}
	godot::String get_key() const
	{
		return m_key;
	}
	bool is_open() const
	{
		return m_open;
	}

protected:
	static void _bind_methods();

private:
	void Open();
	void Close();
	void Submitted( const godot::String& text );

	godot::String m_channel = "chat";
	godot::String m_action = "chat";
	godot::String m_key = "T";
	bool m_open = false;
	godot::ObjectID m_client;
};

class CbTextLog : public godot::Label
{
	GDCLASS( CbTextLog, godot::Label )

public:
	void _ready() override;
	void _process( double delta ) override;

	void set_channel( const godot::String& v )
	{
		m_channel = v;
	}
	godot::String get_channel() const
	{
		return m_channel;
	}
	void set_lines( int v )
	{
		m_lines = v;
	}
	int get_lines() const
	{
		return m_lines;
	}
	void set_seconds( double v )
	{
		m_seconds = v;
	}
	double get_seconds() const
	{
		return m_seconds;
	}
	void set_line_format( const godot::String& v )
	{
		m_lineFormat = v;
	}
	godot::String get_line_format() const
	{
		return m_lineFormat;
	}
	void set_server_format( const godot::String& v )
	{
		m_serverFormat = v;
	}
	godot::String get_server_format() const
	{
		return m_serverFormat;
	}

protected:
	static void _bind_methods();

private:
	godot::String m_channel = "chat";
	int m_lines = 8;
	double m_seconds = 10.0;
	godot::String m_lineFormat = "{from}: {text}";
	godot::String m_serverFormat = "{text}";
	godot::ObjectID m_client;
};

} // namespace cb::gd
