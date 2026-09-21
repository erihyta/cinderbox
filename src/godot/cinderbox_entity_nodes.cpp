#include "cinderbox_entity_nodes.h"

#include "reflect.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::gd
{

namespace
{

// "None,Shape,Body,Material,..." for the component dropdown. Index 0 means nothing selected.
String ComponentHint()
{
	String hint = "None";
	for ( const ComponentDef& def : AuthorableComponents() )
	{
		hint += String( "," ) + def.name;
	}
	return hint;
}

const ComponentDef* DefAt( int index )
{
	const std::vector<ComponentDef>& registry = AuthorableComponents();
	if ( index <= 0 || size_t( index ) > registry.size() )
	{
		return nullptr;
	}
	return &registry[size_t( index ) - 1];
}

Variant DefaultValue( const FieldDef& field )
{
	switch ( field.type )
	{
		case FieldType::Vec3:
			return Vector3( field.defaults[0], field.defaults[1], field.defaults[2] );
		case FieldType::Bool:
			return field.defaults[0] != 0.0f;
		case FieldType::Int:
		case FieldType::Enum:
			return int64_t( field.defaults[0] );
		case FieldType::Float:
		default:
			return field.defaults[0];
	}
}

} // namespace

// --- CbComponent --------------------------------------------------------------------------------

void CbComponent::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_component", "index" ), &CbComponent::set_component );
	ClassDB::bind_method( D_METHOD( "get_component" ), &CbComponent::get_component );
	ClassDB::bind_method( D_METHOD( "get_values" ), &CbComponent::get_values );
	ClassDB::bind_method( D_METHOD( "component_name" ), &CbComponent::component_name );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "component", PROPERTY_HINT_ENUM, ComponentHint() ), "set_component", "get_component" );
}

void CbComponent::set_component( int index )
{
	if ( index == m_component )
	{
		return;
	}
	m_component = index;
	m_values.clear();
	// The inspector has to rebuild: a different component means different fields.
	notify_property_list_changed();
}

String CbComponent::component_name() const
{
	const ComponentDef* def = DefAt( m_component );
	return def != nullptr ? String( def->name ) : String();
}

Dictionary CbComponent::get_values() const
{
	Dictionary out;
	const ComponentDef* def = DefAt( m_component );
	if ( def == nullptr )
	{
		return out;
	}
	// Every field of a component the author added counts as authored, so the map carries all of
	// them and the simulation never has to guess which ones were meant.
	for ( const FieldDef& field : def->fields )
	{
		String name = field.name;
		out[name] = m_values.has( name ) ? m_values[name] : DefaultValue( field );
	}
	return out;
}

void CbComponent::_get_property_list( List<PropertyInfo>* list ) const
{
	const ComponentDef* def = DefAt( m_component );
	if ( def == nullptr || list == nullptr )
	{
		return;
	}
	for ( const FieldDef& field : def->fields )
	{
		PropertyInfo info;
		info.name = field.name;
		info.usage = PROPERTY_USAGE_DEFAULT;
		switch ( field.type )
		{
			case FieldType::Vec3:
				info.type = Variant::VECTOR3;
				break;
			case FieldType::Bool:
				info.type = Variant::BOOL;
				break;
			case FieldType::Enum:
				info.type = Variant::INT;
				info.hint = PROPERTY_HINT_ENUM;
				info.hint_string = field.enumNames != nullptr ? field.enumNames : "";
				break;
			case FieldType::Int:
				info.type = Variant::INT;
				info.hint = PROPERTY_HINT_RANGE;
				info.hint_string = String::num_int64( int64_t( field.min ) ) + "," + String::num_int64( int64_t( field.max ) ) + ",1";
				break;
			case FieldType::Float:
			default:
				info.type = Variant::FLOAT;
				info.hint = PROPERTY_HINT_RANGE;
				// The step matches the map's storage grid, so what is typed is what is baked.
				info.hint_string = String::num_real( field.min ) + "," + String::num_real( field.max ) + ",0.001,or_greater";
				break;
		}
		list->push_back( info );
	}
}

bool CbComponent::_set( const StringName& name, const Variant& value )
{
	const ComponentDef* def = DefAt( m_component );
	if ( def == nullptr )
	{
		return false;
	}
	if ( def->Field( Fnv32( String( name ).utf8().get_data() ) ) == nullptr )
	{
		return false;
	}
	m_values[String( name )] = value;
	return true;
}

bool CbComponent::_get( const StringName& name, Variant& out ) const
{
	const ComponentDef* def = DefAt( m_component );
	if ( def == nullptr )
	{
		return false;
	}
	const FieldDef* field = def->Field( Fnv32( String( name ).utf8().get_data() ) );
	if ( field == nullptr )
	{
		return false;
	}
	String key = String( name );
	out = m_values.has( key ) ? m_values[key] : DefaultValue( *field );
	return true;
}

bool CbComponent::_property_can_revert( const StringName& name ) const
{
	const ComponentDef* def = DefAt( m_component );
	return def != nullptr && def->Field( Fnv32( String( name ).utf8().get_data() ) ) != nullptr;
}

bool CbComponent::_property_get_revert( const StringName& name, Variant& out ) const
{
	const ComponentDef* def = DefAt( m_component );
	if ( def == nullptr )
	{
		return false;
	}
	const FieldDef* field = def->Field( Fnv32( String( name ).utf8().get_data() ) );
	if ( field == nullptr )
	{
		return false;
	}
	out = DefaultValue( *field );
	return true;
}

// --- CbTemplate ---------------------------------------------------------------------------------

void CbTemplate::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_template_name", "name" ), &CbTemplate::set_template_name );
	ClassDB::bind_method( D_METHOD( "get_template_name" ), &CbTemplate::get_template_name );
	ClassDB::bind_method( D_METHOD( "set_visual", "visual" ), &CbTemplate::set_visual );
	ClassDB::bind_method( D_METHOD( "get_visual" ), &CbTemplate::get_visual );
	ClassDB::bind_method( D_METHOD( "set_spawnable", "value" ), &CbTemplate::set_spawnable );
	ClassDB::bind_method( D_METHOD( "get_spawnable" ), &CbTemplate::get_spawnable );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "template_name" ), "set_template_name", "get_template_name" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "visual" ), "set_visual", "get_visual" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "spawnable" ), "set_spawnable", "get_spawnable" );
}

// --- CbEntity -----------------------------------------------------------------------------------

void CbEntity::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_template_name", "name" ), &CbEntity::set_template_name );
	ClassDB::bind_method( D_METHOD( "get_template_name" ), &CbEntity::get_template_name );
	ClassDB::bind_method( D_METHOD( "set_visual", "visual" ), &CbEntity::set_visual );
	ClassDB::bind_method( D_METHOD( "get_visual" ), &CbEntity::get_visual );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "template_name" ), "set_template_name", "get_template_name" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "visual" ), "set_visual", "get_visual" );
}

} // namespace cb::gd
