#include "cinderbox_autobake.h"

#include "cinderbox_character.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_paths.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace cb::gd
{

namespace
{

// What a mod's look, motions, characters and maps are made of, in the order a modder meets them.
const char* const kOfferedNodes[] = {
	"CbReaction",	"CbPrediction", "CbItemLook",  "CbItemBody",	"CbGrip",	  "CbMotionSet",   "CbMotion",	"CbFieldLabel",
	"CbFieldBinding", "CbEventFeed",	"CbScoreboard", "CbPromptLabel", "CbCharacter", "CbAnimPack",	   "CbHitbox",	"CbSocket",
	"CbStatic",		"CbProp",		"CbSpawn",	   "CbTemplate",	"CbComponent", "CbEntity",
};

PackedStringArray ReadLines( const String& path )
{
	PackedStringArray lines;
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
	while ( file.is_valid() && file->eof_reached() == false )
	{
		String line = file->get_line().strip_edges();
		if ( line.is_empty() == false )
		{
			lines.push_back( line );
		}
	}
	return lines;
}

void WriteLines( const String& path, const PackedStringArray& lines )
{
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::WRITE );
	for ( int64_t i = 0; file.is_valid() && i < lines.size(); ++i )
	{
		file->store_line( lines[i] );
	}
}

} // namespace

void CbAutoBakePlugin::OfferNodes()
{
	EditorPaths* paths = EditorInterface::get_singleton()->get_editor_paths();
	if ( paths == nullptr )
	{
		return;
	}
	// The dialog's own file: one class name a line (favorites.<base type>; nodes are added under Node).
	String favoritesPath = paths->get_project_settings_dir().path_join( "favorites.Node" );
	// The nodes this project was already given: one the user took out again is not put back.
	String offeredPath = paths->get_project_settings_dir().path_join( "cinderbox_nodes.offered" );
	PackedStringArray favorites = ReadLines( favoritesPath );
	PackedStringArray offered = ReadLines( offeredPath );
	bool changedFavorites = false, changedOffered = false;
	for ( const char* name : kOfferedNodes )
	{
		if ( offered.has( name ) || ClassDB::class_exists( name ) == false )
		{
			continue;
		}
		offered.push_back( name );
		changedOffered = true;
		// A favorite's line starts with the class name.
		bool there = false;
		for ( int64_t i = 0; i < favorites.size(); ++i )
		{
			there |= favorites[i].get_slicec( ' ', 0 ) == String( name );
		}
		if ( there == false )
		{
			favorites.push_back( name );
			changedFavorites = true;
		}
	}
	if ( changedFavorites )
	{
		WriteLines( favoritesPath, favorites );
	}
	if ( changedOffered )
	{
		WriteLines( offeredPath, offered );
	}
}

void CbAutoBakePlugin::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "on_scene_saved", "path" ), &CbAutoBakePlugin::on_scene_saved );
}

void CbAutoBakePlugin::_enter_tree()
{
	connect( "scene_saved", Callable( this, "on_scene_saved" ) );
	m_motionInspector.instantiate();
	add_inspector_plugin( m_motionInspector );
	OfferNodes();
}

void CbAutoBakePlugin::_exit_tree()
{
	disconnect( "scene_saved", Callable( this, "on_scene_saved" ) );
	remove_inspector_plugin( m_motionInspector );
	m_motionInspector.unref();
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
		auto* motions = Object::cast_to<CbMotionSet>( Object::cast_to<Node>( roots[i] ) );
		if ( motions != nullptr && motions->get_scene_file_path() == path )
		{
			motions->bake_to_project();
		}
	}
}

} // namespace cb::gd
