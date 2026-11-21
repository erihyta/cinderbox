#include "sheet_dump.h"

#include "sheet_text.h"

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

void Write( const Value& v, const std::string& indent, std::string& out )
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
				Write( list[i], indent + "    ", out );
			}
			break;
		}
		case Value::Type::Dict:
		{
			const Dict dict = v.AsDict();
			out += "dict\n";
			for ( const auto& [key, value] : dict.Entries() )
			{
				out += indent + "  " + key + ": ";
				Write( value, indent + "  ", out );
			}
			break;
		}
	}
}

} // namespace

std::string Dump( const std::vector<Rule>& rules, const Strings& errors )
{
	std::string out;
	for ( const std::string& error : errors )
	{
		out += "error " + Quoted( error ) + "\n";
	}
	for ( const Rule& rule : rules )
	{
		out += "rule " + rule.type + " line " + std::to_string( rule.line ) + " head " + std::to_string( rule.head ) +
			   ( rule.prediction ? " prediction" : "" ) + "\n";
		for ( const auto& [key, value] : rule.properties.Entries() )
		{
			out += "  " + key + ": ";
			Write( value, "  ", out );
		}
	}
	return out;
}

} // namespace cb::sheet
