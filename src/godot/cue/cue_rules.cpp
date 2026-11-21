#include "cue_rules.h"

#include "cue_prediction.h"
#include "cue_reaction.h"

#include "sheet_rules.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
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
	ClassDB::bind_method( D_METHOD( "set_file", "value" ), &CbRules::set_file );
	ClassDB::bind_method( D_METHOD( "get_file" ), &CbRules::get_file );
	ClassDB::bind_method( D_METHOD( "get_reaction_count" ), &CbRules::get_reaction_count );
	ClassDB::bind_method( D_METHOD( "get_problems" ), &CbRules::get_problems );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "file", PROPERTY_HINT_FILE, "*.rules" ), "set_file", "get_file" );
}

void CbRules::set_file( const String& v )
{
	m_file = v.strip_edges();
	if ( is_inside_tree() )
	{
		Clear();
		Build();
	}
	update_configuration_warnings();
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
	if ( m_file.is_empty() )
	{
		return;
	}
	// Only a file of the pack itself.
	if ( m_file.begins_with( "res://" ) == false || m_file.contains( ".." ) )
	{
		m_problems.push_back( "the file is not one of this project's (res://...)" );
		UtilityFunctions::push_warning( "CbRules ", get_path(), ": ", m_problems[0] );
		return;
	}
	Ref<FileAccess> file = FileAccess::open( m_file, FileAccess::READ );
	if ( file.is_null() )
	{
		m_problems.push_back( "cannot read " + m_file );
		UtilityFunctions::push_warning( "CbRules ", get_path(), ": ", m_problems[0] );
		return;
	}
	std::vector<cue::Rule> rules = cue::ParseRules( file->get_as_text(), m_problems );
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
		UtilityFunctions::push_warning( "CbRules ", m_file, ": ", m_problems[i] );
	}
}

PackedStringArray CbRules::_get_configuration_warnings() const
{
	PackedStringArray out;
	if ( m_file.is_empty() )
	{
		out.push_back( "Give it a rules file (file): the reactions of this scene, as lines of text." );
		return out;
	}
	if ( FileAccess::file_exists( m_file ) == false )
	{
		out.push_back( "No such file: " + m_file );
		return out;
	}
	PackedStringArray errors;
	cue::ParseRules( FileAccess::get_file_as_string( m_file ), errors );
	for ( int64_t i = 0; i < errors.size(); ++i )
	{
		out.push_back( m_file.get_file() + " " + errors[i] );
	}
	return out;
}

} // namespace cb::gd

