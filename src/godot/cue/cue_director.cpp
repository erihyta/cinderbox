#include "cue_director.h"

#include "cue_guard.h"
#include "cue_prediction.h"
#include "cue_reaction.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>

using namespace godot;

namespace cb::gd
{

void CbDirector::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "add_entity", "node", "kind", "template_name", "id" ), &CbDirector::add_entity, DEFVAL( 0 ) );
	ClassDB::bind_method( D_METHOD( "set_state", "entity", "state" ), &CbDirector::set_state );
	ClassDB::bind_method( D_METHOD( "get_state", "entity" ), &CbDirector::get_state );
	ClassDB::bind_method( D_METHOD( "set_world_state", "state" ), &CbDirector::set_world_state );
	ClassDB::bind_method( D_METHOD( "set_known", "names" ), &CbDirector::set_known );
	ClassDB::bind_method( D_METHOD( "set_local", "entity" ), &CbDirector::set_local );
	ClassDB::bind_method( D_METHOD( "get_local" ), &CbDirector::get_local );
	ClassDB::bind_method( D_METHOD( "cue", "name", "at", "other", "args" ), &CbDirector::cue, DEFVAL( Variant() ),
						  DEFVAL( Dictionary() ) );
	ClassDB::bind_method( D_METHOD( "press", "action" ), &CbDirector::press );
	ClassDB::bind_method( D_METHOD( "explain_press", "action" ), &CbDirector::explain_press );
	ClassDB::bind_method( D_METHOD( "pending_predictions" ), &CbDirector::pending_predictions );
	ClassDB::bind_method( D_METHOD( "explain", "name", "at", "other", "args" ), &CbDirector::explain, DEFVAL( Variant() ),
						  DEFVAL( Dictionary() ) );
	ClassDB::bind_method( D_METHOD( "update" ), &CbDirector::update );
	ClassDB::bind_static_method( "CbDirector", D_METHOD( "check_scene", "scene" ), &CbDirector::check_scene );
	ClassDB::bind_static_method( "CbDirector", D_METHOD( "instantiate", "scene" ), &CbDirector::instantiate );
	ClassDB::bind_method( D_METHOD( "set_auto_update", "value" ), &CbDirector::set_auto_update );
	ClassDB::bind_method( D_METHOD( "get_auto_update" ), &CbDirector::get_auto_update );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "auto_update" ), "set_auto_update", "get_auto_update" );

	// A press was shown as this cue before the server answered (CbPrediction).
	ADD_SIGNAL( MethodInfo( "predicted", PropertyInfo( Variant::STRING, "cue" ) ) );
	// A reaction shook the camera or flashed the screen: the viewer's, so the game applies it.
	ADD_SIGNAL( MethodInfo( "screen_effect", PropertyInfo( Variant::FLOAT, "shake" ), PropertyInfo( Variant::FLOAT, "shake_time" ),
							PropertyInfo( Variant::COLOR, "flash_color" ), PropertyInfo( Variant::FLOAT, "flash_time" ) ) );
}

String CbDirector::check_scene( const Ref<PackedScene>& scene )
{
	return cue::CheckScene( scene );
}

Node* CbDirector::instantiate( const Ref<PackedScene>& scene )
{
	return cue::Instantiate( scene );
}

void CbDirector::add_entity( Node* node, const String& kind, const String& template_name, int64_t id )
{
	if ( node == nullptr )
	{
		return;
	}
	if ( id != 0 )
	{
		Dictionary ids = get_meta( cue::kIdsMeta, Dictionary() );
		ids[id] = int64_t( node->get_instance_id() );
		set_meta( cue::kIdsMeta, ids );
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

void CbDirector::Register( CbPrediction* prediction )
{
	m_predictions.push_back( ObjectID( prediction->get_instance_id() ) );
}

void CbDirector::Unregister( CbPrediction* prediction )
{
	ObjectID id( prediction->get_instance_id() );
	m_predictions.erase( std::remove( m_predictions.begin(), m_predictions.end(), id ), m_predictions.end() );
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

namespace
{
// How long a press waits for the server's cue. After that the next cue of the name plays in full.
constexpr double kEchoSeconds = 1.0;
} // namespace

void CbDirector::Play( const String& name, const cue::Context& context, const std::vector<ObjectID>* skip, std::vector<ObjectID>* acted )
{
	Index();
	auto it = m_byCue.find( name );
	if ( it == m_byCue.end() )
	{
		return;
	}
	double now = double( Time::get_singleton()->get_ticks_usec() ) / 1e6;
	// A copy: a reaction may add scenes with reactions of their own.
	std::vector<ObjectID> ids = it->second;
	for ( ObjectID id : ids )
	{
		if ( skip != nullptr && std::find( skip->begin(), skip->end(), id ) != skip->end() )
		{
			continue;
		}
		auto* reaction = Object::cast_to<CbReaction>( ObjectDB::get_instance( id ) );
		if ( reaction != nullptr && reaction->Fire( context, now ) && acted != nullptr )
		{
			acted->push_back( id );
		}
	}
}

void CbDirector::cue( const String& name, Node* at, Node* other, const Dictionary& args )
{
	// The server's word on something the viewer's press already showed: what played then does not
	// play again; what had to wait for this (it uses what only the server knows) plays now.
	Pending();
	std::vector<ObjectID> already;
	bool echo = false;
	if ( at != nullptr && at == get_local() )
	{
		for ( auto it = m_shown.begin(); it != m_shown.end(); ++it )
		{
			if ( it->cue == name )
			{
				already = std::move( it->acted );
				m_shown.erase( it );
				echo = true;
				break;
			}
		}
	}
	Play( name, CueContext( at, other, args ), echo ? &already : nullptr, nullptr );
}

const std::deque<CbDirector::Shown>& CbDirector::Pending()
{
	double now = double( Time::get_singleton()->get_ticks_usec() ) / 1e6;
	while ( m_shown.empty() == false && now - m_shown.front().at > kEchoSeconds )
	{
		m_shown.pop_front();
	}
	return m_shown;
}

Array CbDirector::pending_predictions()
{
	double now = double( Time::get_singleton()->get_ticks_usec() ) / 1e6;
	Array out;
	for ( const Shown& shown : Pending() )
	{
		Dictionary entry;
		entry["cue"] = shown.cue;
		entry["age"] = now - shown.at;
		entry["changes"] = shown.changes;
		entry["stance"] = shown.stance;
		entry["stance_layer"] = shown.stanceLayer;
		out.push_back( entry );
	}
	return out;
}

Dictionary CbDirector::explain_press( const String& action ) const
{
	Dictionary out;
	double now = double( Time::get_singleton()->get_ticks_usec() ) / 1e6;
	cue::Context base = BaseContext();
	for ( ObjectID id : m_predictions )
	{
		auto* prediction = Object::cast_to<CbPrediction>( ObjectDB::get_instance( id ) );
		if ( prediction != nullptr )
		{
			String why = prediction->Refusal( action, base, now );
			out[String( get_path_to( prediction ) )] = why.is_empty() ? "predicts " + prediction->Cue().strip_edges() : why;
		}
	}
	return out;
}

int CbDirector::press( const String& action )
{
	Node* local = get_local();
	if ( local == nullptr )
	{
		return 0;
	}
	double now = double( Time::get_singleton()->get_ticks_usec() ) / 1e6;
	cue::Context base = BaseContext();
	int predicted = 0;
	std::vector<ObjectID> ids = m_predictions;
	for ( ObjectID id : ids )
	{
		auto* prediction = Object::cast_to<CbPrediction>( ObjectDB::get_instance( id ) );
		if ( prediction == nullptr || prediction->Accepts( action, base, now ) == false )
		{
			continue;
		}
		String name = prediction->Cue().strip_edges();
		cue::Context context = CueContext( local, nullptr, Dictionary() );
		context.predicted = true;
		Shown shown;
		shown.cue = name;
		shown.at = now;
		shown.changes = prediction->get_changes();
		shown.stance = prediction->get_stance().strip_edges();
		shown.stanceLayer = prediction->get_stance_layer().strip_edges();
		Play( name, context, nullptr, &shown.acted );
		m_shown.push_back( std::move( shown ) );
		emit_signal( "predicted", name );
		predicted += 1;
	}
	return predicted;
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
