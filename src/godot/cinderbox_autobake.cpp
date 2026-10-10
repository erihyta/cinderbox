#include "cinderbox_autobake.h"

#include "cinderbox_character.h"
#include "cinderbox_item_look.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_paths.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace cb::gd
{

namespace
{

// What a mod's look, motions, characters and maps are made of, in the order a modder meets them.
const char* const kOfferedNodes[] = {
	"CbReaction",	"CbPrediction", "CbItem",	"CbLinkLook", "CbMotionSet",   "CbMotion",	"CbProbe", "CbLaunch", "CbImpulse", "CbForce", "CbLink",	"CbFieldLabel",
	"CbFieldBinding", "CbList", "CbKey", "CbClick", "CbPromptLabel", "CbCharacter", "CbAnimPack",	   "CbHitbox",	"CbSocket",
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

void CbAutoBakePlugin::OfferTest()
{
	// Only where it can work: this project is <checkout>/server_mods/<mod>/client, and the checkout
	// has the script (PowerShell: Windows).
	if ( OS::get_singleton()->get_name() != "Windows" )
	{
		return;
	}
	String project = ProjectSettings::get_singleton()->globalize_path( "res://" ).replace( "\\", "/" ).trim_suffix( "/" );
	PackedStringArray parts = project.split( "/" );
	int64_t n = parts.size();
	if ( n < 4 || parts[n - 1] != "client" || parts[n - 3] != "server_mods" )
	{
		return;
	}
	String root = String( "/" ).join( parts.slice( 0, n - 3 ) );
	String script = root.path_join( "tools/test_mod.ps1" );
	if ( FileAccess::file_exists( script ) == false )
	{
		return;
	}
	m_testScript = script;
	m_testMod = parts[n - 2];
	m_testButton = memnew( Button );
	m_testButton->set_text( "Test mod" );
	m_testButton->set_tooltip_text( "Saves the open scenes, then: builds the server, publishes this mod's look (" + m_testMod +
									"), and starts the game on a server of its own with it. Closing the game stops the server.\n"
									"(tools\\test_mod.ps1 -Mod " + m_testMod + ", in a window of its own.)" );
	m_testButton->connect( "pressed", Callable( this, "on_test_mod" ) );
	add_control_to_container( CONTAINER_TOOLBAR, m_testButton );
}

void CbAutoBakePlugin::on_test_mod()
{
	if ( m_testScript.is_empty() )
	{
		return;
	}
	// What is tried is what is on the screen.
	EditorInterface::get_singleton()->save_all_scenes();
	// In a console of its own, which stays open when a step fails (a name nobody declares, a build
	// error), so what it said can be read.
	String command = "& '" + m_testScript.replace( "/", "\\" ) + "' -Mod " + m_testMod +
					 "; if ( $LASTEXITCODE -ne 0 ) { Write-Host ''; Read-Host 'It stopped (see above). Press Enter to close' }";
	PackedStringArray args;
	args.push_back( "-NoProfile" );
	args.push_back( "-ExecutionPolicy" );
	args.push_back( "Bypass" );
	args.push_back( "-Command" );
	args.push_back( command );
	OS::get_singleton()->create_process( "powershell.exe", args, true );
}

void CbAutoBakePlugin::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "on_scene_saved", "path" ), &CbAutoBakePlugin::on_scene_saved );
	ClassDB::bind_method( D_METHOD( "on_test_mod" ), &CbAutoBakePlugin::on_test_mod );
}

void CbAutoBakePlugin::_enter_tree()
{
	connect( "scene_saved", Callable( this, "on_scene_saved" ) );
	m_motionInspector.instantiate();
	add_inspector_plugin( m_motionInspector );
	OfferNodes();
	OfferTest();
}

void CbAutoBakePlugin::_exit_tree()
{
	disconnect( "scene_saved", Callable( this, "on_scene_saved" ) );
	remove_inspector_plugin( m_motionInspector );
	m_motionInspector.unref();
	if ( m_testButton != nullptr )
	{
		remove_control_from_container( CONTAINER_TOOLBAR, m_testButton );
		m_testButton->queue_free();
		m_testButton = nullptr;
	}
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
		auto* item = Object::cast_to<CbItem>( Object::cast_to<Node>( roots[i] ) );
		if ( item != nullptr && item->get_scene_file_path() == path )
		{
			item->bake_to_project();
		}
		auto* motions = Object::cast_to<CbMotionSet>( Object::cast_to<Node>( roots[i] ) );
		if ( motions != nullptr && motions->get_scene_file_path() == path )
		{
			motions->bake_to_project();
		}
	}
}

} // namespace cb::gd
