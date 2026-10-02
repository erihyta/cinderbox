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
	return Carets( name ) > 0 || name == "$at" || name == "$other" || name == "$local" || name == "$world" || name.contains( "@" );
}

// "$local@pickup.target" -> "$local" and "pickup.target"; "@x" -> "^" and "x".
void SplitAnchor( const String& first, String& anchor, String& field )
{
	int64_t at = first.find( "@" );
	anchor = at < 0 ? first : first.substr( 0, at );
	field = at < 0 ? String() : first.substr( at + 1 );
	if ( at >= 0 && anchor.is_empty() )
	{
		anchor = "^";
	}
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
	String anchor, field;
	SplitAnchor( path.get_name( 0 ), anchor, field );
	return anchor == "$at" || anchor == "$other";
}

Node* Resolve( const NodePath& path, Node* origin, const Context& context )
{
	if ( origin == nullptr || path.is_empty() || CheckPath( path, nullptr ) == false )
	{
		return nullptr;
	}
	String first, field;
	SplitAnchor( path.get_name( 0 ), first, field );
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
	if ( field.is_empty() == false )
	{
		// The entity whose id that state field holds.
		Node* holder = base == context.director ? base : EntityOf( base, context.director );
		Dictionary state = holder != nullptr ? Dictionary( holder->get_meta( kStateMeta, Dictionary() ) ) : Dictionary();
		int64_t id = int64_t( state.get( field, 0 ) );
		Dictionary ids = context.director != nullptr ? Dictionary( context.director->get_meta( kIdsMeta, Dictionary() ) ) : Dictionary();
		base = id != 0 ? Object::cast_to<Node>( ObjectDB::get_instance( ObjectID( uint64_t( int64_t( ids.get( id, 0 ) ) ) ) ) ) : nullptr;
		if ( base == nullptr )
		{
			return nullptr;
		}
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
	std::string problem;
	if ( expr::Compile( std::string( text.utf8().get_data() ), out.program, problem ) == false )
	{
		if ( error != nullptr )
		{
			*error = String::utf8( problem.c_str() );
		}
		return false;
	}
	for ( const std::string& name : out.program.names )
	{
		std::string path, plain;
		expr::SplitName( name, path, plain );
		if ( path.empty() == false && CheckPath( NodePath( String::utf8( path.c_str() ) ), error ) == false )
		{
			return false;
		}
	}
	return true;
}

double Evaluate( const Condition& condition, Node* origin, Node* subject, const Context& context, String* missing )
{
	return double( expr::Evaluate( condition.program, [&]( const std::string& name, float& value ) {
		std::string path, plain;
		expr::SplitName( name, path, plain );
		Node* whose = subject;
		if ( path.empty() == false )
		{
			Node* found = Resolve( NodePath( String::utf8( path.c_str() ) ), origin, context );
			if ( found == nullptr )
			{
				if ( missing != nullptr )
				{
					*missing = String::utf8( path.c_str() );
				}
				return false;
			}
			whose = found == context.director ? nullptr : EntityOf( found, context.director );
		}
		Variant found;
		bool known = LookUp( String::utf8( plain.c_str() ), whose, context, found );
		value = float( AsNumber( found ) );
		return known;
	} ) );
}

bool ReadsTheCue( const Condition& condition )
{
	for ( const std::string& name : condition.program.names )
	{
		std::string path, plain;
		expr::SplitName( name, path, plain );
		if ( plain.rfind( "event.", 0 ) == 0 || path == "$other" || path.rfind( "$other/", 0 ) == 0 || path.rfind( "$other@", 0 ) == 0 )
		{
			return true;
		}
	}
	return false;
}

PackedStringArray StateNames( const Condition& condition )
{
	PackedStringArray out;
	for ( const std::string& name : condition.program.names )
	{
		std::string path, plain;
		expr::SplitName( name, path, plain );
		if ( plain != "is_local" && plain.rfind( "event.", 0 ) != 0 )
		{
			out.push_back( String::utf8( plain.c_str() ) );
		}
	}
	return out;
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
