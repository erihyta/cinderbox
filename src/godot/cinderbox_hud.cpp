#include "cinderbox_hud.h"

#include "cinderbox_client.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::gd
{

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
	set_process( Engine::get_singleton()->is_editor_hint() == false );
}

CinderboxClient* CbFieldLabel::Client()
{
	if ( auto* client = Object::cast_to<CinderboxClient>( ObjectDB::get_instance( m_client ) ) )
	{
		return client;
	}
	if ( is_inside_tree() == false )
	{
		return nullptr;
	}
	auto* client = Object::cast_to<CinderboxClient>( get_tree()->get_first_node_in_group( "cinderbox_client" ) );
	m_client = client != nullptr ? client->get_instance_id() : ObjectID();
	return client;
}

void CbFieldLabel::_process( double )
{
	CinderboxClient* client = Client();
	bool show = client != nullptr && client->has_local_player() && client->check_local_conditions( m_conditions );
	set_visible( show );
	if ( show && m_format.is_empty() == false )
	{
		set_text( client->format_local_fields( m_format ) );
	}
}

} // namespace cb::gd
