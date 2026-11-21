#include "cue_rules.h"

#include "cue_prediction.h"
#include "cue_reaction.h"

#include "sheet_rules.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <vector>

using namespace godot;

// Since M283 the sheet's parser is src/sheet (plain C++, sheet_rules.h): this is the Godot side of it,
// its rules as Godot values for the nodes they become (CbReaction, CbPrediction, CbFieldBinding) and
// the names check (cue_names.cpp).

namespace cb::cue
{

namespace
{

std::string Std( const String& s )
{
	return std::string( s.utf8().get_data() );
}

String Gd( const std::string& s )
{
	return String::utf8( s.c_str() );
}

// A sheet value as the Variant the parser on Godot's types made. (A part's "amounts" were keyed by
// the field's number: an int.)
Variant ToVariant( const sheet::Value& v, bool intKeys = false )
{
	switch ( v.type() )
	{
		case sheet::Value::Type::Nil:
			return Variant();
		case sheet::Value::Type::Bool:
			return v.AsBool();
		case sheet::Value::Type::Int:
			return v.AsInt();
		case sheet::Value::Type::Float:
			return v.AsFloat();
		case sheet::Value::Type::String:
			return Gd( v.AsString() );
		case sheet::Value::Type::Vec2:
			return Vector2( v.AsVec2().x, v.AsVec2().y );
		case sheet::Value::Type::Vec3:
			return Vector3( v.AsVec3().x, v.AsVec3().y, v.AsVec3().z );
		case sheet::Value::Type::Color:
			return Color( v.AsColor().r, v.AsColor().g, v.AsColor().b, v.AsColor().a );
		case sheet::Value::Type::Strings:
		{
			PackedStringArray out;
			for ( const std::string& s : v.AsStrings() )
			{
				out.push_back( Gd( s ) );
			}
			return out;
		}
		case sheet::Value::Type::List:
		{
			const sheet::List list = v.AsList();
			Array out;
			for ( size_t i = 0; i < list.size(); ++i )
			{
				out.push_back( ToVariant( list[i] ) );
			}
			return out;
		}
		case sheet::Value::Type::Dict:
		{
			Dictionary out;
			const sheet::Dict dict = v.AsDict();
			for ( const auto& [key, value] : dict.Entries() )
			{
				Variant k = intKeys ? Variant( String( key.c_str() ).to_int() ) : Variant( Gd( key ) );
				out[k] = ToVariant( value, key == "amounts" );
			}
			return out;
		}
	}
	return Variant();
}

} // namespace

void SetMoveParameterNames( const PackedStringArray& names )
{
	sheet::Strings all;
	for ( int64_t i = 0; i < names.size(); ++i )
	{
		all.push_back( Std( names[i] ) );
	}
	sheet::SetMoveParameterNames( all );
}

bool IsPathProperty( const String& name )
{
	return sheet::IsPathProperty( Std( name ) );
}

std::vector<Rule> ParseRules( const String& text, PackedStringArray& errors )
{
	sheet::Strings problems;
	const std::vector<sheet::Rule> parsed = sheet::ParseRules( Std( text ), problems );
	for ( const std::string& problem : problems )
	{
		errors.push_back( Gd( problem ) );
	}
	std::vector<Rule> out;
	for ( const sheet::Rule& one : parsed )
	{
		Rule rule;
		rule.line = one.line;
		rule.head = one.head;
		rule.prediction = one.prediction;
		rule.type = Gd( one.type );
		rule.properties = ToVariant( sheet::Value( one.properties ) );
		out.push_back( rule );
	}
	return out;
}

} // namespace cb::cue

namespace cb::gd
{

void CbRules::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_sheet", "value" ), &CbRules::set_sheet );
	ClassDB::bind_method( D_METHOD( "get_sheet" ), &CbRules::get_sheet );
	ClassDB::bind_method( D_METHOD( "get_reaction_count" ), &CbRules::get_reaction_count );
	ClassDB::bind_method( D_METHOD( "get_problems" ), &CbRules::get_problems );
	ClassDB::bind_method( D_METHOD( "get_warnings" ), &CbRules::_get_configuration_warnings );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "sheet", PROPERTY_HINT_RESOURCE_TYPE, "CbSheet" ), "set_sheet", "get_sheet" );
}

void CbRules::set_sheet( const Ref<CbSheet>& v )
{
	if ( v == m_sheet )
	{
		return;
	}
	const Callable changed = callable_mp( this, &CbRules::OnSheetChanged );
	if ( m_sheet.is_valid() && m_sheet->is_connected( "changed", changed ) )
	{
		m_sheet->disconnect( "changed", changed );
	}
	m_sheet = v;
	if ( m_sheet.is_valid() )
	{
		m_sheet->connect( "changed", changed );
	}
	OnSheetChanged();
}

void CbRules::OnSheetChanged()
{
	if ( is_inside_tree() )
	{
		Clear();
		Build();
	}
	update_configuration_warnings();
}

String CbRules::SheetText() const
{
	return m_sheet.is_valid() ? m_sheet->get_text() : String();
}

String CbRules::SheetName() const
{
	if ( m_sheet.is_null() )
	{
		return String();
	}
	return m_sheet->get_path().is_empty() ? String( "(a sheet of its own)" ) : m_sheet->get_path();
}

int64_t CbRules::get_reaction_count() const
{
	return get_child_count( true ) - get_child_count( false );
}

void CbRules::_notification( int what )
{
	if ( what == NOTIFICATION_READY && m_built == false )
	{
		Build();
	}
}

void CbRules::Clear()
{
	for ( int i = get_child_count( true ) - 1; i >= 0; --i )
	{
		Node* child = get_child( i, true );
		if ( child->get_owner() == nullptr )
		{
			remove_child( child );
			child->queue_free();
		}
	}
	m_built = false;
}

void CbRules::Build()
{
	m_built = true;
	m_problems.clear();
	if ( m_sheet.is_null() )
	{
		return;
	}
	std::vector<cue::Rule> rules = cue::ParseRules( m_sheet->get_text(), m_problems );
	// How far a plain path may climb: no higher than the scene (the rules are written from the node
	// this one is under).
	int depth = 0;
	Node* owner = get_owner() != nullptr ? get_owner() : nullptr;
	for ( Node* n = get_parent(); n != nullptr && owner != nullptr && n != owner; n = n->get_parent() )
	{
		++depth;
	}
	for ( const cue::Rule& rule : rules )
	{
		if ( rule.type == "use" || rule.type == "motion" )
		{
			continue; // (baked with the item or the motion set: motions the simulation runs)
		}
		Node* reaction = rule.type == "CbReaction"	   ? static_cast<Node*>( memnew( CbReaction ) )
						 : rule.type == "CbPrediction" ? static_cast<Node*>( memnew( CbPrediction ) )
													   : Object::cast_to<Node>( ClassDB::instantiate( rule.type ) );
		if ( reaction == nullptr )
		{
			m_problems.push_back( "line " + String::num_int64( rule.line ) + ": this game has no " + rule.type );
			continue;
		}
		reaction->set_name( rule.type.substr( 2 ) + String::num_int64( rule.line ) );
		bool ok = true;
		Array keys = rule.properties.keys();
		for ( int64_t i = 0; i < keys.size(); ++i )
		{
			String key = keys[i];
			Variant value = rule.properties[key];
			if ( cue::IsPathProperty( key ) )
			{
				String path = value;
				bool anchored = path.begins_with( "^" ) || path.begins_with( "$" ) || path.begins_with( "@" );
				if ( path.begins_with( "/" ) )
				{
					m_problems.push_back( "line " + String::num_int64( rule.line ) + ": " + key + " " + path + ": not a path from the root" );
					ok = false;
					break;
				}
				if ( anchored == false && path.is_empty() == false )
				{
					int climbs = 0;
					PackedStringArray parts = path.split( "/" );
					for ( int64_t j = 0; j < parts.size() && parts[j] == ".."; ++j )
					{
						++climbs;
					}
					if ( climbs > depth )
					{
						m_problems.push_back( "line " + String::num_int64( rule.line ) + ": " + key + " " + path + " climbs out of the scene" );
						ok = false;
						break;
					}
					// From the reaction, under this node, to the node this one is under.
					path = path == "." ? String( "../.." ) : "../../" + path;
				}
				value = NodePath( path );
			}
			reaction->set( key, value );
		}
		if ( ok == false )
		{
			memdelete( reaction );
			continue;
		}
		add_child( reaction, false, INTERNAL_MODE_BACK );
	}
	for ( int64_t i = 0; i < m_problems.size(); ++i )
	{
		UtilityFunctions::push_warning( "CbRules ", SheetName(), ": ", m_problems[i] );
	}
}

PackedStringArray CbRules::_get_configuration_warnings() const
{
	PackedStringArray out;
	if ( m_sheet.is_null() )
	{
		out.push_back( "Give it a sheet (sheet: a .cbsheet file): the reactions of this scene, as lines of text." );
		return out;
	}
	if ( m_sheet->get_path().is_empty() || m_sheet->get_path().contains( "::" ) )
	{
		out.push_back( "Its sheet is kept inside the scene: save it as a .cbsheet file (the sheet's menu, Save As), so tools, the bake and the cooker read it." );
	}
	PackedStringArray errors;
	cue::ParseRules( m_sheet->get_text(), errors );
	for ( int64_t i = 0; i < errors.size(); ++i )
	{
		out.push_back( SheetName().get_file() + " " + errors[i] );
	}
	return out;
}

} // namespace cb::gd

