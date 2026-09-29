#include "cue_paths.h"

using namespace godot;

namespace cb::cue
{

namespace
{

// "^" -> 1, "^^" -> 2; 0 when the name is not a caret anchor.
int Carets( const String& name )
{
	if ( name.is_empty() )
	{
		return 0;
	}
	for ( int64_t i = 0; i < name.length(); ++i )
	{
		if ( name[i] != '^' )
		{
			return 0;
		}
	}
	return int( name.length() );
}

bool IsAnchorName( const String& name )
{
	return Carets( name ) > 0 || name == "$at" || name == "$other" || name == "$local" || name == "$world";
}

double AsNumber( const Variant& v )
{
	switch ( v.get_type() )
	{
		case Variant::BOOL:
			return bool( v ) ? 1.0 : 0.0;
		case Variant::INT:
			return double( int64_t( v ) );
		case Variant::FLOAT:
			return double( v );
		default:
			return 0.0;
	}
}

bool ParseNumber( const String& text, double& out )
{
	if ( text == "true" || text == "false" )
	{
		out = text == "true" ? 1.0 : 0.0;
		return true;
	}
	if ( text.is_valid_float() == false )
	{
		return false;
	}
	out = text.to_float();
	return true;
}

} // namespace

bool IsEntity( const Node* node )
{
	return node != nullptr && bool( node->get_meta( kEntityMeta, false ) );
}

Node* EntityOf( Node* node, const Node* stop )
{
	for ( Node* n = node; n != nullptr && n != stop; n = n->get_parent() )
	{
		if ( IsEntity( n ) )
		{
			return n;
		}
	}
	return nullptr;
}

bool CheckPath( const NodePath& path, String* error )
{
	if ( path.is_absolute() )
	{
		if ( error != nullptr )
		{
			*error = "an absolute path leaves the world; start from ^, $at, $other, $local or $world";
		}
		return false;
	}
	for ( int64_t i = 1; i < path.get_name_count(); ++i )
	{
		if ( IsAnchorName( path.get_name( i ) ) )
		{
			if ( error != nullptr )
			{
				*error = "\"" + String( path.get_name( i ) ) + "\" can only start a path";
			}
			return false;
		}
	}
	return true;
}

bool FromCue( const NodePath& path )
{
	if ( path.get_name_count() == 0 )
	{
		return false;
	}
	String first = path.get_name( 0 );
	return first == "$at" || first == "$other";
}

Node* Resolve( const NodePath& path, Node* origin, const Context& context )
{
	if ( origin == nullptr || path.is_empty() || CheckPath( path, nullptr ) == false )
	{
		return nullptr;
	}
	String first = path.get_name( 0 );
	Node* base = nullptr;
	int64_t skip = 1;
	if ( int carets = Carets( first ) )
	{
		base = EntityOf( origin, context.director );
		for ( int i = 1; i < carets && base != nullptr; ++i )
		{
			base = EntityOf( base->get_parent(), context.director );
		}
	}
	else if ( first == "$at" )
	{
		base = context.at;
	}
	else if ( first == "$other" )
	{
		base = context.other;
	}
	else if ( first == "$local" )
	{
		base = context.local;
	}
	else if ( first == "$world" )
	{
		base = context.director;
	}
	else
	{
		base = origin;
		skip = 0;
	}
	if ( base == nullptr )
	{
		return nullptr;
	}
	Node* found = base;
	if ( path.get_name_count() > skip )
	{
		String rest;
		for ( int64_t i = skip; i < path.get_name_count(); ++i )
		{
			rest += ( i > skip ? "/" : "" ) + String( path.get_name( i ) );
		}
		found = base->get_node_or_null( NodePath( rest ) );
	}
	// Nothing outside the world: a workshop item cannot reach the game's HUD or menus.
	if ( found != nullptr && context.director != nullptr && found != context.director && context.director->is_ancestor_of( found ) == false )
	{
		return nullptr;
	}
	return found;
}

bool ParseCondition( const String& text, Condition& out, String* error )
{
	out = Condition();
	String rest = text.strip_edges();
	String prefix;
	for ( const char* p : { "!?", "!", "?" } )
	{
		if ( rest.begins_with( p ) )
		{
			prefix = p;
			rest = rest.substr( prefix.length() ).strip_edges();
			break;
		}
	}
	// A path ends at the last colon before any comparison.
	int64_t op = -1;
	for ( int64_t i = 0; i < rest.length(); ++i )
	{
		char32_t c = rest[i];
		if ( c == '=' || c == '!' || c == '<' || c == '>' )
		{
			op = i;
			break;
		}
	}
	String head = op >= 0 ? rest.substr( 0, op ) : rest;
	int64_t colon = head.rfind( ":" );
	if ( colon >= 0 )
	{
		out.hasPath = true;
		out.path = NodePath( head.substr( 0, colon ).strip_edges() );
		if ( out.path.is_empty() || CheckPath( out.path, error ) == false )
		{
			if ( error != nullptr && error->is_empty() )
			{
				*error = "nothing before the colon";
			}
			return false;
		}
		rest = rest.substr( colon + 1 ).strip_edges();
	}
	if ( rest.is_empty() )
	{
		if ( error != nullptr )
		{
			*error = "no name to test";
		}
		return false;
	}
	out.test = prefix + rest;
	return true;
}

bool Test( const String& condition, const std::function<bool( const String&, Variant& )>& lookup )
{
	String text = condition.strip_edges();
	if ( text.is_empty() )
	{
		return true;
	}
	Variant value;
	if ( text.begins_with( "!?" ) )
	{
		return lookup( text.substr( 2 ).strip_edges(), value ) == false;
	}
	if ( text.begins_with( "!" ) && text.begins_with( "!=" ) == false )
	{
		lookup( text.substr( 1 ).strip_edges(), value );
		return AsNumber( value ) == 0.0;
	}
	if ( text.begins_with( "?" ) )
	{
		return lookup( text.substr( 1 ).strip_edges(), value );
	}
	static const char* kOps[] = { "==", "!=", ">=", "<=", ">", "<" };
	for ( const char* op : kOps )
	{
		int64_t at = text.find( op );
		if ( at < 0 )
		{
			continue;
		}
		double rhs = 0.0;
		if ( ParseNumber( text.substr( at + String( op ).length() ).strip_edges(), rhs ) == false )
		{
			return false;
		}
		lookup( text.substr( 0, at ).strip_edges(), value );
		double lhs = AsNumber( value );
		String o = op;
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
	lookup( text, value );
	return AsNumber( value ) != 0.0;
}

bool LookUp( const String& name, Node* entity, const Context& context, Variant& out )
{
	out = Variant();
	if ( name == "is_local" )
	{
		out = entity != nullptr && entity == context.local;
		return true;
	}
	if ( name.begins_with( "event." ) )
	{
		String key = name.substr( 6 );
		if ( context.event && context.args.has( key ) )
		{
			out = context.args[key];
			return true;
		}
		return false;
	}
	if ( entity != nullptr )
	{
		Dictionary state = entity->get_meta( kStateMeta, Dictionary() );
		if ( state.has( name ) )
		{
			out = state[name];
			return true;
		}
	}
	if ( context.director != nullptr )
	{
		Dictionary world = context.director->get_meta( kStateMeta, Dictionary() );
		if ( world.has( name ) )
		{
			out = world[name];
			return true;
		}
		PackedStringArray known = context.director->get_meta( kKnownMeta, PackedStringArray() );
		if ( known.has( name ) )
		{
			out = 0;
			return true;
		}
	}
	return false;
}

} // namespace cb::cue
