#include "cinderbox_body.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::gd
{

void CbBody::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_mass", "value" ), &CbBody::set_mass );
	ClassDB::bind_method( D_METHOD( "get_mass" ), &CbBody::get_mass );
	ClassDB::bind_method( D_METHOD( "set_density", "value" ), &CbBody::set_density );
	ClassDB::bind_method( D_METHOD( "get_density" ), &CbBody::get_density );
	ClassDB::bind_method( D_METHOD( "set_friction", "value" ), &CbBody::set_friction );
	ClassDB::bind_method( D_METHOD( "get_friction" ), &CbBody::get_friction );
	ClassDB::bind_method( D_METHOD( "set_bounce", "value" ), &CbBody::set_bounce );
	ClassDB::bind_method( D_METHOD( "get_bounce" ), &CbBody::get_bounce );
	ADD_GROUP( "Body", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "mass", PROPERTY_HINT_RANGE, "0,1000,0.01,or_greater,suffix:kg" ), "set_mass", "get_mass" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0.001,1000,0.001,or_greater,suffix:kg/m3" ), "set_density",
				  "get_density" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "friction", PROPERTY_HINT_RANGE, "0,2,0.01,or_greater" ), "set_friction", "get_friction" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "bounce", PROPERTY_HINT_RANGE, "0,1,0.01" ), "set_bounce", "get_bounce" );
}

} // namespace cb::gd
