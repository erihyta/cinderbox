#include "cue_preview.h"

#include "cue_paths.h"
#include "cue_reaction.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/capsule_mesh.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/h_separator.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/sub_viewport_container.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace cb::gd
{

namespace
{

const char* kWhoNames[] = { "player_0", "player_1", "the item", "the world", "nobody" };
const char* kBuiltInCues[] = { "spawned", "destroying", "jumped", "landed", "footstep", "impact" };

void FindReactions( Node* node, std::vector<CbReaction*>& out )
{
	if ( auto* reaction = Object::cast_to<CbReaction>( node ) )
	{
		out.push_back( reaction );
	}
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		FindReactions( node->get_child( i ), out );
	}
}

template <typename T>
bool Contains( Node* node )
{
	if ( Object::cast_to<T>( node ) != nullptr )
	{
		return true;
	}
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		if ( Contains<T>( node->get_child( i ) ) )
		{
			return true;
		}
	}
	return false;
}

// The state name a condition reads: "!^^:combat.dead" -> "combat.dead"; "" for is_local and event.*.
String NameIn( const String& condition )
{
	cue::Condition parsed;
	if ( cue::ParseCondition( condition, parsed, nullptr ) == false )
	{
		return String();
	}
	String test = parsed.test;
	for ( const char* prefix : { "!?", "!", "?" } )
	{
		if ( test.begins_with( prefix ) )
		{
			test = test.substr( String( prefix ).length() );
			break;
		}
	}
	for ( int64_t i = 0; i < test.length(); ++i )
	{
		char32_t c = test[i];
		if ( c == '=' || c == '!' || c == '<' || c == '>' )
		{
			test = test.substr( 0, i );
			break;
		}
	}
	test = test.strip_edges();
	return test == "is_local" || test.begins_with( "event." ) ? String() : test;
}

Label* MakeLabel( const String& text )
{
	auto* label = memnew( Label );
	label->set_text( text );
	return label;
}

OptionButton* MakeWho( std::initializer_list<int> whos, int selected )
{
	auto* button = memnew( OptionButton );
	for ( int who : whos )
	{
		button->add_item( kWhoNames[who], who );
	}
	button->select( std::max( 0, button->get_item_index( selected ) ) );
	return button;
}

} // namespace

void CbCuePreviewDock::Build()
{
	set_name( "Cue Preview" );
	set_custom_minimum_size( Vector2( 0, 280 ) );

	// The stage: a viewport of its own, with a flash overlay for screen effects.
	auto* view = memnew( Control );
	view->set_h_size_flags( SIZE_EXPAND_FILL );
	view->set_custom_minimum_size( Vector2( 420, 260 ) );
	add_child( view );
	auto* container = memnew( SubViewportContainer );
	container->set_stretch( true );
	container->set_anchors_preset( PRESET_FULL_RECT );
	view->add_child( container );
	m_viewport = memnew( SubViewport );
	m_viewport->set_update_mode( SubViewport::UPDATE_ALWAYS );
	m_viewport->set_use_own_world_3d( true );
	container->add_child( m_viewport );
	m_flash = memnew( ColorRect );
	m_flash->set_anchors_preset( PRESET_FULL_RECT );
	m_flash->set_mouse_filter( MOUSE_FILTER_IGNORE );
	m_flash->set_color( Color( 1, 1, 1, 0 ) );
	view->add_child( m_flash );

	m_stage = memnew( Node3D );
	m_viewport->add_child( m_stage );
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_background( Environment::BG_COLOR );
	environment->set_bg_color( Color( 0.23, 0.25, 0.3 ) );
	environment->set_ambient_source( Environment::AMBIENT_SOURCE_COLOR );
	environment->set_ambient_light_color( Color( 0.6, 0.62, 0.68 ) );
	auto* world = memnew( WorldEnvironment );
	world->set_environment( environment );
	m_stage->add_child( world );
	auto* light = memnew( DirectionalLight3D );
	light->set_rotation_degrees( Vector3( -50, 35, 0 ) );
	light->set_shadow( true );
	m_stage->add_child( light );
	auto* floor = memnew( MeshInstance3D );
	Ref<PlaneMesh> plane;
	plane.instantiate();
	plane->set_size( Vector2( 12, 12 ) );
	floor->set_mesh( plane );
	m_stage->add_child( floor );
	m_camera = memnew( Camera3D );
	m_stage->add_child( m_camera );
	m_camera->look_at_from_position( Vector3( 2.6, 1.75, 0.9 ), Vector3( 0, 1.05, 0 ) );
	m_cameraHome = m_camera->get_transform();
	m_camera->set_current( true );

	// The controls.
	auto* scroll = memnew( ScrollContainer );
	scroll->set_custom_minimum_size( Vector2( 330, 0 ) );
	scroll->set_horizontal_scroll_mode( ScrollContainer::SCROLL_MODE_DISABLED );
	add_child( scroll );
	auto* column = memnew( VBoxContainer );
	column->set_h_size_flags( SIZE_EXPAND_FILL );
	scroll->add_child( column );

	auto* top = memnew( HBoxContainer );
	column->add_child( top );
	m_title = MakeLabel( "No scene" );
	m_title->set_h_size_flags( SIZE_EXPAND_FILL );
	m_title->set_clip_text( true );
	top->add_child( m_title );
	m_mode = memnew( OptionButton );
	m_mode->add_item( "Auto", MODE_AUTO );
	m_mode->add_item( "Held item", MODE_ITEM );
	m_mode->add_item( "Character", MODE_CHARACTER );
	m_mode->add_item( "World", MODE_WORLD );
	m_mode->set_tooltip_text( "How the scene is put on the stage: in player_0's right hand, as player_0, or under the World." );
	m_mode->connect( "item_selected", callable_mp( this, &CbCuePreviewDock::OnReload ).unbind( 1 ) );
	top->add_child( m_mode );
	auto* reload = memnew( Button );
	reload->set_text( "Reload" );
	reload->set_tooltip_text( "Copy the edited scene onto the stage again (unsaved edits included)." );
	reload->connect( "pressed", callable_mp( this, &CbCuePreviewDock::OnReload ) );
	top->add_child( reload );

	column->add_child( memnew( HSeparator ) );
	auto* cueRow = memnew( HBoxContainer );
	column->add_child( cueRow );
	cueRow->add_child( MakeLabel( "Cue" ) );
	m_cue = memnew( LineEdit );
	m_cue->set_placeholder( "melee.hit" );
	m_cue->set_h_size_flags( SIZE_EXPAND_FILL );
	cueRow->add_child( m_cue );
	m_cueNames = memnew( OptionButton );
	m_cueNames->set_tooltip_text( "Cues this scene's reactions listen for, and the game's own." );
	m_cueNames->connect( "item_selected", callable_mp( this, &CbCuePreviewDock::OnCuePicked ) );
	cueRow->add_child( m_cueNames );

	auto* whoRow = memnew( HBoxContainer );
	column->add_child( whoRow );
	whoRow->add_child( MakeLabel( "$at" ) );
	m_at = MakeWho( { WHO_PLAYER0, WHO_PLAYER1, WHO_ITEM }, WHO_PLAYER0 );
	whoRow->add_child( m_at );
	whoRow->add_child( MakeLabel( "$other" ) );
	m_other = MakeWho( { WHO_PLAYER1, WHO_PLAYER0, WHO_ITEM, WHO_NONE }, WHO_PLAYER1 );
	whoRow->add_child( m_other );

	auto* argRow = memnew( HBoxContainer );
	column->add_child( argRow );
	argRow->add_child( MakeLabel( "value" ) );
	m_value = memnew( SpinBox );
	m_value->set_min( -100000 );
	m_value->set_max( 100000 );
	m_value->set_value( 25 );
	argRow->add_child( m_value );
	argRow->add_child( MakeLabel( "strength" ) );
	m_strength = memnew( SpinBox );
	m_strength->set_max( 1000 );
	m_strength->set_step( 0.1 );
	m_strength->set_value( 5 );
	argRow->add_child( m_strength );
	auto* fire = memnew( Button );
	fire->set_text( "Fire cue" );
	fire->set_tooltip_text( "The cue's point and end are $other's chest (or in front of $at)." );
	fire->connect( "pressed", callable_mp( this, &CbCuePreviewDock::OnFire ) );
	column->add_child( fire );

	column->add_child( memnew( HSeparator ) );
	auto* stateRow = memnew( HBoxContainer );
	column->add_child( stateRow );
	stateRow->add_child( MakeLabel( "State of" ) );
	m_stateEntity = MakeWho( { WHO_PLAYER0, WHO_PLAYER1, WHO_ITEM, WHO_WORLD }, WHO_ITEM );
	m_stateEntity->connect( "item_selected", callable_mp( this, &CbCuePreviewDock::OnStateEntity ) );
	stateRow->add_child( m_stateEntity );
	stateRow->add_child( MakeLabel( "local" ) );
	m_local = MakeWho( { WHO_PLAYER0, WHO_PLAYER1, WHO_NONE }, WHO_PLAYER0 );
	m_local->connect( "item_selected", callable_mp( this, &CbCuePreviewDock::OnLocal ) );
	stateRow->add_child( m_local );
	m_stateRows = memnew( VBoxContainer );
	column->add_child( m_stateRows );

	column->add_child( memnew( HSeparator ) );
	m_log = MakeLabel( "" );
	m_log->set_autowrap_mode( TextServer::AUTOWRAP_WORD_SMART );
	column->add_child( m_log );
}

void CbCuePreviewDock::SetEdited( Node* root )
{
	m_edited = root != nullptr ? ObjectID( root->get_instance_id() ) : ObjectID();
	Rebuild();
}

void CbCuePreviewDock::OnReload()
{
	Rebuild();
}

int CbCuePreviewDock::DetectMode( Node* root ) const
{
	int selected = m_mode->get_selected_id();
	if ( selected != MODE_AUTO )
	{
		return selected;
	}
	if ( Contains<Skeleton3D>( root ) ) // a rigged body
	{
		return MODE_CHARACTER;
	}
	// Reactions that name their subject from the cue, and nothing to look at: a world scene.
	std::vector<CbReaction*> reactions;
	FindReactions( root, reactions );
	bool fromCue = std::any_of( reactions.begin(), reactions.end(),
								[]( CbReaction* r ) { return String( r->get_subject() ).begins_with( "$" ); } );
	return fromCue && Contains<VisualInstance3D>( root ) == false ? MODE_WORLD : MODE_ITEM;
}

Node3D* CbCuePreviewDock::StandIn( const String& name, const Vector3& at, const Color& color )
{
	auto* body = memnew( Node3D );
	body->set_name( name );
	body->set_position( at );
	auto* mesh = memnew( MeshInstance3D );
	Ref<CapsuleMesh> capsule;
	capsule.instantiate();
	capsule->set_radius( 0.28 );
	capsule->set_height( 1.75 );
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo( color );
	capsule->set_material( material );
	mesh->set_mesh( capsule );
	mesh->set_position( Vector3( 0, 0.875, 0 ) );
	mesh->set_name( "Body" );
	body->add_child( mesh );
	return body;
}

Node* CbCuePreviewDock::EntityFor( int who ) const
{
	switch ( who )
	{
		case WHO_PLAYER0:
			return Object::cast_to<Node>( ObjectDB::get_instance( m_player0 ) );
		case WHO_PLAYER1:
			return Object::cast_to<Node>( ObjectDB::get_instance( m_player1 ) );
		case WHO_ITEM:
			return Object::cast_to<Node>( ObjectDB::get_instance( m_item ) );
		case WHO_WORLD:
			return m_director;
		default:
			return nullptr;
	}
}

void CbCuePreviewDock::Rebuild()
{
	if ( m_director != nullptr )
	{
		m_stage->remove_child( m_director );
		m_director->queue_free();
	}
	m_director = memnew( CbDirector );
	m_director->set_name( "World" );
	m_director->connect( "screen_effect", callable_mp( this, &CbCuePreviewDock::OnScreenEffect ) );
	m_stage->add_child( m_director );
	m_item = ObjectID();

	auto* root = Object::cast_to<Node>( ObjectDB::get_instance( m_edited ) );
	int mode = root != nullptr ? DetectMode( root ) : MODE_ITEM;
	Node* copy = root != nullptr ? root->duplicate() : nullptr;

	// player_0 (the scene itself for a character) faces player_1.
	Node3D* player0 = nullptr;
	if ( mode == MODE_CHARACTER && Object::cast_to<Node3D>( copy ) != nullptr )
	{
		player0 = Object::cast_to<Node3D>( copy );
		player0->set_name( "player_0" );
		player0->set_position( Vector3( 0, 0, 1.4 ) );
		copy = nullptr;
	}
	else
	{
		player0 = StandIn( "player_0", Vector3( 0, 0, 1.4 ), Color( 0.35, 0.55, 0.95 ) );
	}
	player0->set_rotation( Vector3( 0, Math_PI, 0 ) ); // facing -Z, toward player_1
	Node3D* player1 = StandIn( "player_1", Vector3( 0, 0, -1.4 ), Color( 0.95, 0.5, 0.35 ) );
	for ( Node3D* player : { player0, player1 } )
	{
		// Sockets are children of the entity, as in the game.
		struct Socket
		{
			const char* name;
			Vector3 at;
		};
		for ( Socket s : { Socket{ "RightHand", Vector3( -0.32, 1.05, 0.3 ) }, Socket{ "LeftHand", Vector3( 0.32, 1.05, 0.3 ) },
						   Socket{ "Head", Vector3( 0, 1.62, 0 ) } } )
		{
			if ( player->get_node_or_null( NodePath( s.name ) ) == nullptr )
			{
				auto* socket = memnew( Node3D );
				socket->set_name( s.name );
				socket->set_position( s.at );
				socket->set_rotation( Vector3( 0, Math_PI, 0 ) ); // items point along the player's facing
				player->add_child( socket );
			}
		}
		m_director->add_child( player );
		m_director->add_entity( player, "player", "" );
	}
	player1->set_rotation( Vector3( 0, 0, 0 ) );
	m_player0 = ObjectID( player0->get_instance_id() );
	m_player1 = ObjectID( player1->get_instance_id() );

	String what = root != nullptr ? String( root->get_name() ) : String( "nothing" );
	if ( copy != nullptr && mode == MODE_ITEM )
	{
		copy->set_name( "Item" );
		player0->get_node<Node>( "RightHand" )->add_child( copy );
		String kind = root->get_scene_file_path().get_file().get_basename();
		m_director->add_entity( copy, "item", kind );
		m_item = ObjectID( copy->get_instance_id() );
		what += " (held item in player_0/RightHand)";
	}
	else if ( copy != nullptr )
	{
		m_director->add_child( copy );
		what += " (world reactions)";
	}
	else if ( mode == MODE_CHARACTER )
	{
		what += " (character, as player_0)";
	}
	m_title->set_text( what );

	CollectNames();
	for ( int who : { WHO_PLAYER0, WHO_PLAYER1, WHO_ITEM } )
	{
		if ( Node* entity = EntityFor( who ) )
		{
			m_director->set_state( entity, m_states[who].duplicate() );
		}
	}
	m_director->set_world_state( m_states[WHO_WORLD].duplicate() );
	OnLocal( m_local->get_selected() );
	RefreshState();
	m_log->set_text( String::num_int64( int64_t( m_cues.size() ) ) + " cue name(s) and " + String::num_int64( int64_t( m_names.size() ) ) +
					 " state name(s) found." );
}

void CbCuePreviewDock::CollectNames()
{
	std::vector<CbReaction*> reactions;
	FindReactions( m_director, reactions );
	m_names.clear();
	m_cues.clear();
	auto add = []( std::vector<String>& list, const String& name ) {
		if ( name.is_empty() == false && std::find( list.begin(), list.end(), name ) == list.end() )
		{
			list.push_back( name );
		}
	};
	for ( CbReaction* reaction : reactions )
	{
		if ( reaction->get_when() == CbReaction::WHEN_EVENT )
		{
			add( m_cues, reaction->get_event().strip_edges() );
		}
		PackedStringArray conditions = reaction->get_conditions();
		for ( int64_t i = 0; i < conditions.size(); ++i )
		{
			add( m_names, NameIn( conditions[i] ) );
		}
	}
	PackedStringArray known;
	for ( const String& name : m_names )
	{
		known.push_back( name );
	}
	m_director->set_known( known );

	m_cueNames->clear();
	m_cueNames->add_item( "Pick..." );
	for ( const String& cue : m_cues )
	{
		m_cueNames->add_item( cue );
	}
	m_cueNames->add_separator();
	for ( const char* cue : kBuiltInCues )
	{
		m_cueNames->add_item( cue );
	}
	if ( m_cue->get_text().is_empty() && m_cues.empty() == false )
	{
		m_cue->set_text( m_cues[0] );
	}

	for ( int i = m_stateRows->get_child_count() - 1; i >= 0; --i )
	{
		Node* row = m_stateRows->get_child( i );
		m_stateRows->remove_child( row );
		row->queue_free();
	}
	if ( m_names.empty() )
	{
		m_stateRows->add_child( MakeLabel( "(no condition reads any state)" ) );
	}
	for ( const String& name : m_names )
	{
		auto* row = memnew( HBoxContainer );
		auto* label = MakeLabel( name );
		label->set_h_size_flags( SIZE_EXPAND_FILL );
		row->add_child( label );
		auto* spin = memnew( SpinBox );
		spin->set_name( name.replace( ".", "_" ) );
		spin->set_min( -100000 );
		spin->set_max( 100000 );
		spin->set_step( 0.01 );
		spin->set_tooltip_text( "0 is false, anything else true." );
		spin->connect( "value_changed", callable_mp( this, &CbCuePreviewDock::OnStateValue ).bind( name ) );
		row->add_child( spin );
		m_stateRows->add_child( row );
	}
}

void CbCuePreviewDock::RefreshState()
{
	int who = m_stateEntity->get_selected_id();
	for ( const String& name : m_names )
	{
		if ( auto* spin = m_stateRows->find_child( name.replace( ".", "_" ), true, false ) )
		{
			Object::cast_to<SpinBox>( spin )->set_value_no_signal( double( m_states[who].get( name, 0 ) ) );
		}
	}
}

void CbCuePreviewDock::OnCuePicked( int index )
{
	if ( index > 0 )
	{
		m_cue->set_text( m_cueNames->get_item_text( index ) );
	}
	m_cueNames->select( 0 );
}

void CbCuePreviewDock::OnStateEntity( int )
{
	RefreshState();
}

void CbCuePreviewDock::OnStateValue( double value, String name )
{
	int who = m_stateEntity->get_selected_id();
	m_states[who][name] = value == std::floor( value ) ? Variant( int64_t( value ) ) : Variant( value );
	if ( m_director == nullptr )
	{
		return;
	}
	if ( who == WHO_WORLD )
	{
		m_director->set_world_state( m_states[who].duplicate() );
	}
	else if ( Node* entity = EntityFor( who ) )
	{
		m_director->set_state( entity, m_states[who].duplicate() );
	}
	else
	{
		m_log->set_text( String( kWhoNames[who] ) + " is not on the stage." );
	}
}

void CbCuePreviewDock::OnLocal( int )
{
	if ( m_director != nullptr )
	{
		m_director->set_local( EntityFor( m_local->get_selected_id() ) );
	}
}

void CbCuePreviewDock::OnFire()
{
	String name = m_cue->get_text().strip_edges();
	if ( m_director == nullptr || name.is_empty() )
	{
		return;
	}
	Node* at = EntityFor( m_at->get_selected_id() );
	Node* other = EntityFor( m_other->get_selected_id() );
	// The point: $other's chest, or a step in front of $at.
	Vector3 point( 0, 1.2, 0 );
	if ( auto* target = Object::cast_to<Node3D>( other ) )
	{
		point = target->get_global_position() + Vector3( 0, 1.2, 0 );
	}
	else if ( auto* source = Object::cast_to<Node3D>( at ) )
	{
		point = source->get_global_position() + source->get_global_basis().get_column( 2 ) * -1.0 + Vector3( 0, 1.2, 0 );
	}
	Dictionary args;
	args["value"] = int64_t( m_value->get_value() );
	args["strength"] = m_strength->get_value();
	args["point"] = point;
	args["end"] = point;
	m_director->cue( name, at, other, args );
	m_log->set_text( "cue " + name + " at " + String( kWhoNames[m_at->get_selected_id()] ) + ", other " +
					 String( kWhoNames[m_other->get_selected_id()] ) );
}

void CbCuePreviewDock::OnScreenEffect( double shake, double shakeTime, Color flash, double flashTime )
{
	if ( shake > 0.0 )
	{
		m_shake = std::max( m_shake, shake );
		m_shakeDecay = shake / std::max( shakeTime, 0.05 );
	}
	if ( flash.a > 0.0f )
	{
		m_flashColor = flash;
		m_flashAlpha = flash.a;
		m_flashDecay = flash.a / std::max( flashTime, 0.02 );
	}
}

void CbCuePreviewDock::_process( double delta )
{
	if ( m_camera == nullptr )
	{
		return;
	}
	m_shake = std::max( 0.0, m_shake - m_shakeDecay * delta );
	Transform3D camera = m_cameraHome;
	if ( m_shake > 0.0 )
	{
		camera.origin += Vector3( UtilityFunctions::randf_range( -m_shake, m_shake ), UtilityFunctions::randf_range( -m_shake, m_shake ),
								  UtilityFunctions::randf_range( -m_shake, m_shake ) );
	}
	m_camera->set_transform( camera );
	m_flashAlpha = std::max( 0.0, m_flashAlpha - m_flashDecay * delta );
	m_flash->set_color( Color( m_flashColor.r, m_flashColor.g, m_flashColor.b, float( m_flashAlpha ) ) );
}

void CbCuePreviewPlugin::_enter_tree()
{
	m_dock = memnew( CbCuePreviewDock );
	m_dock->Build();
	add_control_to_bottom_panel( m_dock, "Cue Preview" );
	connect( "scene_changed", callable_mp( this, &CbCuePreviewPlugin::OnSceneChanged ) );
	m_dock->SetEdited( EditorInterface::get_singleton()->get_edited_scene_root() );
}

void CbCuePreviewPlugin::_exit_tree()
{
	if ( m_dock != nullptr )
	{
		remove_control_from_bottom_panel( m_dock );
		m_dock->queue_free();
		m_dock = nullptr;
	}
}

void CbCuePreviewPlugin::OnSceneChanged( Node* root )
{
	if ( m_dock != nullptr )
	{
		m_dock->SetEdited( root );
	}
}

} // namespace cb::gd
