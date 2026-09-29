#include "cinderbox_item_look.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::gd
{

void CbItemLook::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_kind", "value" ), &CbItemLook::set_kind );
	ClassDB::bind_method( D_METHOD( "get_kind" ), &CbItemLook::get_kind );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "kind", PROPERTY_HINT_PLACEHOLDER_TEXT, "melee.bat" ), "set_kind", "get_kind" );
	ClassDB::bind_method( D_METHOD( "set_scene", "value" ), &CbItemLook::set_scene );
	ClassDB::bind_method( D_METHOD( "get_scene" ), &CbItemLook::get_scene );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "scene", PROPERTY_HINT_FILE, "*.tscn,*.scn" ), "set_scene", "get_scene" );
}

} // namespace cb::gd
