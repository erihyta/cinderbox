#include "cinderbox_effects.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::gd
{

void CbEffect::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_event", "value" ), &CbEffect::set_event );
	ClassDB::bind_method( D_METHOD( "get_event" ), &CbEffect::get_event );
	ClassDB::bind_method( D_METHOD( "set_template_name", "value" ), &CbEffect::set_template_name );
	ClassDB::bind_method( D_METHOD( "get_template_name" ), &CbEffect::get_template_name );
	ClassDB::bind_method( D_METHOD( "set_kind", "value" ), &CbEffect::set_kind );
	ClassDB::bind_method( D_METHOD( "get_kind" ), &CbEffect::get_kind );
	ClassDB::bind_method( D_METHOD( "set_scene", "value" ), &CbEffect::set_scene );
	ClassDB::bind_method( D_METHOD( "get_scene" ), &CbEffect::get_scene );
	ClassDB::bind_method( D_METHOD( "set_offset", "value" ), &CbEffect::set_offset );
	ClassDB::bind_method( D_METHOD( "get_offset" ), &CbEffect::get_offset );
	ClassDB::bind_method( D_METHOD( "set_lifetime", "value" ), &CbEffect::set_lifetime );
	ClassDB::bind_method( D_METHOD( "get_lifetime" ), &CbEffect::get_lifetime );
	ClassDB::bind_method( D_METHOD( "set_follow", "value" ), &CbEffect::set_follow );
	ClassDB::bind_method( D_METHOD( "get_follow" ), &CbEffect::get_follow );
	ClassDB::bind_method( D_METHOD( "set_who", "value" ), &CbEffect::set_who );
	ClassDB::bind_method( D_METHOD( "get_who" ), &CbEffect::get_who );

	ADD_PROPERTY( PropertyInfo( Variant::INT, "event", PROPERTY_HINT_ENUM, "Spawned,Destroying,Jumped,Landed" ), "set_event",
				  "get_event" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "template_name" ), "set_template_name", "get_template_name" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "kind", PROPERTY_HINT_ENUM_SUGGESTION, "any,prop,player,static" ), "set_kind",
				  "get_kind" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "scene", PROPERTY_HINT_FILE, "*.tscn,*.scn" ), "set_scene", "get_scene" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR3, "offset" ), "set_offset", "get_offset" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "lifetime", PROPERTY_HINT_RANGE, "0.1,60,0.1" ), "set_lifetime", "get_lifetime" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "follow" ), "set_follow", "get_follow" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "who", PROPERTY_HINT_ENUM, "Anyone,Local player,Other players" ), "set_who",
				  "get_who" );

	BIND_ENUM_CONSTANT( EVENT_SPAWNED );
	BIND_ENUM_CONSTANT( EVENT_DESTROYING );
	BIND_ENUM_CONSTANT( EVENT_JUMPED );
	BIND_ENUM_CONSTANT( EVENT_LANDED );
	BIND_ENUM_CONSTANT( WHO_ANYONE );
	BIND_ENUM_CONSTANT( WHO_LOCAL );
	BIND_ENUM_CONSTANT( WHO_REMOTE );
}

void CbEffectTable::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_effects", "effects" ), &CbEffectTable::set_effects );
	ClassDB::bind_method( D_METHOD( "get_effects" ), &CbEffectTable::get_effects );
	ADD_PROPERTY( PropertyInfo( Variant::ARRAY, "effects", PROPERTY_HINT_ARRAY_TYPE,
								String::num_int64( Variant::OBJECT ) + "/" + String::num_int64( PROPERTY_HINT_RESOURCE_TYPE ) +
									":CbEffect" ),
				  "set_effects", "get_effects" );
}

} // namespace cb::gd
