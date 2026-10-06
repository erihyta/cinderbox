#include "cinderbox_peer.h"

#include "live_source.h"
#include "replay_source.h"

#include <godot_cpp/core/class_db.hpp>

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

CinderboxPeer::CinderboxPeer() = default;
CinderboxPeer::~CinderboxPeer() = default;

void CinderboxPeer::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "connect_to_server" ), &CinderboxPeer::connect_to_server );
	ClassDB::bind_method( D_METHOD( "open_replay", "path" ), &CinderboxPeer::open_replay );
	ClassDB::bind_method( D_METHOD( "stop" ), &CinderboxPeer::stop );
	ClassDB::bind_method( D_METHOD( "is_running" ), &CinderboxPeer::is_running );
	ClassDB::bind_method( D_METHOD( "take", "whole" ), &CinderboxPeer::take );
	ClassDB::bind_method( D_METHOD( "takes_input" ), &CinderboxPeer::takes_input );
	ClassDB::bind_method( D_METHOD( "set_input", "input" ), &CinderboxPeer::set_input );
	ClassDB::bind_method( D_METHOD( "control", "name", "value" ), &CinderboxPeer::control );

	ClassDB::bind_method( D_METHOD( "set_host", "host" ), &CinderboxPeer::set_host );
	ClassDB::bind_method( D_METHOD( "get_host" ), &CinderboxPeer::get_host );
	ClassDB::bind_method( D_METHOD( "set_port", "port" ), &CinderboxPeer::set_port );
	ClassDB::bind_method( D_METHOD( "get_port" ), &CinderboxPeer::get_port );
	ClassDB::bind_method( D_METHOD( "set_player_name", "name" ), &CinderboxPeer::set_player_name );
	ClassDB::bind_method( D_METHOD( "get_player_name" ), &CinderboxPeer::get_player_name );
	ClassDB::bind_method( D_METHOD( "set_rollback_min", "ticks" ), &CinderboxPeer::set_rollback_min );
	ClassDB::bind_method( D_METHOD( "get_rollback_min" ), &CinderboxPeer::get_rollback_min );
	ClassDB::bind_method( D_METHOD( "set_rollback_max", "ticks" ), &CinderboxPeer::set_rollback_max );
	ClassDB::bind_method( D_METHOD( "get_rollback_max" ), &CinderboxPeer::get_rollback_max );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "host" ), "set_host", "get_host" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "port", PROPERTY_HINT_RANGE, "1,65535" ), "set_port", "get_port" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "player_name" ), "set_player_name", "get_player_name" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "rollback_min", PROPERTY_HINT_RANGE, "1,64" ), "set_rollback_min", "get_rollback_min" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "rollback_max", PROPERTY_HINT_RANGE, "1,64" ), "set_rollback_max", "get_rollback_max" );
}

void CinderboxPeer::Start( std::unique_ptr<present::ViewSource> source )
{
	m_source.reset();
	m_handoff.Reset();
	m_source = std::move( source );
}

void CinderboxPeer::connect_to_server()
{
	ClientOptions options;
	options.host = ToStd( m_host );
	options.port = uint16_t( std::clamp( m_port, 1, 65535 ) );
	options.minRollbackTicks = uint32_t( std::max( 1, m_rollbackMin ) );
	options.maxRollbackTicks = uint32_t( std::max( m_rollbackMin, m_rollbackMax ) );
	options.logName = "godot";
	options.playerName = ToStd( m_playerName );
	Start( std::make_unique<LiveSource>( options ) );
}

void CinderboxPeer::open_replay( const String& path )
{
	Start( std::make_unique<ReplaySource>( ToStd( path ) ) );
}

void CinderboxPeer::stop()
{
	m_source.reset();
}

bool CinderboxPeer::is_running() const
{
	return m_source != nullptr;
}

PackedByteArray CinderboxPeer::take( bool whole )
{
	return m_source ? m_handoff.Take( *m_source, whole ) : PackedByteArray();
}

bool CinderboxPeer::takes_input() const
{
	return m_source != nullptr && m_source->TakesInput();
}

void CinderboxPeer::set_input( const PackedByteArray& input )
{
	if ( m_source && input.size() == int64_t( sizeof( PlayerInput ) ) )
	{
		PlayerInput in;
		std::memcpy( &in, input.ptr(), sizeof( PlayerInput ) );
		// From another library, so bounded like anything received.
		in.cameraPitch = std::clamp( in.cameraPitch, int16_t( -kMaxCameraPitch ), kMaxCameraPitch );
		in.buttons &= kEngineButtons;
		in.view = in.view < kViewModes ? in.view : uint8_t( 0 );
		in.intent = in.intent <= kLastSlotIntent ? in.intent : uint8_t( 0 );
		m_source->SetInput( in );
	}
}

void CinderboxPeer::control( const String& name, double value )
{
	if ( m_source )
	{
		m_source->Control( ToStd( name ), value );
	}
}

} // namespace cb::gd
