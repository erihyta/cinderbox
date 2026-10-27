#include "cinderbox_autobake.h"

#include "cinderbox_character.h"
#include "cinderbox_item_look.h"
#include "cinderbox_rules_editor.h"
#include "pose.h" // CinderboxSkeleton holds a PoseEvaluator
#include "cinderbox_skeleton.h"

#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/sub_viewport_container.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/editor_paths.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/script_editor.hpp>
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
	"CbReaction",	"CbPrediction", "CbItem",	"CbLinkLook", "CbMotionSet",	 "CbFieldLabel", "CbFieldBinding", "CbList",
	"CbKey",		"CbClick",		"CbPromptLabel", "CbCharacter", "CbAnimPack",	   "CbHitbox",	"CbSocket",	   "CbStatic",
	"CbProp",		"CbBreakable", "CbSpawn",	"CbCharacterBody", "CbTemplate", "CbComponent", "CbEntity",
};

// Offered before M243, and taken out of the favorites again: the motion nodes, kept but not
// maintained (a motion is a sheet's block now: docs/rules-language.md).
const char* const kWithdrawnNodes[] = { "CbMotion", "CbProbe", "CbLaunch", "CbImpulse", "CbForce", "CbLink", "CbMove" };

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
	// Withdrawn: out of the favorites, once (the offered file keeps them, so they are not put back).
	for ( const char* name : kWithdrawnNodes )
	{
		const String withdrawn = String( name ) + ".withdrawn";
		if ( offered.has( withdrawn ) )
		{
			continue;
		}
		offered.push_back( withdrawn );
		changedOffered = true;
		for ( int64_t i = favorites.size() - 1; i >= 0; --i )
		{
			if ( favorites[i].get_slicec( ' ', 0 ) == String( name ) )
			{
				favorites.remove_at( i );
				changedFavorites = true;
			}
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

void CbAutoBakePlugin::MakeViewPanel()
{
	auto* box = memnew( VBoxContainer );
	box->set_custom_minimum_size( Vector2( 420, 0 ) );
	auto* title = memnew( Label );
	title->set_text( "First person: what the holder sees (Preview > Hold)" );
	title->set_tooltip_text( "The arms and the item as the game draws them for the holder's own view: the same pose, the item's view_offset, "
							 "view_rotation, view_fov and hands. Change them in the inspector and watch. The pose is the pack's starting state." );
	box->add_child( title );
	auto* frame = memnew( SubViewportContainer );
	frame->set_stretch( true );
	frame->set_custom_minimum_size( Vector2( 420, 236 ) ); // 16 : 9
	box->add_child( frame );
	m_viewPort = memnew( SubViewport );
	m_viewPort->set_use_own_world_3d( true );
	m_viewPort->set_update_mode( SubViewport::UPDATE_WHEN_VISIBLE );
	frame->add_child( m_viewPort );
	m_viewCamera = memnew( Camera3D );
	m_viewCamera->set_near( 0.01 );
	Ref<Environment> env;
	env.instantiate();
	env->set_background( Environment::BG_COLOR );
	env->set_bg_color( Color( 0.55f, 0.62f, 0.72f ) );
	env->set_ambient_source( Environment::AMBIENT_SOURCE_COLOR );
	env->set_ambient_light_color( Color( 0.8f, 0.8f, 0.85f ) );
	env->set_ambient_light_energy( 0.6f );
	m_viewCamera->set_environment( env );
	m_viewPort->add_child( m_viewCamera );
	auto* sun = memnew( DirectionalLight3D );
	sun->set_rotation_degrees( Vector3( -50, 30, 0 ) );
	m_viewPort->add_child( sun );
	auto* arms = memnew( CinderboxSkeleton );
	arms->set_use_slot_color( false );
	arms->set_body_color( Color( 0.75f, 0.78f, 0.82f ) );
	m_viewArms = arms;
	m_viewPort->add_child( arms );
	m_viewHeld = memnew( Node3D );
	m_viewPort->add_child( m_viewHeld );
	m_viewPanel = box;
	m_viewPanel->set_visible( false );
	add_control_to_container( CONTAINER_SPATIAL_EDITOR_SIDE_RIGHT, m_viewPanel );
}

void CbAutoBakePlugin::_process( double )
{
	if ( m_viewPanel == nullptr )
	{
		return;
	}
	// The item being edited, with its Hold preview on: its holder's view.
	auto* item = Object::cast_to<CbItem>( EditorInterface::get_singleton()->get_edited_scene_root() );
	Dictionary seen = item != nullptr && item->get_preview_hold() ? item->preview_view( m_viewArms, m_viewHeld ) : Dictionary();
	bool on = seen.has( "eye" );
	if ( m_viewPanel->is_visible() != on )
	{
		m_viewPanel->set_visible( on );
	}
	if ( on )
	{
		m_viewCamera->set_transform( seen["eye"] );
		m_viewCamera->set_fov( float( double( seen["fov"] ) ) );
	}
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
	// .rules files: coloured, names offered while typing, mistakes marked on their lines.
	if ( ScriptEditor* scripts = EditorInterface::get_singleton()->get_script_editor() )
	{
		m_rulesHighlighter = Ref<CbRulesHighlighter>( memnew( CbRulesHighlighter ) );
		scripts->register_syntax_highlighter( m_rulesHighlighter );
	}
	OfferNodes();
	OfferTest();
	MakeViewPanel();
	// A rules sheet (CbRules) is text: the editor opens .rules files in its script editor.
	Ref<EditorSettings> settings = EditorInterface::get_singleton()->get_editor_settings();
	const String key = "docks/filesystem/textfile_extensions";
	String extensions = settings.is_valid() && settings->has_setting( key ) ? String( settings->get_setting( key ) ) : String();
	if ( settings.is_valid() && extensions.split( "," ).has( "rules" ) == false )
	{
		settings->set_setting( key, extensions.is_empty() ? String( "rules" ) : extensions + ",rules" );
	}
	set_process( true );
}

void CbAutoBakePlugin::_exit_tree()
{
	disconnect( "scene_saved", Callable( this, "on_scene_saved" ) );
	remove_inspector_plugin( m_motionInspector );
	m_motionInspector.unref();
	if ( ScriptEditor* scripts = EditorInterface::get_singleton()->get_script_editor(); scripts != nullptr && m_rulesHighlighter.is_valid() )
	{
		scripts->unregister_syntax_highlighter( m_rulesHighlighter );
	}
	m_rulesHighlighter.unref();
	if ( m_viewPanel != nullptr )
	{
		remove_control_from_container( CONTAINER_SPATIAL_EDITOR_SIDE_RIGHT, m_viewPanel );
		m_viewPanel->queue_free();
		m_viewPanel = nullptr;
	}
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
