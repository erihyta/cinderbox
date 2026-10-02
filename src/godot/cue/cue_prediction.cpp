#include "cue_prediction.h"

#include "cue_director.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::cue
{

bool ParseChange( const String& text, Change& out, String* error )
{
	auto fail = [&]( const String& why ) {
		if ( error != nullptr )
		{
			*error = why;
		}
		return false;
	};
	String rest = text.strip_edges();
	int64_t at = rest.find( "=" );
	if ( at <= 0 )
	{
		return fail( "write it as a field, an operator and a number: pistol.ammo -= 1 (operators: -=, +=, =)" );
	}
	out = Change();
	char32_t before = rest[at - 1];
	String name = rest.substr( 0, at );
	if ( before == '-' || before == '+' )
	{
		out.op = char( before );
		name = rest.substr( 0, at - 1 );
	}
	out.field = name.strip_edges();
	String number = rest.substr( at + 1 ).strip_edges();
	if ( out.field.is_empty() || out.field.contains( " " ) )
	{
		return fail( "name one field before the operator" );
	}
	if ( number.is_valid_float() == false )
	{
		return fail( "\"" + number + "\" is not a number" );
	}
	out.value = number.to_float();
	return true;
}

} // namespace cb::cue

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
	ClassDB::bind_method( D_METHOD( "set_changes", "changes" ), &CbPrediction::set_changes );
	ClassDB::bind_method( D_METHOD( "get_changes" ), &CbPrediction::get_changes );
	ClassDB::bind_method( D_METHOD( "set_stance", "stance" ), &CbPrediction::set_stance );
	ClassDB::bind_method( D_METHOD( "get_stance" ), &CbPrediction::get_stance );
	ClassDB::bind_method( D_METHOD( "set_stance_layer", "layer" ), &CbPrediction::set_stance_layer );
	ClassDB::bind_method( D_METHOD( "get_stance_layer" ), &CbPrediction::get_stance_layer );
	ADD_GROUP( "Until the server answers", "" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "changes" ), "set_changes", "get_changes" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "stance", PROPERTY_HINT_PLACEHOLDER_TEXT, "melee_swing" ), "set_stance", "get_stance" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "stance_layer", PROPERTY_HINT_PLACEHOLDER_TEXT, "full" ), "set_stance_layer",
				  "get_stance_layer" );
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
	for ( int64_t i = 0; i < m_changes.size(); ++i )
	{
		cue::Change change;
		String error;
		if ( cue::ParseChange( m_changes[i], change, &error ) == false )
		{
			warnings.push_back( "Change \"" + m_changes[i] + "\": " + error );
		}
	}
	if ( m_stance.strip_edges().is_empty() != m_stanceLayer.strip_edges().is_empty() )
	{
		warnings.push_back( "A predicted stance needs both: the stance and the layer the server's mod sets it on." );
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
