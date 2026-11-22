#include "sheet_dump.h"

#include "sheet_text.h"

#include <algorithm>

namespace cb::sheet
{

namespace
{

std::string Quoted( const std::string& text )
{
	std::string out = "\"";
	for ( char c : text )
	{
		out += c == '"' ? std::string( "\\\"" ) : c == '\\' ? std::string( "\\\\" ) : c == '\t' ? std::string( "\\t" ) : std::string( 1, c );
	}
	return out + "\"";
}

DictEntries InOrder( const Dict& dict, bool meaning )
{
	DictEntries entries = dict.Entries();
	if ( meaning )
	{
		std::stable_sort( entries.begin(), entries.end(), []( const auto& a, const auto& b ) { return a.first < b.first; } );
	}
	return entries;
}

void Write( const Value& v, const std::string& indent, std::string& out, bool meaning )
{
	switch ( v.type() )
	{
		case Value::Type::Nil:
			out += "nil\n";
			break;
		case Value::Type::Bool:
			out += std::string( "bool " ) + ( v.AsBool() ? "true" : "false" ) + "\n";
			break;
		case Value::Type::Int:
			out += "int " + std::to_string( v.AsInt() ) + "\n";
			break;
		case Value::Type::Float:
			// (As the float it becomes: a reaction's property, a motion's number.)
			out += "float " + Num( v.AsFloat() ) + "\n";
			break;
		case Value::Type::String:
			out += "text " + Quoted( v.AsString() ) + "\n";
			break;
		case Value::Type::Vec2:
			out += "vec2 " + Num( v.AsVec2().x ) + " " + Num( v.AsVec2().y ) + "\n";
			break;
		case Value::Type::Vec3:
			out += "vec3 " + Num( v.AsVec3().x ) + " " + Num( v.AsVec3().y ) + " " + Num( v.AsVec3().z ) + "\n";
			break;
		case Value::Type::Color:
			out += "color " + Num( v.AsColor().r ) + " " + Num( v.AsColor().g ) + " " + Num( v.AsColor().b ) + " " + Num( v.AsColor().a ) + "\n";
			break;
		case Value::Type::Strings:
			out += "texts\n";
			for ( const std::string& s : v.AsStrings() )
			{
				out += indent + "  - " + Quoted( s ) + "\n";
			}
			break;
		case Value::Type::List:
		{
			const List list = v.AsList();
			out += "list\n";
			for ( size_t i = 0; i < list.size(); ++i )
			{
				out += indent + "  - ";
				Write( list[i], indent + "    ", out, meaning );
			}
			break;
		}
		case Value::Type::Dict:
		{
			out += "dict\n";
			for ( const auto& [key, value] : InOrder( v.AsDict(), meaning ) )
			{
				if ( meaning && key == "line" )
				{
					continue;
				}
				out += indent + "  " + key + ": ";
				Write( value, indent + "  ", out, meaning );
			}
			break;
		}
	}
}

std::string DumpWith( const std::vector<Rule>& rules, const Strings& errors, bool meaning )
{
	std::string out;
	for ( const std::string& error : errors )
	{
		// ("line N: why": without its N when only the meaning is asked.)
		const size_t colon = error.find( ": " );
		const bool numbered = meaning && error.rfind( "line ", 0 ) == 0 && colon != std::string::npos;
		out += "error " + Quoted( numbered ? error.substr( colon + 2 ) : error ) + "\n";
	}
	for ( const Rule& rule : rules )
	{
		out += "rule " + rule.type + ( meaning ? std::string() : " line " + std::to_string( rule.line ) + " head " + std::to_string( rule.head ) ) +
			   ( rule.prediction ? " prediction" : "" ) + "\n";
		for ( const auto& [key, value] : InOrder( rule.properties, meaning ) )
		{
			out += "  " + key + ": ";
			Write( value, "  ", out, meaning );
		}
	}
	return out;
}

} // namespace

std::string Dump( const std::vector<Rule>& rules, const Strings& errors )
{
	return DumpWith( rules, errors, false );
}

std::string DumpMeaning( const std::vector<Rule>& rules, const Strings& errors )
{
	return DumpWith( rules, errors, true );
}

} // namespace cb::sheet
