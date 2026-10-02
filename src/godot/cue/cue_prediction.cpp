#include "cue_prediction.h"

#include "cue_director.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::gd
{

void CbPrediction::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_action", "action" ), &CbPrediction::set_action );
	ClassDB::bind_method( D_METHOD( "get_action" ), &CbPrediction::get_action );
	ClassDB::bind_method( D_METHOD( "set_cue", "cue" ), &CbPrediction::set_cue );
	ClassDB::bind_method( D_METHOD( "get_cue" ), &CbPrediction::get_cue );
	ClassDB::bind_method( D_METHOD( "set_conditions", "conditions" ), &CbPrediction::set_conditions );
	ClassDB::bind_method( D_METHOD( "get_conditions" ), &CbPrediction::get_conditions );
	ClassDB::bind_method( D_METHOD( "set_cooldown", "seconds" ), &CbPrediction::set_cooldown );
	ClassDB::bind_method( D_METHOD( "get_cooldown" ), &CbPrediction::get_cooldown );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "action", PROPERTY_HINT_PLACEHOLDER_TEXT, "fire" ), "set_action", "get_action" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "cue", PROPERTY_HINT_PLACEHOLDER_TEXT, "pistol.fired" ), "set_cue", "get_cue" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "conditions" ), "set_conditions", "get_conditions" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "cooldown", PROPERTY_HINT_RANGE, "0,10,0.01,suffix:s" ), "set_cooldown", "get_cooldown" );
}

void CbPrediction::_notification( int what )
{
	if ( what == NOTIFICATION_ENTER_TREE )
	{
		for ( Node* n = get_parent(); n != nullptr; n = n->get_parent() )
		{
			if ( auto* director = Object::cast_to<CbDirector>( n ) )
			{
				m_director = director;
				director->Register( this );
				break;
			}
		}
	}
	else if ( what == NOTIFICATION_EXIT_TREE && m_director != nullptr )
	{
		m_director->Unregister( this );
		m_director = nullptr;
	}
}

bool CbPrediction::Parse() const
{
	if ( m_parsed )
	{
		return m_valid;
	}
	m_parsed = true;
	m_valid = true;
	m_tests.clear();
	for ( int64_t i = 0; i < m_conditions.size(); ++i )
	{
		cue::Condition condition;
		m_valid &= cue::ParseCondition( m_conditions[i], condition, nullptr );
		m_tests.push_back( condition );
	}
	return m_valid;
}

PackedStringArray CbPrediction::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	if ( m_action.strip_edges().is_empty() )
	{
		warnings.push_back( "Name the action whose press this predicts (one the server's mod declares: fire)." );
	}
	if ( m_cue.strip_edges().is_empty() )
	{
		warnings.push_back( "Name the cue the server will send for that press (pistol.fired): the same name, so the same reactions play." );
	}
	for ( int64_t i = 0; i < m_conditions.size(); ++i )
	{
		cue::Condition condition;
		String error;
		if ( cue::ParseCondition( m_conditions[i], condition, &error ) == false )
		{
			warnings.push_back( "Condition \"" + m_conditions[i] + "\": " + error );
		}
	}
	return warnings;
}

bool CbPrediction::Accepts( const String& action, const cue::Context& context, double now )
{
	if ( Refusal( action, context, now ).is_empty() == false )
	{
		return false;
	}
	m_lastPredicted = now;
	return true;
}

String CbPrediction::Refusal( const String& action, const cue::Context& context, double now ) const
{
	auto no = []( const String& reason ) { return reason; };
	if ( action != m_action.strip_edges() )
	{
		return no( "it predicts another action (" + m_action.strip_edges() + ")" );
	}
	if ( m_cue.strip_edges().is_empty() )
	{
		return no( "it names no cue" );
	}
	if ( Parse() == false )
	{
		return no( "a condition does not parse (see the node's warnings)" );
	}
	if ( context.local == nullptr )
	{
		return no( "there is no local player" );
	}
	// Conditions read the viewer's own entity (and the world), like a reaction whose subject it is.
	Node* local = cue::EntityOf( context.local, context.director );
	for ( size_t i = 0; i < m_tests.size(); ++i )
	{
		const cue::Condition& c = m_tests[i];
		Node* whose = local;
		if ( c.hasPath )
		{
			Node* found = cue::Resolve( c.path, const_cast<CbPrediction*>( this ), context );
			if ( found == nullptr )
			{
				return no( "condition \"" + m_conditions[int64_t( i )] + "\": " + String( c.path ) + " finds nothing" );
			}
			whose = found == context.director ? nullptr : cue::EntityOf( found, context.director );
		}
		auto lookup = [&]( const String& name, Variant& out ) { return cue::LookUp( name, whose, context, out ); };
		if ( cue::Test( c.test, lookup ) == false )
		{
			return no( "condition \"" + m_conditions[int64_t( i )] + "\" is false" );
		}
	}
	if ( m_cooldown > 0.0 && now - m_lastPredicted < m_cooldown )
	{
		return no( "cooling down" );
	}
	return String();
}

} // namespace cb::gd
