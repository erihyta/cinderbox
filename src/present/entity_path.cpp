#include "entity_path.h"

namespace cb::present
{

namespace
{

std::string Trim( const std::string& s )
{
	size_t begin = s.find_first_not_of( " \t" );
	if ( begin == std::string::npos )
	{
		return std::string();
	}
	size_t end = s.find_last_not_of( " \t" );
	return s.substr( begin, end - begin + 1 );
}

bool Fail( std::string* error, const std::string& why )
{
	if ( error != nullptr )
	{
		*error = why;
	}
	return false;
}

} // namespace

bool ParseEntityPath( const std::string& text, EntityPath& out, std::string* error )
{
	out.parts.clear();
	std::string path = Trim( text );
	if ( path.empty() )
	{
		return true;
	}
	size_t start = 0;
	while ( start <= path.size() )
	{
		size_t slash = path.find( '/', start );
		std::string word = Trim( path.substr( start, slash == std::string::npos ? std::string::npos : slash - start ) );
		start = slash == std::string::npos ? path.size() + 1 : slash + 1;

		EntityPath::Part part;
		bool root = false;
		if ( word == "self" )
		{
			part.step = EntityPath::Step::Self;
			root = true;
		}
		else if ( word == "event.a" )
		{
			part.step = EntityPath::Step::EventA;
			root = true;
		}
		else if ( word == "event.b" )
		{
			part.step = EntityPath::Step::EventB;
			root = true;
		}
		else if ( word == "local" )
		{
			part.step = EntityPath::Step::Local;
			root = true;
		}
		else if ( word == "world" )
		{
			part.step = EntityPath::Step::World;
			root = true;
		}
		else if ( word == "holder" )
		{
			part.step = EntityPath::Step::Holder;
		}
		else if ( word.rfind( "item:", 0 ) == 0 && word.size() > 5 )
		{
			part.step = EntityPath::Step::Item;
			part.socket = Trim( word.substr( 5 ) );
		}
		else
		{
			return Fail( error, "\"" + word + "\" is not a step (self, holder, item:<socket>, event.a, event.b, local, world)" );
		}
		if ( root && out.parts.empty() == false )
		{
			return Fail( error, "\"" + word + "\" can only start a path" );
		}
		if ( out.parts.empty() == false && out.parts[0].step == EntityPath::Step::World )
		{
			return Fail( error, "\"world\" is not an entity: nothing follows it" );
		}
		out.parts.push_back( part );
	}
	return true;
}

PathTarget ResolveEntityPath( const EntityPath& path, const PathContext& context )
{
	PathTarget target;
	target.netId = context.self;
	for ( const EntityPath::Part& part : path.parts )
	{
		switch ( part.step )
		{
			case EntityPath::Step::Self:
				target.netId = context.self;
				break;
			case EntityPath::Step::EventA:
				target.netId = context.eventA;
				break;
			case EntityPath::Step::EventB:
				target.netId = context.eventB;
				break;
			case EntityPath::Step::Local:
				target.netId = context.local;
				break;
			case EntityPath::Step::World:
				target.world = true;
				target.netId = 0;
				return target;
			case EntityPath::Step::Holder:
				target.netId = target.netId != 0 && context.holderOf ? context.holderOf( target.netId ) : 0;
				break;
			case EntityPath::Step::Item:
				target.netId = target.netId != 0 && context.itemIn ? context.itemIn( target.netId, part.socket ) : 0;
				break;
		}
		if ( target.netId == 0 )
		{
			return PathTarget();
		}
	}
	return target;
}

bool ParsePathCondition( const std::string& text, PathCondition& out, std::string* error )
{
	out = PathCondition();
	std::string rest = Trim( text );
	std::string prefix;
	for ( const char* p : { "!?", "!", "?" } )
	{
		if ( rest.rfind( p, 0 ) == 0 )
		{
			prefix = p;
			rest = Trim( rest.substr( prefix.size() ) );
			break;
		}
	}
	// A path ends at the first colon, if one comes before any comparison.
	size_t colon = rest.find( ':' );
	size_t op = rest.find_first_of( "=!<>" );
	// "item:RightHand:field": the socket's colon belongs to the path.
	if ( colon != std::string::npos && ( op == std::string::npos || colon < op ) )
	{
		size_t last = colon;
		while ( true )
		{
			size_t next = rest.find( ':', last + 1 );
			if ( next == std::string::npos || ( op != std::string::npos && next > op ) )
			{
				break;
			}
			last = next;
		}
		if ( ParseEntityPath( rest.substr( 0, last ), out.path, error ) == false )
		{
			return false;
		}
		rest = Trim( rest.substr( last + 1 ) );
	}
	out.condition = prefix + rest;
	return true;
}

} // namespace cb::present
