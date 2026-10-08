#include "fields.h"

#include "expr.h"

#include <cmath>
#include <unordered_map>

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

} // namespace

FieldValue ReadField( const ModSchema& schema, const std::string& name, const Blackboard* board, const BoardValues* globals,
					  const Blackboard* privates )
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
		v.raw = globals != nullptr ? ( *globals )[field->slot] : 0;
	}
	else if ( field->scope == BoardScope::Private )
	{
		v.raw = privates != nullptr ? privates->values[field->slot] : 0;
	}
	else
	{
		v.raw = board != nullptr ? board->values[field->slot] : 0;
	}
	return v;
}

namespace
{

// Programs by their text: HUD nodes ask about the same few conditions every frame.
const expr::Program* Compiled( const std::string& text )
{
	static thread_local std::unordered_map<std::string, std::pair<bool, expr::Program>> cache;
	auto found = cache.find( text );
	if ( found == cache.end() )
	{
		if ( cache.size() > 1024 )
		{
			cache.clear();
		}
		std::pair<bool, expr::Program> entry;
		std::string error;
		entry.first = expr::Compile( text, entry.second, error );
		found = cache.emplace( text, std::move( entry ) ).first;
	}
	return found->second.first ? &found->second.second : nullptr;
}

} // namespace

bool EvaluateFields( const ModSchema& schema, const std::string& expression, const Blackboard* board, const BoardValues* globals, float& out,
					 const ExtraFields* extra, const Blackboard* privates )
{
	const expr::Program* program = Compiled( expression );
	if ( program == nullptr )
	{
		out = 0.0f;
		return false;
	}
	out = expr::Evaluate( *program, [&]( const std::string& name, float& value ) {
		if ( extra != nullptr && *extra && ( *extra )( name, value ) )
		{
			return true;
		}
		FieldValue field = ReadField( schema, name, board, globals, privates );
		value = field.type == BoardType::Bool ? ( field.AsBool() ? 1.0f : 0.0f ) : field.AsFloat();
		return field.declared;
	} );
	return true;
}

bool CheckCondition( const ModSchema& schema, const std::string& condition, const Blackboard* board, const BoardValues* globals,
					 const ExtraFields* extra, const Blackboard* privates )
{
	// What does not parse is not true.
	float value = 0.0f;
	return EvaluateFields( schema, condition, board, globals, value, extra, privates ) && value != 0.0f;
}

bool CheckConditions( const ModSchema& schema, const std::vector<std::string>& conditions, const Blackboard* board,
					  const BoardValues* globals, const Blackboard* privates )
{
	for ( const std::string& c : conditions )
	{
		if ( CheckCondition( schema, c, board, globals, nullptr, privates ) == false )
		{
			return false;
		}
	}
	return true;
}

std::string FormatFields( const ModSchema& schema, const std::string& format, const Blackboard* board, const BoardValues* globals,
						  const Blackboard* privates )
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
		std::string inside = Trim( format.substr( i + 1, close - i - 1 ) );
		FieldValue v = ReadField( schema, inside, board, globals, privates );
		char buffer[32];
		float computed = 0.0f;
		if ( v.declared == false && schema.FindField( inside ) == nullptr &&
			 EvaluateFields( schema, inside, board, globals, computed, nullptr, privates ) )
		{
			// An expression ("{combat.health * 100 / combat.max_health}"): a whole number as one,
			// anything else with one decimal.
			if ( computed == std::floor( computed ) && std::fabs( computed ) < 1e9f )
			{
				std::snprintf( buffer, sizeof( buffer ), "%d", int( computed ) );
			}
			else
			{
				std::snprintf( buffer, sizeof( buffer ), "%.1f", double( computed ) );
			}
			out += buffer;
			i = close;
			continue;
		}
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
