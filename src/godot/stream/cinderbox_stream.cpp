#include "cinderbox_stream.h"

#include "stream_source.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include <algorithm>
#include <cstring>
#include <string>

using namespace godot;

namespace cb::gd
{

namespace
{

std::string ToStd( const String& s )
{
	CharString utf8 = s.utf8();
	return std::string( utf8.get_data(), size_t( utf8.length() ) );
}

} // namespace

CinderboxStream::CinderboxStream() = default;
CinderboxStream::~CinderboxStream() = default;

void CinderboxStream::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "connect_to_server" ), &CinderboxStream::connect_to_server );
	ClassDB::bind_method( D_METHOD( "stop" ), &CinderboxStream::stop );
	ClassDB::bind_method( D_METHOD( "is_running" ), &CinderboxStream::is_running );
	ClassDB::bind_method( D_METHOD( "take", "whole" ), &CinderboxStream::take );
	ClassDB::bind_method( D_METHOD( "takes_input" ), &CinderboxStream::takes_input );
	ClassDB::bind_method( D_METHOD( "set_input", "input" ), &CinderboxStream::set_input );
	ClassDB::bind_method( D_METHOD( "control", "name", "value" ), &CinderboxStream::control );

	ClassDB::bind_method( D_METHOD( "set_host", "host" ), &CinderboxStream::set_host );
	ClassDB::bind_method( D_METHOD( "get_host" ), &CinderboxStream::get_host );
	ClassDB::bind_method( D_METHOD( "set_port", "port" ), &CinderboxStream::set_port );
	ClassDB::bind_method( D_METHOD( "get_port" ), &CinderboxStream::get_port );
	ClassDB::bind_method( D_METHOD( "set_player_name", "name" ), &CinderboxStream::set_player_name );
	ClassDB::bind_method( D_METHOD( "get_player_name" ), &CinderboxStream::get_player_name );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "host" ), "set_host", "get_host" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "port", PROPERTY_HINT_RANGE, "1,65535" ), "set_port", "get_port" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "player_name" ), "set_player_name", "get_player_name" );
}

void CinderboxStream::connect_to_server()
{
	m_source.reset();
	m_handoff.Reset();
	StreamOptions options;
	options.host = ToStd( m_host );
	options.port = uint16_t( std::clamp( m_port, 1, 65535 ) );
	options.playerName = ToStd( m_playerName );
	m_source = std::make_unique<StreamSource>( options );
}

void CinderboxStream::stop()
{
	m_source.reset();
}

bool CinderboxStream::is_running() const
{
	return m_source != nullptr;
}

PackedByteArray CinderboxStream::take( bool whole )
{
	return m_source ? m_handoff.Take( *m_source, whole ) : PackedByteArray();
}

bool CinderboxStream::takes_input() const
{
	return m_source != nullptr && m_source->TakesInput();
}

void CinderboxStream::set_input( const PackedByteArray& input )
{
	if ( m_source && input.size() == int64_t( sizeof( PlayerInput ) ) )
	{
		// (The server bounds it again: it trusts no client.)
		PlayerInput in;
		std::memcpy( &in, input.ptr(), sizeof( PlayerInput ) );
		m_source->SetInput( in );
	}
}

void CinderboxStream::control( const String& name, double value )
{
	if ( m_source )
	{
		m_source->Control( ToStd( name ), value );
	}
}

} // namespace cb::gd

namespace
{

void InitializeStream( ModuleInitializationLevel level )
{
	if ( level == MODULE_INITIALIZATION_LEVEL_SCENE )
	{
		GDREGISTER_CLASS( cb::gd::CinderboxStream );
	}
}

void UninitializeStream( ModuleInitializationLevel )
{
}

} // namespace

extern "C"
{
	GDExtensionBool GDE_EXPORT cinderbox_stream_library_init( GDExtensionInterfaceGetProcAddress getProcAddress,
															  GDExtensionClassLibraryPtr library, GDExtensionInitialization* initialization )
	{
		GDExtensionBinding::InitObject init( getProcAddress, library, initialization );
		init.register_initializer( InitializeStream );
		init.register_terminator( UninitializeStream );
		init.set_minimum_library_initialization_level( MODULE_INITIALIZATION_LEVEL_SCENE );
		return init.init();
	}
}
