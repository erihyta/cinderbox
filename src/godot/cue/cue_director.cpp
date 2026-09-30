#include "cue_director.h"

#include "cue_reaction.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>

using namespace godot;

namespace cb::gd
{

void CbDirector::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "add_entity", "node", "kind", "template_name" ), &CbDirector::add_entity );
	ClassDB::bind_method( D_METHOD( "set_state", "entity", "state" ), &CbDirector::set_state );
	ClassDB::bind_method( D_METHOD( "get_state", "entity" ), &CbDirector::get_state );
	ClassDB::bind_method( D_METHOD( "set_world_state", "state" ), &CbDirector::set_world_state );
	ClassDB::bind_method( D_METHOD( "set_known", "names" ), &CbDirector::set_known );
	ClassDB::bind_method( D_METHOD( "set_local", "entity" ), &CbDirector::set_local );
	ClassDB::bind_method( D_METHOD( "get_local" ), &CbDirector::get_local );
	ClassDB::bind_method( D_METHOD( "cue", "name", "at", "other", "args" ), &CbDirector::cue, DEFVAL( Variant() ),
						  DEFVAL( Dictionary() ) );
	ClassDB::bind_method( D_METHOD( "explain", "name", "at", "other", "args" ), &CbDirector::explain, DEFVAL( Variant() ),
						  DEFVAL( Dictionary() ) );
	ClassDB::bind_method( D_METHOD( "update" ), &CbDirector::update );
	ClassDB::bind_method( D_METHOD( "set_auto_update", "value" ), &CbDirector::set_auto_update );
	ClassDB::bind_method( D_METHOD( "get_auto_update" ), &CbDirector::get_auto_update );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "auto_update" ), "set_auto_update", "get_auto_update" );

	// A reaction shook the camera or flashed the screen: the viewer's, so the game applies it.
	ADD_SIGNAL( MethodInfo( "screen_effect", PropertyInfo( Variant::FLOAT, "shake" ), PropertyInfo( Variant::FLOAT, "shake_time" ),
							PropertyInfo( Variant::COLOR, "flash_color" ), PropertyInfo( Variant::FLOAT, "flash_time" ) ) );
}

void CbDirector::add_entity( Node* node, const String& kind, const String& template_name )
{
	if ( node == nullptr )
	{
		return;
	}
	node->set_meta( cue::kEntityMeta, true );
	node->set_meta( cue::kKindMeta, kind );
	node->set_meta( cue::kTemplateMeta, template_name );
	if ( node->has_meta( cue::kStateMeta ) == false )
	{
		node->set_meta( cue::kStateMeta, Dictionary() );
	}
}

void CbDirector::set_state( Node* entity, const Dictionary& state )
{
	if ( entity != nullptr )
	{
		entity->set_meta( cue::kStateMeta, state );
	}
}

Dictionary CbDirector::get_state( Node* entity ) const
{
	return entity != nullptr ? Dictionary( entity->get_meta( cue::kStateMeta, Dictionary() ) ) : Dictionary();
}

void CbDirector::set_world_state( const Dictionary& state )
{
	set_meta( cue::kStateMeta, state );
}

void CbDirector::set_known( const PackedStringArray& names )
{
	set_meta( cue::kKnownMeta, names );
}

void CbDirector::set_local( Node* entity )
{
	m_local = entity != nullptr ? ObjectID( entity->get_instance_id() ) : ObjectID();
}

Node* CbDirector::get_local() const
{
	return Object::cast_to<Node>( ObjectDB::get_instance( m_local ) );
}

void CbDirector::Register( CbReaction* reaction )
{
	m_reactions.push_back( ObjectID( reaction->get_instance_id() ) );
	m_dirty = true;
}

void CbDirector::Unregister( CbReaction* reaction )
{
	ObjectID id( reaction->get_instance_id() );
	m_reactions.erase( std::remove( m_reactions.begin(), m_reactions.end(), id ), m_reactions.end() );
	m_dirty = true;
}

void CbDirector::Index()
{
	if ( m_dirty == false )
	{
		return;
	}
	m_dirty = false;
	m_whiles.clear();
	m_byCue.clear();
	for ( ObjectID id : m_reactions )
	{
		auto* reaction = Object::cast_to<CbReaction>( ObjectDB::get_instance( id ) );
		if ( reaction == nullptr )
		{
			continue;
		}
		if ( reaction->IsWhile() )
		{
			m_whiles.push_back( id );
		}
		else
		{
			m_byCue[reaction->Event().strip_edges()].push_back( id );
		}
	}
}

cue::Context CbDirector::BaseContext() const
{
	cue::Context context;
	context.director = const_cast<CbDirector*>( this );
	context.local = get_local();
	return context;
}

cue::Context CbDirector::CueContext( Node* at, Node* other, const Dictionary& args ) const
{
	cue::Context context = BaseContext();
	context.event = true;
	context.at = at;
	context.other = other;
	context.args = args;
	context.point = args.get( "point", Vector3() );
	context.end = args.get( "end", context.point );
	return context;
}

Dictionary CbDirector::explain( const String& name, Node* at, Node* other, const Dictionary& args )
{
	Index();
	Dictionary result;
	auto it = m_byCue.find( name );
	if ( it == m_byCue.end() )
	{
		return result;
	}
	cue::Context context = CueContext( at, other, args );
	double now = double( Time::get_singleton()->get_ticks_usec() ) / 1e6;
	for ( ObjectID id : it->second )
	{
		if ( auto* reaction = Object::cast_to<CbReaction>( ObjectDB::get_instance( id ) ) )
		{
			result[String( get_path_to( reaction ) )] = reaction->Explain( context, now );
		}
	}
	return result;
}

void CbDirector::cue( const String& name, Node* at, Node* other, const Dictionary& args )
{
	Index();
	auto it = m_byCue.find( name );
	if ( it == m_byCue.end() )
	{
		return;
	}
	cue::Context context = CueContext( at, other, args );
	double now = double( Time::get_singleton()->get_ticks_usec() ) / 1e6;
	// A copy: a reaction may add scenes with reactions of their own.
	std::vector<ObjectID> ids = it->second;
	for ( ObjectID id : ids )
	{
		auto* reaction = Object::cast_to<CbReaction>( ObjectDB::get_instance( id ) );
		if ( reaction != nullptr )
		{
			reaction->Fire( context, now );
		}
	}
}

void CbDirector::update()
{
	Index();
	cue::Context context = BaseContext();
	std::vector<ObjectID> ids = m_whiles;
	for ( ObjectID id : ids )
	{
		if ( auto* reaction = Object::cast_to<CbReaction>( ObjectDB::get_instance( id ) ) )
		{
			reaction->Update( context );
		}
	}
}

void CbDirector::_process( double )
{
	if ( m_autoUpdate )
	{
		update();
	}
}

} // namespace cb::gd
