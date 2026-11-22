#include "sheet_value.h"

namespace cb::sheet
{

List::List()
	: m_items( std::make_shared<ListItems>() )
{
}

size_t List::size() const
{
	return m_items->size();
}

void List::push_back( const Value& value )
{
	m_items->push_back( value );
}

Value& List::operator[]( size_t index )
{
	return ( *m_items )[index];
}

const Value& List::operator[]( size_t index ) const
{
	return ( *m_items )[index];
}

List List::Duplicate() const
{
	return List( std::make_shared<ListItems>( *m_items ) );
}

Dict::Dict()
	: m_entries( std::make_shared<DictEntries>() )
{
}

bool Dict::Has( const std::string& key ) const
{
	for ( const auto& entry : *m_entries )
	{
		if ( entry.first == key )
		{
			return true;
		}
	}
	return false;
}

Value Dict::Get( const std::string& key, const Value& fallback ) const
{
	for ( const auto& entry : *m_entries )
	{
		if ( entry.first == key )
		{
			return entry.second;
		}
	}
	return fallback;
}

const Value& Dict::At( const std::string& key ) const
{
	static const Value kNil;
	for ( const auto& entry : *m_entries )
	{
		if ( entry.first == key )
		{
			return entry.second;
		}
	}
	return kNil;
}

Value& Dict::operator[]( const std::string& key )
{
	for ( auto& entry : *m_entries )
	{
		if ( entry.first == key )
		{
			return entry.second;
		}
	}
	m_entries->emplace_back( key, Value() );
	return m_entries->back().second;
}

void Dict::Erase( const std::string& key )
{
	for ( auto it = m_entries->begin(); it != m_entries->end(); ++it )
	{
		if ( it->first == key )
		{
			m_entries->erase( it );
			return;
		}
	}
}

void Dict::Merge( const Dict& other, bool overwrite )
{
	const DictEntries copy = *other.m_entries; // (merging a dict into itself)
	for ( const auto& entry : copy )
	{
		if ( overwrite || Has( entry.first ) == false )
		{
			( *this )[entry.first] = entry.second;
		}
	}
}

Dict Dict::Duplicate() const
{
	return Dict( std::make_shared<DictEntries>( *m_entries ) );
}

size_t Dict::size() const
{
	return m_entries->size();
}

const DictEntries& Dict::Entries() const
{
	return *m_entries;
}

Strings Dict::Keys() const
{
	Strings out;
	for ( const auto& entry : *m_entries )
	{
		out.push_back( entry.first );
	}
	return out;
}

Value::Value( bool v )
	: m_type( Type::Bool )
	, m_bool( v )
{
}

Value::Value( int v )
	: m_type( Type::Int )
	, m_int( v )
{
}

Value::Value( int64_t v )
	: m_type( Type::Int )
	, m_int( v )
{
}

Value::Value( double v )
	: m_type( Type::Float )
	, m_float( v )
{
}

Value::Value( const char* v )
	: m_type( Type::String )
	, m_string( v )
{
}

Value::Value( const std::string& v )
	: m_type( Type::String )
	, m_string( v )
{
}

Value::Value( Float2 v )
	: m_type( Type::Vec2 )
	, m_vec2( v )
{
}

Value::Value( Float3 v )
	: m_type( Type::Vec3 )
	, m_vec3( v )
{
}

Value::Value( Color4 v )
	: m_type( Type::Color )
	, m_color( v )
{
}

Value::Value( const Strings& v )
	: m_type( Type::Strings )
	, m_strings( v )
{
}

Value::Value( const List& v )
	: m_type( Type::List )
	, m_list( v.m_items )
{
}

Value::Value( const Dict& v )
	: m_type( Type::Dict )
	, m_dict( v.m_entries )
{
}

bool Value::AsBool() const
{
	switch ( m_type )
	{
		case Type::Bool:
			return m_bool;
		case Type::Int:
			return m_int != 0;
		case Type::Float:
			return m_float != 0.0;
		case Type::String:
			return m_string.empty() == false;
		default:
			return false;
	}
}

int64_t Value::AsInt() const
{
	switch ( m_type )
	{
		case Type::Bool:
			return m_bool ? 1 : 0;
		case Type::Int:
			return m_int;
		case Type::Float:
			return int64_t( m_float );
		default:
			return 0;
	}
}

double Value::AsFloat() const
{
	switch ( m_type )
	{
		case Type::Bool:
			return m_bool ? 1.0 : 0.0;
		case Type::Int:
			return double( m_int );
		case Type::Float:
			return m_float;
		default:
			return 0.0;
	}
}

const std::string& Value::AsString() const
{
	static const std::string kEmpty;
	return m_type == Type::String ? m_string : kEmpty;
}

Float2 Value::AsVec2() const
{
	return m_type == Type::Vec2 ? m_vec2 : Float2{};
}

Float3 Value::AsVec3() const
{
	return m_type == Type::Vec3 ? m_vec3 : Float3{};
}

Color4 Value::AsColor() const
{
	return m_type == Type::Color ? m_color : Color4{};
}

const Strings& Value::AsStrings() const
{
	static const Strings kNone;
	return m_type == Type::Strings ? m_strings : kNone;
}

List Value::AsList() const
{
	return m_type == Type::List && m_list ? List( m_list ) : List();
}

Dict Value::AsDict() const
{
	return m_type == Type::Dict && m_dict ? Dict( m_dict ) : Dict();
}

} // namespace cb::sheet
