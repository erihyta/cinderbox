#include "cinderbox_hud.h"

#include "cinderbox_client.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_map.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>

using namespace godot;

namespace cb::gd
{

namespace
{

bool InGame()
{
	return Engine::get_singleton()->is_editor_hint() == false;
}

double Now()
{
	return double( Time::get_singleton()->get_ticks_msec() ) / 1000.0;
}

} // namespace

CinderboxClient* FindClient( Node* from, ObjectID& cache )
{
	if ( auto* client = Object::cast_to<CinderboxClient>( ObjectDB::get_instance( cache ) ) )
	{
		return client;
	}
	if ( from->is_inside_tree() == false )
	{
		return nullptr;
	}
	auto* client = Object::cast_to<CinderboxClient>( from->get_tree()->get_first_node_in_group( "cinderbox_client" ) );
	cache = client != nullptr ? client->get_instance_id() : ObjectID();
	return client;
}

// --- CbFieldLabel ----------------------------------------------------------------------------------

void CbFieldLabel::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_text_format", "value" ), &CbFieldLabel::set_text_format );
	ClassDB::bind_method( D_METHOD( "get_text_format" ), &CbFieldLabel::get_text_format );
	ClassDB::bind_method( D_METHOD( "set_conditions", "value" ), &CbFieldLabel::set_conditions );
	ClassDB::bind_method( D_METHOD( "get_conditions" ), &CbFieldLabel::get_conditions );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "text_format", PROPERTY_HINT_MULTILINE_TEXT ), "set_text_format", "get_text_format" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "conditions" ), "set_conditions", "get_conditions" );
}

void CbFieldLabel::_ready()
{
	set_process( InGame() );
}

void CbFieldLabel::_process( double )
{
	CinderboxClient* client = FindClient( this, m_client );
	bool show = client != nullptr && client->has_local_player() && client->check_local_conditions( m_conditions );
	set_visible( show );
	if ( show && m_format.is_empty() == false )
	{
		set_text( client->format_local_fields( m_format ) );
	}
}

// --- CbFieldBinding --------------------------------------------------------------------------------

void CbFieldBinding::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_field", "value" ), &CbFieldBinding::set_field );
	ClassDB::bind_method( D_METHOD( "get_field" ), &CbFieldBinding::get_field );
	ClassDB::bind_method( D_METHOD( "set_target", "value" ), &CbFieldBinding::set_target );
	ClassDB::bind_method( D_METHOD( "get_target" ), &CbFieldBinding::get_target );
	ClassDB::bind_method( D_METHOD( "set_property", "value" ), &CbFieldBinding::set_property );
	ClassDB::bind_method( D_METHOD( "get_property" ), &CbFieldBinding::get_property );
	ClassDB::bind_method( D_METHOD( "set_multiply", "value" ), &CbFieldBinding::set_multiply );
	ClassDB::bind_method( D_METHOD( "get_multiply" ), &CbFieldBinding::get_multiply );
	ClassDB::bind_method( D_METHOD( "set_add", "value" ), &CbFieldBinding::set_add );
	ClassDB::bind_method( D_METHOD( "get_add" ), &CbFieldBinding::get_add );
	ClassDB::bind_method( D_METHOD( "set_conditions", "value" ), &CbFieldBinding::set_conditions );
	ClassDB::bind_method( D_METHOD( "get_conditions" ), &CbFieldBinding::get_conditions );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "field" ), "set_field", "get_field" );
	ADD_PROPERTY( PropertyInfo( Variant::NODE_PATH, "target" ), "set_target", "get_target" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "property" ), "set_property", "get_property" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "multiply" ), "set_multiply", "get_multiply" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "add" ), "set_add", "get_add" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "conditions" ), "set_conditions", "get_conditions" );
}

void CbFieldBinding::_ready()
{
	set_process( InGame() );
}

void CbFieldBinding::_process( double )
{
	CinderboxClient* client = FindClient( this, m_client );
	Node* target = get_node_or_null( m_target );
	if ( client == nullptr || target == nullptr )
	{
		return;
	}
	if ( m_conditions.is_empty() == false )
	{
		bool holds = client->has_local_player() && client->check_local_conditions( m_conditions );
		Variant visible = target->get( "visible" );
		if ( visible.get_type() == Variant::BOOL && bool( visible ) != holds )
		{
			target->set( "visible", holds );
		}
		if ( holds == false )
		{
			return;
		}
	}
	if ( m_field.is_empty() || m_property.is_empty() )
	{
		return;
	}
	Variant value = client->get_local_field( m_field );
	if ( value.get_type() == Variant::NIL )
	{
		return; // the server does not run the mod that declares it
	}
	Variant current = target->get( m_property );
	if ( current.get_type() == Variant::BOOL )
	{
		target->set( m_property, double( value ) * m_multiply + m_add != 0.0 );
	}
	else if ( current.get_type() == Variant::INT )
	{
		target->set( m_property, int64_t( double( value ) * m_multiply + m_add ) );
	}
	else
	{
		target->set( m_property, double( value ) * m_multiply + m_add );
	}
}

// --- CbEventFeed -----------------------------------------------------------------------------------

void CbEventFeed::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_event", "value" ), &CbEventFeed::set_event );
	ClassDB::bind_method( D_METHOD( "get_event" ), &CbEventFeed::get_event );
	ClassDB::bind_method( D_METHOD( "set_text_format", "value" ), &CbEventFeed::set_text_format );
	ClassDB::bind_method( D_METHOD( "get_text_format" ), &CbEventFeed::get_text_format );
	ClassDB::bind_method( D_METHOD( "set_nobody_text", "value" ), &CbEventFeed::set_nobody_text );
	ClassDB::bind_method( D_METHOD( "get_nobody_text" ), &CbEventFeed::get_nobody_text );
	ClassDB::bind_method( D_METHOD( "set_max_lines", "value" ), &CbEventFeed::set_max_lines );
	ClassDB::bind_method( D_METHOD( "get_max_lines" ), &CbEventFeed::get_max_lines );
	ClassDB::bind_method( D_METHOD( "set_line_seconds", "value" ), &CbEventFeed::set_line_seconds );
	ClassDB::bind_method( D_METHOD( "get_line_seconds" ), &CbEventFeed::get_line_seconds );
	ClassDB::bind_method( D_METHOD( "set_label_settings", "value" ), &CbEventFeed::set_label_settings );
	ClassDB::bind_method( D_METHOD( "get_label_settings" ), &CbEventFeed::get_label_settings );
	ClassDB::bind_method( D_METHOD( "on_mod_event", "name", "a", "b", "value", "position", "vector" ), &CbEventFeed::on_mod_event );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "event" ), "set_event", "get_event" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "text_format" ), "set_text_format", "get_text_format" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "nobody_text" ), "set_nobody_text", "get_nobody_text" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "max_lines", PROPERTY_HINT_RANGE, "1,20" ), "set_max_lines", "get_max_lines" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "line_seconds", PROPERTY_HINT_RANGE, "0.5,30,0.1" ), "set_line_seconds",
				  "get_line_seconds" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "label_settings", PROPERTY_HINT_RESOURCE_TYPE, "LabelSettings" ),
				  "set_label_settings", "get_label_settings" );
}

void CbEventFeed::_ready()
{
	set_process( InGame() );
}

void CbEventFeed::_process( double )
{
	if ( m_connected == false )
	{
		if ( CinderboxClient* client = FindClient( this, m_client ) )
		{
			client->connect( "mod_event", Callable( this, "on_mod_event" ) );
			m_connected = true;
		}
	}
	// Lines leave oldest first once their time is up.
	double now = Now();
	while ( m_expires.empty() == false && m_expires.front() <= now && get_child_count() > 0 )
	{
		m_expires.erase( m_expires.begin() );
		Node* oldest = get_child( 0 );
		remove_child( oldest );
		oldest->queue_free();
	}
}

void CbEventFeed::on_mod_event( const String& name, int64_t a, int64_t b, int64_t value, const Vector3&, const Vector3& )
{
	if ( name != m_event )
	{
		return;
	}
	CinderboxClient* client = FindClient( this, m_client );
	if ( client == nullptr )
	{
		return;
	}
	auto who = [&]( int64_t id ) {
		String n = id != 0 ? client->get_player_name( id ) : String();
		return n.is_empty() ? m_nobody : n;
	};
	Label* line = memnew( Label );
	line->set_text( m_format.replace( "{a}", who( a ) ).replace( "{b}", who( b ) ).replace( "{value}", String::num_int64( value ) ) );
	if ( m_labelSettings.is_valid() )
	{
		line->set_label_settings( m_labelSettings );
	}
	line->set_horizontal_alignment( HORIZONTAL_ALIGNMENT_RIGHT );
	add_child( line );
	m_expires.push_back( Now() + double( m_lineSeconds ) );
	while ( get_child_count() > m_maxLines )
	{
		Node* oldest = get_child( 0 );
		remove_child( oldest );
		oldest->queue_free();
		m_expires.erase( m_expires.begin() );
	}
}

// --- CbScoreboard ----------------------------------------------------------------------------------

void CbScoreboard::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_headers", "value" ), &CbScoreboard::set_headers );
	ClassDB::bind_method( D_METHOD( "get_headers" ), &CbScoreboard::get_headers );
	ClassDB::bind_method( D_METHOD( "set_cells", "value" ), &CbScoreboard::set_cells );
	ClassDB::bind_method( D_METHOD( "get_cells" ), &CbScoreboard::get_cells );
	ClassDB::bind_method( D_METHOD( "set_sort_field", "value" ), &CbScoreboard::set_sort_field );
	ClassDB::bind_method( D_METHOD( "get_sort_field" ), &CbScoreboard::get_sort_field );
	ClassDB::bind_method( D_METHOD( "set_show_action", "value" ), &CbScoreboard::set_show_action );
	ClassDB::bind_method( D_METHOD( "get_show_action" ), &CbScoreboard::get_show_action );
	ClassDB::bind_method( D_METHOD( "set_label_settings", "value" ), &CbScoreboard::set_label_settings );
	ClassDB::bind_method( D_METHOD( "get_label_settings" ), &CbScoreboard::get_label_settings );
	ClassDB::bind_method( D_METHOD( "set_conditions", "value" ), &CbScoreboard::set_conditions );
	ClassDB::bind_method( D_METHOD( "get_conditions" ), &CbScoreboard::get_conditions );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "conditions" ), "set_conditions", "get_conditions" );
	ClassDB::bind_method( D_METHOD( "set_local_settings", "value" ), &CbScoreboard::set_local_settings );
	ClassDB::bind_method( D_METHOD( "get_local_settings" ), &CbScoreboard::get_local_settings );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "headers" ), "set_headers", "get_headers" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "cells" ), "set_cells", "get_cells" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "sort_field" ), "set_sort_field", "get_sort_field" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "show_action" ), "set_show_action", "get_show_action" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "label_settings", PROPERTY_HINT_RESOURCE_TYPE, "LabelSettings" ),
				  "set_label_settings", "get_label_settings" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "local_settings", PROPERTY_HINT_RESOURCE_TYPE, "LabelSettings" ),
				  "set_local_settings", "get_local_settings" );
}

void CbScoreboard::_ready()
{
	set_process( InGame() );
}

Label* CbScoreboard::Cell( int index )
{
	while ( get_child_count() <= index )
	{
		add_child( memnew( Label ) );
	}
	return Object::cast_to<Label>( get_child( index ) );
}

void CbScoreboard::_process( double )
{
	CinderboxClient* client = FindClient( this, m_client );
	bool held = m_showAction.is_empty() ||
				( InputMap::get_singleton()->has_action( m_showAction ) && Input::get_singleton()->is_action_pressed( m_showAction ) );
	bool show = client != nullptr && held && m_cells.is_empty() == false && client->check_local_conditions( m_conditions );
	set_visible( show );
	if ( show == false )
	{
		return;
	}

	int columns = int( m_cells.size() );
	set_columns( columns );
	PackedInt64Array players = client->get_players();
	std::vector<int64_t> order;
	for ( int64_t i = 0; i < players.size(); ++i )
	{
		order.push_back( players[i] );
	}
	if ( m_sortField.is_empty() == false )
	{
		std::stable_sort( order.begin(), order.end(), [&]( int64_t x, int64_t y ) {
			return double( client->get_field( x, m_sortField ) ) > double( client->get_field( y, m_sortField ) );
		} );
	}

	int used = 0;
	for ( int c = 0; c < columns && m_headers.size() > 0; ++c )
	{
		Label* cell = Cell( used++ );
		cell->set_text( c < m_headers.size() ? m_headers[c] : String() );
		cell->set_label_settings( m_labelSettings );
		cell->set_visible( true );
	}
	int64_t me = client->get_local_net_id();
	for ( int64_t id : order )
	{
		for ( int c = 0; c < columns; ++c )
		{
			Label* cell = Cell( used++ );
			cell->set_text( client->format_fields( id, m_cells[c] ) );
			cell->set_label_settings( id == me && m_localSettings.is_valid() ? m_localSettings : m_labelSettings );
			cell->set_visible( true );
		}
	}
	for ( int i = used; i < get_child_count(); ++i )
	{
		Object::cast_to<Label>( get_child( i ) )->set_visible( false );
	}
}

} // namespace cb::gd
