#include "object_source.h"

#include "view_codec.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <utility>

using namespace godot;

namespace cb::gd
{

ObjectSource::ObjectSource( const Variant& object )
	: m_object( object )
{
}

Object* ObjectSource::Target() const
{
	// (Null for an object that was freed meanwhile.)
	return m_object.get_validated_object();
}

bool ObjectSource::Take( present::ViewFrame& out )
{
	Object* target = Target();
	if ( target == nullptr )
	{
		return false;
	}
	Variant result = target->call( "take", m_wantWhole );
	if ( result.get_type() != Variant::PACKED_BYTE_ARRAY )
	{
		if ( m_warned == false )
		{
			m_warned = true;
			UtilityFunctions::push_warning( "Cinderbox: a view source must have take( whole ) -> PackedByteArray" );
		}
		return false;
	}
	PackedByteArray bytes = result;
	if ( bytes.is_empty() )
	{
		return false;
	}
	bool decoded = present::DecodeView( bytes.ptr(), size_t( bytes.size() ), m_haveLast && m_wantWhole == false ? &m_last : nullptr, m_scratch );
	if ( decoded == false )
	{
		// Out of step with the source (or not a packet at all): start over from a whole frame.
		m_wantWhole = true;
		return false;
	}
	std::swap( m_last, m_scratch );
	m_haveLast = true;
	m_wantWhole = false;
	out = m_last;
	return true;
}

float ObjectSource::CameraFreeDistance( b3Vec3 origin, b3Vec3 direction, float maxDistance, float radius )
{
	Object* target = Target();
	if ( target == nullptr || target->has_method( "camera_free_distance" ) == false )
	{
		return -1.0f;
	}
	Variant result = target->call( "camera_free_distance", Vector3( origin.x, origin.y, origin.z ),
								   Vector3( direction.x, direction.y, direction.z ), maxDistance, radius );
	return result.get_type() == Variant::FLOAT || result.get_type() == Variant::INT ? float( double( result ) ) : -1.0f;
}

bool ObjectSource::TakesInput() const
{
	Object* target = Target();
	return target != nullptr && target->has_method( "takes_input" ) && bool( target->call( "takes_input" ) );
}

void ObjectSource::SetInput( const PlayerInput& input )
{
	Object* target = Target();
	if ( target == nullptr || target->has_method( "set_input" ) == false )
	{
		return;
	}
	PackedByteArray bytes;
	bytes.resize( sizeof( PlayerInput ) );
	std::memcpy( bytes.ptrw(), &input, sizeof( PlayerInput ) );
	target->call( "set_input", bytes );
}

void ObjectSource::SendText( const std::string& channel, const std::string& text )
{
	Object* target = Target();
	if ( target != nullptr && target->has_method( "send_text" ) )
	{
		target->call( "send_text", String::utf8( channel.c_str() ), String::utf8( text.c_str() ) );
	}
}

void ObjectSource::Control( const std::string& name, double value )
{
	Object* target = Target();
	if ( target != nullptr && target->has_method( "control" ) )
	{
		target->call( "control", String::utf8( name.c_str() ), value );
	}
}

} // namespace cb::gd
