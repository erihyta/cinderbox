#include "cinderbox_text.h"

#include "cinderbox_client.h"
#include "cinderbox_hud.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_map.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <vector>

using namespace godot;

namespace cb::gd
{

namespace
{

bool Playing()
{
	return Engine::get_singleton()->is_editor_hint() == false;
}

} // namespace

// --- CbTextEntry ----------------------------------------------------------------------------------

void CbTextEntry::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_channel", "value" ), &CbTextEntry::set_channel );
	ClassDB::bind_method( D_METHOD( "get_channel" ), &CbTextEntry::get_channel );
	ClassDB::bind_method( D_METHOD( "set_action", "value" ), &CbTextEntry::set_action );
	ClassDB::bind_method( D_METHOD( "get_action" ), &CbTextEntry::get_action );
	ClassDB::bind_method( D_METHOD( "set_key", "value" ), &CbTextEntry::set_key );
	ClassDB::bind_method( D_METHOD( "get_key" ), &CbTextEntry::get_key );
	ClassDB::bind_method( D_METHOD( "is_open" ), &CbTextEntry::is_open );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "channel", PROPERTY_HINT_PLACEHOLDER_TEXT, "chat" ), "set_channel", "get_channel" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "action", PROPERTY_HINT_PLACEHOLDER_TEXT, "chat" ), "set_action", "get_action" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "key", PROPERTY_HINT_PLACEHOLDER_TEXT, "T" ), "set_key", "get_key" );
}

void CbTextEntry::_ready()
{
	if ( Playing() == false )
	{
		return;
	}
	set_max_length( 200 );
	hide();
	connect( "text_submitted", callable_mp( this, &CbTextEntry::Submitted ) );
	connect( "focus_exited", callable_mp( this, &CbTextEntry::Close ) );
	set_process( true );
}

void CbTextEntry::_process( double )
{
	CinderboxClient* client = FindClient( this, m_client );
	if ( client == nullptr || m_action.strip_edges().is_empty() )
	{
		return;
	}
	// Its key is an input action of the game's (cb_<action>), as a CbKey's: the Keys page rebinds it.
	StringName action = "cb_" + m_action.strip_edges();
	InputMap* map = InputMap::get_singleton();
	if ( map->has_action( action ) == false )
	{
		Key key = OS::get_singleton()->find_keycode_from_string( m_key.strip_edges() );
		if ( key == KEY_NONE )
		{
			return;
		}
		Ref<InputEventKey> event;
		event.instantiate();
		event->set_physical_keycode( key );
		map->add_action( action );
		map->action_add_event( action, event );
	}
	// Only from the game itself: not from a menu, nor from a screen that has the cursor.
	Input* input = Input::get_singleton();
	if ( m_open == false && input->is_action_just_pressed( action ) && input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED &&
		 client->wants_cursor() == false )
	{
		Open();
	}
}

void CbTextEntry::Open()
{
	CinderboxClient* client = FindClient( this, m_client );
	if ( client == nullptr )
	{
		return;
	}
	m_open = true;
	clear();
	show();
	grab_focus();
	client->want_typing( int64_t( get_instance_id() ), true );
	client->want_cursor( int64_t( get_instance_id() ), true );
}

void CbTextEntry::Close()
{
	if ( m_open == false )
	{
		return;
	}
	m_open = false;
	clear();
	hide();
	if ( has_focus() )
	{
		release_focus();
	}
	if ( CinderboxClient* client = FindClient( this, m_client ) )
	{
		client->want_typing( int64_t( get_instance_id() ), false );
		client->want_cursor( int64_t( get_instance_id() ), false );
	}
}

void CbTextEntry::Submitted( const String& text )
{
	CinderboxClient* client = FindClient( this, m_client );
	if ( client != nullptr && text.strip_edges().is_empty() == false )
	{
		client->send_text( m_channel, text );
	}
	Close();
}

void CbTextEntry::_gui_input( const Ref<InputEvent>& event )
{
	Ref<InputEventKey> key = event;
	if ( m_open && key.is_valid() && key->is_pressed() && key->get_keycode() == KEY_ESCAPE )
	{
		accept_event(); // (not the game's menu)
		Close();
	}
}

void CbTextEntry::_exit_tree()
{
	Close();
}

// --- CbTextLog ------------------------------------------------------------------------------------

void CbTextLog::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_channel", "value" ), &CbTextLog::set_channel );
	ClassDB::bind_method( D_METHOD( "get_channel" ), &CbTextLog::get_channel );
	ClassDB::bind_method( D_METHOD( "set_lines", "value" ), &CbTextLog::set_lines );
	ClassDB::bind_method( D_METHOD( "get_lines" ), &CbTextLog::get_lines );
	ClassDB::bind_method( D_METHOD( "set_seconds", "value" ), &CbTextLog::set_seconds );
	ClassDB::bind_method( D_METHOD( "get_seconds" ), &CbTextLog::get_seconds );
	ClassDB::bind_method( D_METHOD( "set_line_format", "value" ), &CbTextLog::set_line_format );
	ClassDB::bind_method( D_METHOD( "get_line_format" ), &CbTextLog::get_line_format );
	ClassDB::bind_method( D_METHOD( "set_server_format", "value" ), &CbTextLog::set_server_format );
	ClassDB::bind_method( D_METHOD( "get_server_format" ), &CbTextLog::get_server_format );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "channel", PROPERTY_HINT_PLACEHOLDER_TEXT, "chat" ), "set_channel", "get_channel" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "lines", PROPERTY_HINT_RANGE, "1,100" ), "set_lines", "get_lines" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "seconds", PROPERTY_HINT_RANGE, "0,600,0.5,suffix:s" ), "set_seconds", "get_seconds" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "line_format", PROPERTY_HINT_PLACEHOLDER_TEXT, "{from}: {text}" ), "set_line_format",
				  "get_line_format" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "server_format", PROPERTY_HINT_PLACEHOLDER_TEXT, "{text}" ), "set_server_format",
				  "get_server_format" );
}

void CbTextLog::_ready()
{
	if ( Playing() )
	{
		set_text( "" );
		set_process( true );
	}
}

void CbTextLog::_process( double )
{
	CinderboxClient* client = FindClient( this, m_client );
	if ( client == nullptr )
	{
		return;
	}
	double now = double( Time::get_singleton()->get_ticks_usec() ) / 1e6;
	bool all = client->is_typing();
	String channel = m_channel.strip_edges();
	std::vector<String> shown;
	for ( const CinderboxClient::ShownText& t : client->Texts() )
	{
		if ( t.channel == channel && ( all || m_seconds <= 0.0 || now - t.at < m_seconds ) )
		{
			Dictionary words;
			words["from"] = t.from;
			words["text"] = t.text;
			shown.push_back( ( t.from.is_empty() ? m_serverFormat : m_lineFormat ).format( words ) );
		}
	}
	String text;
	size_t first = shown.size() > size_t( std::max( m_lines, 1 ) ) ? shown.size() - size_t( std::max( m_lines, 1 ) ) : 0;
	for ( size_t i = first; i < shown.size(); ++i )
	{
		text += ( i > first ? "\n" : "" ) + shown[i];
	}
	if ( text != get_text() )
	{
		set_text( text );
	}
}

} // namespace cb::gd
