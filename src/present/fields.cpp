#include "fields.h"

#include <cstdio>
#include <cstdlib>

namespace cb::present
{

namespace
{

std::string Trim( const std::string& s )
{
	size_t a = s.find_first_not_of( " \t" );
	if ( a == std::string::npos )
	{
		return {};
	}
	size_t b = s.find_last_not_of( " \t" );
	return s.substr( a, b - a + 1 );
}

bool ParseNumber( const std::string& text, float& out )
{
	if ( text == "true" )
	{
		out = 1.0f;
		return true;
	}
	if ( text == "false" )
	{
		out = 0.0f;
		return true;
	}
	char* end = nullptr;
	out = std::strtof( text.c_str(), &end );
	return end != text.c_str() && *end == '\0';
}

} // namespace

FieldValue ReadField( const ModSchema& schema, const std::string& name, const Blackboard* board, const int32_t* globals )
{
	FieldValue v;
	const BoardField* field = schema.FindField( name );
	if ( field == nullptr )
	{
		return v;
	}
	v.type = field->type;
	v.declared = true;
	if ( field->scope == BoardScope::Global )
	{
		v.raw = globals != nullptr ? globals[field->slot] : 0;
	}
	else
	{
		v.raw = board != nullptr ? board->values[field->slot] : 0;
	}
	return v;
}

bool CheckCondition( const ModSchema& schema, const std::string& condition, const Blackboard* board, const int32_t* globals )
{
	std::string text = Trim( condition );
	if ( text.empty() )
	{
		return true;
	}
	if ( text.rfind( "!?", 0 ) == 0 )
	{
		return schema.FindField( Trim( text.substr( 2 ) ) ) == nullptr;
	}
	if ( text[0] == '!' )
	{
		return ReadField( schema, Trim( text.substr( 1 ) ), board, globals ).AsBool() == false;
	}
	if ( text[0] == '?' )
	{
		return schema.FindField( Trim( text.substr( 1 ) ) ) != nullptr;
	}

	static const char* kOps[] = { "==", "!=", ">=", "<=", ">", "<" };
	for ( const char* op : kOps )
	{
		size_t at = text.find( op );
		if ( at == std::string::npos )
		{
			continue;
		}
		std::string name = Trim( text.substr( 0, at ) );
		float rhs = 0.0f;
		if ( ParseNumber( Trim( text.substr( at + std::string( op ).size() ) ), rhs ) == false )
		{
			return false;
		}
		float lhs = ReadField( schema, name, board, globals ).AsFloat();
		std::string o = op;
		if ( o == "==" )
			return lhs == rhs;
		if ( o == "!=" )
			return lhs != rhs;
		if ( o == ">=" )
			return lhs >= rhs;
		if ( o == "<=" )
			return lhs <= rhs;
		if ( o == ">" )
			return lhs > rhs;
		return lhs < rhs;
	}
	return ReadField( schema, text, board, globals ).AsBool();
}

bool CheckConditions( const ModSchema& schema, const std::vector<std::string>& conditions, const Blackboard* board,
					  const int32_t* globals )
{
	for ( const std::string& c : conditions )
	{
		if ( CheckCondition( schema, c, board, globals ) == false )
		{
			return false;
		}
	}
	return true;
}

std::string FormatFields( const ModSchema& schema, const std::string& format, const Blackboard* board, const int32_t* globals )
{
	std::string out;
	for ( size_t i = 0; i < format.size(); ++i )
	{
		char c = format[i];
		if ( c == '{' && i + 1 < format.size() && format[i + 1] == '{' )
		{
			out += '{';
			++i;
			continue;
		}
		if ( c != '{' )
		{
			out += c;
			continue;
		}
		size_t close = format.find( '}', i );
		if ( close == std::string::npos )
		{
			out += format.substr( i );
			break;
		}
		FieldValue v = ReadField( schema, Trim( format.substr( i + 1, close - i - 1 ) ), board, globals );
		char buffer[32];
		switch ( v.type )
		{
			case BoardType::Float:
				std::snprintf( buffer, sizeof( buffer ), "%.1f", double( v.AsFloat() ) );
				break;
			case BoardType::Bool:
				std::snprintf( buffer, sizeof( buffer ), "%s", v.AsBool() ? "yes" : "no" );
				break;
			case BoardType::Int:
			default:
				std::snprintf( buffer, sizeof( buffer ), "%d", int( v.raw ) );
				break;
		}
		out += buffer;
		i = close;
	}
	return out;
}

} // namespace cb::present
