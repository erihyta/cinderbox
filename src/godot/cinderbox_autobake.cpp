#include "cinderbox_autobake.h"

#include "cinderbox_character.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace cb::gd
{

void CbAutoBakePlugin::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "on_scene_saved", "path" ), &CbAutoBakePlugin::on_scene_saved );
}

void CbAutoBakePlugin::_enter_tree()
{
	connect( "scene_saved", Callable( this, "on_scene_saved" ) );
}

void CbAutoBakePlugin::_exit_tree()
{
	disconnect( "scene_saved", Callable( this, "on_scene_saved" ) );
}

void CbAutoBakePlugin::on_scene_saved( const String& path )
{
	TypedArray<Node> roots = EditorInterface::get_singleton()->get_open_scene_roots();
	for ( int64_t i = 0; i < roots.size(); ++i )
	{
		auto* character = Object::cast_to<CbCharacter>( Object::cast_to<Node>( roots[i] ) );
		if ( character != nullptr && character->get_scene_file_path() == path )
		{
			character->bake();
		}
	}
}

} // namespace cb::gd
