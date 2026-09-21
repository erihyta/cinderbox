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

	ClassDB::bind_method( D_METHOD( "set_cooldown", "value" ), &CbEffect::set_cooldown );
	ClassDB::bind_method( D_METHOD( "get_cooldown" ), &CbEffect::get_cooldown );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "cooldown", PROPERTY_HINT_RANGE, "0,10,0.01" ), "set_cooldown", "get_cooldown" );

	ClassDB::bind_method( D_METHOD( "set_sound", "value" ), &CbEffect::set_sound );
	ClassDB::bind_method( D_METHOD( "get_sound" ), &CbEffect::get_sound );
	ClassDB::bind_method( D_METHOD( "set_volume_db", "value" ), &CbEffect::set_volume_db );
	ClassDB::bind_method( D_METHOD( "get_volume_db" ), &CbEffect::get_volume_db );
	ClassDB::bind_method( D_METHOD( "set_pitch_scale", "value" ), &CbEffect::set_pitch_scale );
	ClassDB::bind_method( D_METHOD( "get_pitch_scale" ), &CbEffect::get_pitch_scale );
	ClassDB::bind_method( D_METHOD( "set_pitch_jitter", "value" ), &CbEffect::set_pitch_jitter );
	ClassDB::bind_method( D_METHOD( "get_pitch_jitter" ), &CbEffect::get_pitch_jitter );
	ClassDB::bind_method( D_METHOD( "set_bus", "value" ), &CbEffect::set_bus );
	ClassDB::bind_method( D_METHOD( "get_bus" ), &CbEffect::get_bus );
	ClassDB::bind_method( D_METHOD( "set_max_distance", "value" ), &CbEffect::set_max_distance );
	ClassDB::bind_method( D_METHOD( "get_max_distance" ), &CbEffect::get_max_distance );

	ADD_GROUP( "Sound", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "sound", PROPERTY_HINT_FILE, "*.wav,*.ogg,*.mp3" ), "set_sound", "get_sound" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "volume_db", PROPERTY_HINT_RANGE, "-60,12,0.1" ), "set_volume_db",
				  "get_volume_db" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "pitch_scale", PROPERTY_HINT_RANGE, "0.1,4,0.01" ), "set_pitch_scale",
				  "get_pitch_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "pitch_jitter", PROPERTY_HINT_RANGE, "0,2,0.01" ), "set_pitch_jitter",
				  "get_pitch_jitter" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "bus" ), "set_bus", "get_bus" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "max_distance", PROPERTY_HINT_RANGE, "0,200,0.5" ), "set_max_distance",
				  "get_max_distance" );

	ClassDB::bind_method( D_METHOD( "set_shake", "value" ), &CbEffect::set_shake );
	ClassDB::bind_method( D_METHOD( "get_shake" ), &CbEffect::get_shake );
	ClassDB::bind_method( D_METHOD( "set_shake_time", "value" ), &CbEffect::set_shake_time );
	ClassDB::bind_method( D_METHOD( "get_shake_time" ), &CbEffect::get_shake_time );
	ClassDB::bind_method( D_METHOD( "set_flash_color", "value" ), &CbEffect::set_flash_color );
	ClassDB::bind_method( D_METHOD( "get_flash_color" ), &CbEffect::get_flash_color );
	ClassDB::bind_method( D_METHOD( "set_flash_time", "value" ), &CbEffect::set_flash_time );
	ClassDB::bind_method( D_METHOD( "get_flash_time" ), &CbEffect::get_flash_time );

	ADD_GROUP( "Screen", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "shake", PROPERTY_HINT_RANGE, "0,2,0.005" ), "set_shake", "get_shake" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "shake_time", PROPERTY_HINT_RANGE, "0.05,3,0.01" ), "set_shake_time",
				  "get_shake_time" );
	ADD_PROPERTY( PropertyInfo( Variant::COLOR, "flash_color" ), "set_flash_color", "get_flash_color" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "flash_time", PROPERTY_HINT_RANGE, "0.02,3,0.01" ), "set_flash_time",
				  "get_flash_time" );

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
