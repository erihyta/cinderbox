#include "cinderbox_item_look.h"

#include "anim_graph.h"
#include "anim_set.h"
#include "cinderbox_motion.h"
#include "cinderbox_skeleton.h"
#include "joint_math.h"
#include "cue_director.h"
#include "cue_names.h"
#include "cue_prediction.h"
#include "cue_rules.h"
#include "events.h"
#include "cinderbox_map_nodes.h"
#include "mod_schema.h"
#include "pose.h"
#include "types.h"

#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace godot;

namespace cb::gd
{

void CbItem::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_kind", "value" ), &CbItem::set_kind );
	ClassDB::bind_method( D_METHOD( "get_kind" ), &CbItem::get_kind );
	ClassDB::bind_method( D_METHOD( "set_display_name", "value" ), &CbItem::set_display_name );
	ClassDB::bind_method( D_METHOD( "get_display_name" ), &CbItem::get_display_name );
	ClassDB::bind_method( D_METHOD( "set_view_offset", "value" ), &CbItem::set_view_offset );
	ClassDB::bind_method( D_METHOD( "get_view_offset" ), &CbItem::get_view_offset );
	ClassDB::bind_method( D_METHOD( "set_view_camera", "value" ), &CbItem::set_view_camera );
	ClassDB::bind_method( D_METHOD( "get_view_camera" ), &CbItem::get_view_camera );
	ClassDB::bind_method( D_METHOD( "set_view_rotation", "value" ), &CbItem::set_view_rotation );
	ClassDB::bind_method( D_METHOD( "get_view_rotation" ), &CbItem::get_view_rotation );
	ClassDB::bind_method( D_METHOD( "set_view_fov", "value" ), &CbItem::set_view_fov );
	ClassDB::bind_method( D_METHOD( "get_view_fov" ), &CbItem::get_view_fov );
	ClassDB::bind_method( D_METHOD( "set_view_left_hand", "value" ), &CbItem::set_view_left_hand );
	ClassDB::bind_method( D_METHOD( "get_view_left_hand" ), &CbItem::get_view_left_hand );
	ClassDB::bind_method( D_METHOD( "set_view_right_hand", "value" ), &CbItem::set_view_right_hand );
	ClassDB::bind_method( D_METHOD( "get_view_right_hand" ), &CbItem::get_view_right_hand );
	ClassDB::bind_method( D_METHOD( "set_properties", "value" ), &CbItem::set_properties );
	ClassDB::bind_method( D_METHOD( "get_properties" ), &CbItem::get_properties );
	ClassDB::bind_method( D_METHOD( "bake" ), &CbItem::bake );
	ClassDB::bind_method( D_METHOD( "bake_motions" ), &CbItem::bake_motions );
	ClassDB::bind_method( D_METHOD( "bake_to_project" ), &CbItem::bake_to_project );
	ClassDB::bind_method( D_METHOD( "get_bake_button" ), &CbItem::get_bake_button );
	ADD_GROUP( "Item", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "kind", PROPERTY_HINT_PLACEHOLDER_TEXT, "melee.bat" ), "set_kind", "get_kind" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "display_name", PROPERTY_HINT_PLACEHOLDER_TEXT, "Bat" ), "set_display_name",
				  "get_display_name" );
	ClassDB::bind_method( D_METHOD( "set_fields", "value" ), &CbItem::set_fields );
	ClassDB::bind_method( D_METHOD( "get_fields" ), &CbItem::get_fields );
	ADD_PROPERTY( PropertyInfo( Variant::DICTIONARY, "fields", PROPERTY_HINT_DICTIONARY_TYPE, "String;Variant" ), "set_fields", "get_fields" );
	ADD_PROPERTY( PropertyInfo( Variant::DICTIONARY, "properties", PROPERTY_HINT_DICTIONARY_TYPE, "String;float" ), "set_properties",
				  "get_properties" );
	ClassDB::bind_method( D_METHOD( "set_carry_grip", "value" ), &CbItem::set_carry_grip );
	ClassDB::bind_method( D_METHOD( "get_carry_grip" ), &CbItem::get_carry_grip );
	ClassDB::bind_method( D_METHOD( "set_other_hand", "value" ), &CbItem::set_other_hand );
	ClassDB::bind_method( D_METHOD( "get_other_hand" ), &CbItem::get_other_hand );
	ClassDB::bind_method( D_METHOD( "set_other_grip", "value" ), &CbItem::set_other_grip );
	ClassDB::bind_method( D_METHOD( "get_other_grip" ), &CbItem::get_other_grip );
	ClassDB::bind_static_method( "CbItem", D_METHOD( "carry_frame_under", "root" ), &CbItem::carry_frame_under );
	ClassDB::bind_method( D_METHOD( "set_use", "value" ), &CbItem::set_use );
	ClassDB::bind_method( D_METHOD( "get_use" ), &CbItem::get_use );
	ClassDB::bind_method( D_METHOD( "set_slot", "value" ), &CbItem::set_slot );
	ClassDB::bind_method( D_METHOD( "get_slot" ), &CbItem::get_slot );
	ClassDB::bind_method( D_METHOD( "set_animation_pack", "value" ), &CbItem::set_animation_pack );
	ClassDB::bind_method( D_METHOD( "get_animation_pack" ), &CbItem::get_animation_pack );
	ClassDB::bind_method( D_METHOD( "set_holster", "value" ), &CbItem::set_holster );
	ClassDB::bind_method( D_METHOD( "get_holster" ), &CbItem::get_holster );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "animation_pack", PROPERTY_HINT_PLACEHOLDER_TEXT, "none: the character's own" ), "set_animation_pack",
				  "get_animation_pack" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "holster", PROPERTY_HINT_PLACEHOLDER_TEXT, "none: out of sight (Hip, Back)" ), "set_holster", "get_holster" );
	ClassDB::bind_method( D_METHOD( "set_bullet", "value" ), &CbItem::set_bullet );
	ClassDB::bind_method( D_METHOD( "get_bullet" ), &CbItem::get_bullet );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "bullet" ), "set_bullet", "get_bullet" );
	ADD_GROUP( "Breaks walls", "break_" );
	ClassDB::bind_method( D_METHOD( "set_break_speed", "value" ), &CbItem::set_break_speed );
	ClassDB::bind_method( D_METHOD( "get_break_speed" ), &CbItem::get_break_speed );
	ClassDB::bind_method( D_METHOD( "set_break_hole", "value" ), &CbItem::set_break_hole );
	ClassDB::bind_method( D_METHOD( "get_break_hole" ), &CbItem::get_break_hole );
	ClassDB::bind_method( D_METHOD( "set_break_through", "value" ), &CbItem::set_break_through );
	ClassDB::bind_method( D_METHOD( "get_break_through" ), &CbItem::get_break_through );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "break_speed", PROPERTY_HINT_RANGE, "0,200,0.5,or_greater,suffix:m/s" ), "set_break_speed",
				  "get_break_speed" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "break_hole", PROPERTY_HINT_RANGE, "0,10,0.05,or_greater,suffix:m" ), "set_break_hole", "get_break_hole" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "break_through", PROPERTY_HINT_RANGE, "0,1,0.05" ), "set_break_through", "get_break_through" );
	ADD_GROUP( "Slots", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "use", PROPERTY_HINT_ENUM, "While the button is down (a tool),Once a press,A press, and while held" ),
				  "set_use", "get_use" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "slot", PROPERTY_HINT_RANGE, "0,36,1" ), "set_slot", "get_slot" );
	ClassDB::bind_method( D_METHOD( "set_slot_key_uses", "value" ), &CbItem::set_slot_key_uses );
	ClassDB::bind_method( D_METHOD( "get_slot_key_uses" ), &CbItem::get_slot_key_uses );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "slot_key_uses" ), "set_slot_key_uses", "get_slot_key_uses" );
	ClassDB::bind_method( D_METHOD( "set_hud", "value" ), &CbItem::set_hud );
	ClassDB::bind_method( D_METHOD( "get_hud" ), &CbItem::get_hud );
	ClassDB::bind_method( D_METHOD( "set_hud_shows", "value" ), &CbItem::set_hud_shows );
	ClassDB::bind_method( D_METHOD( "get_hud_shows" ), &CbItem::get_hud_shows );
	ClassDB::bind_method( D_METHOD( "set_use_key", "value" ), &CbItem::set_use_key );
	ClassDB::bind_method( D_METHOD( "get_use_key" ), &CbItem::get_use_key );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "use_key", PROPERTY_HINT_PLACEHOLDER_TEXT, "H: uses the first one carried, wherever it is (empty: from the hand only)" ),
				  "set_use_key", "get_use_key" );
	ClassDB::bind_method( D_METHOD( "set_hand", "value" ), &CbItem::set_hand );
	ClassDB::bind_method( D_METHOD( "get_hand" ), &CbItem::get_hand );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hand", PROPERTY_HINT_ENUM, "The slots' hand while selected,The left hand while carried" ), "set_hand",
				  "get_hand" );
	ClassDB::bind_method( D_METHOD( "set_icon", "value" ), &CbItem::set_icon );
	ClassDB::bind_method( D_METHOD( "get_icon" ), &CbItem::get_icon );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "icon", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D" ), "set_icon", "get_icon" );
	ClassDB::bind_method( D_METHOD( "set_stack", "value" ), &CbItem::set_stack );
	ClassDB::bind_method( D_METHOD( "get_stack" ), &CbItem::get_stack );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "stack", PROPERTY_HINT_RANGE, "1,99,1" ), "set_stack", "get_stack" );
	ADD_GROUP( "When used", "use_" );
	ClassDB::bind_method( D_METHOD( "set_use_conditions", "value" ), &CbItem::set_use_conditions );
	ClassDB::bind_method( D_METHOD( "get_use_conditions" ), &CbItem::get_use_conditions );
	ClassDB::bind_method( D_METHOD( "set_use_changes", "value" ), &CbItem::set_use_changes );
	ClassDB::bind_method( D_METHOD( "get_use_changes" ), &CbItem::get_use_changes );
	ClassDB::bind_method( D_METHOD( "set_use_cooldown", "value" ), &CbItem::set_use_cooldown );
	ClassDB::bind_method( D_METHOD( "get_use_cooldown" ), &CbItem::get_use_cooldown );
	ClassDB::bind_method( D_METHOD( "set_use_emits", "value" ), &CbItem::set_use_emits );
	ClassDB::bind_method( D_METHOD( "get_use_emits" ), &CbItem::get_use_emits );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "use_conditions" ), "set_use_conditions", "get_use_conditions" );
	ADD_PROPERTY( PropertyInfo( Variant::PACKED_STRING_ARRAY, "use_changes" ), "set_use_changes", "get_use_changes" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "use_cooldown", PROPERTY_HINT_RANGE, "0,60,0.01,or_greater,suffix:s" ), "set_use_cooldown",
				  "get_use_cooldown" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "use_emits", PROPERTY_HINT_PLACEHOLDER_TEXT, "dash.started" ), "set_use_emits", "get_use_emits" );
	ADD_GROUP( "Hands", "" );
	ADD_PROPERTY( PropertyInfo( Variant::NODE_PATH, "carry_grip", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D" ), "set_carry_grip",
				  "get_carry_grip" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "other_hand", PROPERTY_HINT_ENUM,
								"Free,At the marker,At the marker and turned with it,As the animations have it" ),
				  "set_other_hand", "get_other_hand" );
	ADD_PROPERTY( PropertyInfo( Variant::NODE_PATH, "other_grip", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D" ), "set_other_grip",
				  "get_other_grip" );
	ADD_GROUP( "First person", "" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR3, "view_offset", PROPERTY_HINT_NONE, "suffix:m" ), "set_view_offset", "get_view_offset" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR3, "view_rotation", PROPERTY_HINT_NONE, "suffix:deg" ), "set_view_rotation", "get_view_rotation" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "view_fov", PROPERTY_HINT_RANGE, "0,120,0.5,suffix:deg" ), "set_view_fov", "get_view_fov" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "view_left_hand" ), "set_view_left_hand", "get_view_left_hand" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "view_right_hand" ), "set_view_right_hand", "get_view_right_hand" );
	ADD_PROPERTY( PropertyInfo( Variant::NODE_PATH, "view_camera", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D" ), "set_view_camera",
				  "get_view_camera" );
	ClassDB::bind_method( D_METHOD( "set_preview_hold", "value" ), &CbItem::set_preview_hold );
	ClassDB::bind_method( D_METHOD( "get_preview_hold" ), &CbItem::get_preview_hold );
	ClassDB::bind_method( D_METHOD( "set_preview_pack", "value" ), &CbItem::set_preview_pack );
	ClassDB::bind_method( D_METHOD( "get_preview_pack" ), &CbItem::get_preview_pack );
	// (Shown in the editor, kept nowhere: PROPERTY_USAGE_EDITOR.)
	ADD_GROUP( "Preview", "preview_" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "preview_hold", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR ), "set_preview_hold", "get_preview_hold" );
	ClassDB::bind_method( D_METHOD( "preview_view", "arms", "held" ), &CbItem::preview_view );
	ClassDB::bind_method( D_METHOD( "set_preview_cue", "value" ), &CbItem::set_preview_cue );
	ClassDB::bind_method( D_METHOD( "get_preview_cue" ), &CbItem::get_preview_cue );
	ClassDB::bind_method( D_METHOD( "fire_preview_cue", "name" ), &CbItem::fire_preview_cue );
	ClassDB::bind_method( D_METHOD( "fire_preview" ), &CbItem::fire_preview );
	ClassDB::bind_method( D_METHOD( "get_preview_fire_button" ), &CbItem::get_preview_fire_button );
	ClassDB::bind_method( D_METHOD( "set_preview_aim", "value" ), &CbItem::set_preview_aim );
	ClassDB::bind_method( D_METHOD( "get_preview_aim" ), &CbItem::get_preview_aim );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "preview_aim", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR ), "set_preview_aim", "get_preview_aim" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "preview_pack", PROPERTY_HINT_PLACEHOLDER_TEXT, "the first in res://anim", PROPERTY_USAGE_EDITOR ),
				  "set_preview_pack", "get_preview_pack" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "preview_cue", PROPERTY_HINT_PLACEHOLDER_TEXT, "melee.swing", PROPERTY_USAGE_EDITOR ), "set_preview_cue",
				  "get_preview_cue" );
	ADD_PROPERTY( PropertyInfo( Variant::CALLABLE, "preview_fire_button", PROPERTY_HINT_TOOL_BUTTON, "Fire the cue,Play", PROPERTY_USAGE_EDITOR ), "",
				  "get_preview_fire_button" );
	ADD_GROUP( "", "" );
	ADD_PROPERTY( PropertyInfo( Variant::CALLABLE, "bake_button", PROPERTY_HINT_TOOL_BUTTON, "Bake item,Save", PROPERTY_USAGE_EDITOR ), "",
				  "get_bake_button" );

	ADD_GROUP( "Its HUD", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "hud", PROPERTY_HINT_FILE, "*.tscn" ), "set_hud", "get_hud" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hud_shows", PROPERTY_HINT_ENUM, "In the hand,While carried" ), "set_hud_shows", "get_hud_shows" );
	ADD_GROUP( "", "" );
	BIND_ENUM_CONSTANT( HUD_IN_HAND );
	BIND_ENUM_CONSTANT( HUD_CARRIED );
	BIND_ENUM_CONSTANT( USE_TOOL );
	BIND_ENUM_CONSTANT( USE_PRESS );
	BIND_ENUM_CONSTANT( USE_HOLD );
	BIND_ENUM_CONSTANT( HAND_SELECTED );
	BIND_ENUM_CONSTANT( HAND_LEFT );
	BIND_ENUM_CONSTANT( OTHER_FREE );
	BIND_ENUM_CONSTANT( OTHER_AT_MARKER );
	BIND_ENUM_CONSTANT( OTHER_AT_MARKER_TURNED );
	BIND_ENUM_CONSTANT( OTHER_AS_ANIMATED );
}

void CbViewCamera::_notification( int what )
{
	if ( what == NOTIFICATION_ENTER_TREE && Engine::get_singleton()->is_editor_hint() == false )
	{
		clear_current( true );
		queue_free();
	}
}

void CbLinkLook::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_motion", "value" ), &CbLinkLook::set_motion );
	ClassDB::bind_method( D_METHOD( "get_motion" ), &CbLinkLook::get_motion );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "motion", PROPERTY_HINT_PLACEHOLDER_TEXT, "grapple.hook/Use (empty: any link)" ), "set_motion",
				  "get_motion" );
	ClassDB::bind_method( D_METHOD( "set_scene", "value" ), &CbLinkLook::set_scene );
	ClassDB::bind_method( D_METHOD( "get_scene" ), &CbLinkLook::get_scene );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "scene", PROPERTY_HINT_FILE, "*.tscn,*.scn" ), "set_scene", "get_scene" );
	ClassDB::bind_method( D_METHOD( "set_from", "value" ), &CbLinkLook::set_from );
	ClassDB::bind_method( D_METHOD( "get_from" ), &CbLinkLook::get_from );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "from", PROPERTY_HINT_PLACEHOLDER_TEXT, "RightHand (empty: the chest)" ), "set_from", "get_from" );
}

Callable CbItem::get_bake_button()
{
	return Callable( this, "bake_to_project" );
}

namespace
{

// The item's scene that `node` is part of: the scene it was saved in (its owner), or, for a scene
// built in code, the topmost node above it.
const Node* SceneRoot( const Node* node )
{
	if ( node->get_owner() != nullptr )
	{
		return node->get_owner();
	}
	while ( node->get_parent() != nullptr )
	{
		node = node->get_parent();
	}
	return node;
}

// Where a node is in its scene's frame (the root's): through every Node3D between them.
Transform3D InSceneFrame( const Node3D* node, const Node* root )
{
	if ( node == root )
	{
		return Transform3D();
	}
	Transform3D t = node->get_transform();
	for ( Node* parent = node->get_parent(); parent != nullptr && parent != root; parent = parent->get_parent() )
	{
		if ( auto* spatial = Object::cast_to<Node3D>( parent ) )
		{
			t = spatial->get_transform() * t;
		}
	}
	return t;
}

// The item's body: the CollisionShape3D nodes in its scene (the first, when it is one box or ball).
std::vector<const CollisionShape3D*> ShapesOf( const Node* item )
{
	std::vector<const CollisionShape3D*> out;
	TypedArray<Node> found = const_cast<Node*>( item )->find_children( "*", "CollisionShape3D", true, false );
	for ( int64_t i = 0; i < found.size(); ++i )
	{
		out.push_back( Object::cast_to<CollisionShape3D>( found[i] ) );
	}
	return out;
}

const CollisionShape3D* BodyOf( const Node* item )
{
	std::vector<const CollisionShape3D*> shapes = ShapesOf( item );
	return shapes.empty() ? nullptr : shapes[0];
}

// Whether the item's body is the one box or ball the item format has always had (unscaled): then it
// bakes as it always did, and an item's file (its hash) does not change.
bool OneSimpleShape( const Node* item )
{
	std::vector<const CollisionShape3D*> shapes = ShapesOf( item );
	if ( shapes.size() != 1 || shapes[0]->get_shape().is_null() || shapes[0]->is_disabled() )
	{
		return false;
	}
	Ref<Shape3D> shape = shapes[0]->get_shape();
	if ( Object::cast_to<BoxShape3D>( shape.ptr() ) == nullptr && Object::cast_to<SphereShape3D>( shape.ptr() ) == nullptr )
	{
		return false;
	}
	Transform3D t = InSceneFrame( shapes[0], SceneRoot( shapes[0] ) );
	return t.basis.get_scale().is_equal_approx( Vector3( 1, 1, 1 ) ) && t.basis.determinant() > 0.0f;
}

String ShapeProblem( const CollisionShape3D* body )
{
	if ( body == nullptr )
	{
		return "Give it a CollisionShape3D child (a box, a ball, a capsule, a cylinder or a convex polygon; several make one body): "
			   "its body when it lies in the world.";
	}
	// Each of its shapes as the bake would make it (turned and scaled any way: the bake writes them
	// in the frame the item is carried in, the scale in their sizes).
	for ( const CollisionShape3D* node : ShapesOf( SceneRoot( body ) ) )
	{
		BakedShape baked;
		Array said;
		String why = BakeShape( node, InSceneFrame( node, SceneRoot( node ) ), node->get_name(), baked, said );
		if ( why.is_empty() == false )
		{
			return String( node->get_name() ) + ": " + why + ".";
		}
	}
	return String();
}

} // namespace

// What the preview runs with.
struct CbItem::PreviewRun
{
	ModSchema schema; // the names this checkout's mods declare (cinderbox_names.cfg): events, stances, layers
	AnimState anim;	  // where the pack's state machine is
	ModEventRecord events[kModEventHistory];
	uint32_t eventCount = 0;
	uint32_t tick = 0;
	double pending = 0.0; // time not yet stepped
	double seconds = 0.0;
	double stanceUntil[4] = { 0.0, 0.0, 0.0, 0.0 }; // per layer: when a fired cue's stance ends
	double firedAt = -100.0;
	ObjectID viewStage; // the first-person picture's (the third-person one is m_previewCopy)
};

void CbItem::_validate_property( PropertyInfo& property ) const
{
	names::Hint( this, property );
}

Callable CbItem::get_preview_fire_button()
{
	return Callable( this, "fire_preview" );
}

void CbItem::fire_preview()
{
	if ( m_previewHold == false )
	{
		UtilityFunctions::push_warning( "Item preview: tick Hold first: a cue plays on the figure." );
		return;
	}
	if ( m_previewCue.is_empty() )
	{
		UtilityFunctions::push_warning( "Item preview: name the cue to fire (Preview > Cue): an event of the item's mod, such as melee.swing." );
		return;
	}
	fire_preview_cue( m_previewCue );
}

void CbItem::fire_preview_cue( const String& cue )
{
	if ( m_previewRun == nullptr || m_previewGraph == nullptr )
	{
		return;
	}
	PreviewRun& run = *m_previewRun;
	std::string name = std::string( cue.strip_edges().utf8().get_data() );
	run.firedAt = run.seconds;
	// To the state machine: the event, at the next tick (a state entered on it starts).
	int event = run.schema.FindEvent( name );
	if ( event >= 0 )
	{
		ModEventRecord& e = run.events[run.eventCount % kModEventHistory];
		e = ModEventRecord{};
		e.type = uint16_t( event );
		e.netIdA = 1;
		e.tick = run.tick + 1;
		run.eventCount += 1;
	}
	else
	{
		UtilityFunctions::push_warning( "Item preview: no mod declares the event \"", cue, "\" (cinderbox_names.cfg): the state machine does not hear it." );
	}
	// And what the item's own predictions set with it: a stance, for as long as the prediction
	// keeps it (the server's mod sets it for as long as its rule says; this is the look's guess).
	TypedArray<Node> predictions = find_children( "*", "CbPrediction", true, false );
	for ( int i = 0; i < predictions.size(); ++i )
	{
		auto* prediction = Object::cast_to<CbPrediction>( predictions[i] );
		// (The item's own: not those of its copy in the preview's hand.)
		auto* stage = Object::cast_to<Node>( ObjectDB::get_instance( m_previewCopy ) );
		if ( prediction == nullptr || ( stage != nullptr && stage->is_ancestor_of( prediction ) ) ||
			 prediction->get_cue().strip_edges() != cue.strip_edges() || prediction->get_stance().strip_edges().is_empty() )
		{
			continue;
		}
		int stance = run.schema.FindStance( std::string( prediction->get_stance().strip_edges().utf8().get_data() ) );
		int layer = run.schema.FindLayer( std::string( prediction->get_stance_layer().strip_edges().utf8().get_data() ) );
		if ( stance < 0 || layer < 0 || layer >= 4 )
		{
			continue;
		}
		if ( run.anim.stances.size() <= size_t( layer ) )
		{
			run.anim.stances.resize( size_t( layer ) + 1 );
		}
		run.anim.stances[size_t( layer )] = uint8_t( stance + 1 );
		run.stanceUntil[layer] = run.seconds + std::clamp( prediction->get_cooldown(), 0.2, 1.0 );
	}
	// To the item in each hand: its reactions.
	for ( ObjectID id : { m_previewCopy, run.viewStage } )
	{
		auto* stage = Object::cast_to<CbDirector>( ObjectDB::get_instance( id ) );
		auto* player = stage != nullptr ? stage->get_node_or_null( "player_0" ) : nullptr;
		auto* hand = player != nullptr ? Object::cast_to<Node3D>( player->get_node_or_null( "RightHand" ) ) : nullptr;
		if ( hand == nullptr )
		{
			continue;
		}
		Dictionary args;
		args["value"] = 0;
		args["strength"] = 1.0;
		args["point"] = hand->get_global_position();
		args["end"] = hand->get_global_position() + hand->get_global_basis().get_column( 2 ) * -5.0f;
		stage->cue( cue.strip_edges(), player, nullptr, args );
	}
}

void CbItem::PreviewAdvance( double delta )
{
	PreviewRun& run = *m_previewRun;
	const float dt = 1.0f / 60.0f;
	// (At least once before anything is posed: the state machine has to have started.)
	run.pending = std::min( run.pending + delta, 0.25 );
	if ( run.tick == 0 )
	{
		run.pending = std::max( run.pending, double( dt ) );
	}
	const AnimGraphPacks none;
	while ( run.pending >= double( dt ) )
	{
		run.pending -= double( dt );
		run.tick += 1;
		run.seconds += double( dt );
		for ( size_t layer = 0; layer < 4 && layer < run.anim.stances.size(); ++layer )
		{
			if ( run.stanceUntil[layer] > 0.0 && run.seconds >= run.stanceUntil[layer] )
			{
				run.anim.stances[layer] = 0;
				run.stanceUntil[layer] = 0.0;
			}
		}
		AnimGraphInputs in;
		in.builtins[AnimExpr::Grounded] = 1.0f;
		in.events = run.events;
		in.eventCount = run.eventCount;
		in.tick = run.tick;
		in.netId = 1;
		std::vector<int> markers;
		UpdateAnimGraph( run.anim, *m_previewGraph, none, in, dt, markers );
	}
}

void CbItem::PreviewStage( Node3D* under, const Transform3D& handAt, bool firstPerson, bool internal )
{
	static const StringName kAge( "cb_age" );
	CbDirector* stage = nullptr;
	for ( int i = 0; i < under->get_child_count( true ); ++i )
	{
		auto* found = Object::cast_to<CbDirector>( under->get_child( i, true ) );
		if ( found != nullptr && found->is_queued_for_deletion() == false )
		{
			stage = found;
		}
	}
	// Made again twice a second, for the scene is being edited; not while a cue's effects play.
	PreviewRun& run = *m_previewRun;
	if ( stage != nullptr && int( stage->get_meta( kAge, 0 ) ) >= 30 && run.seconds - run.firedAt > 3.0 )
	{
		stage->set_name( "Leaving" );
		stage->queue_free();
		stage = nullptr;
	}
	if ( stage == nullptr )
	{
		stage = memnew( CbDirector );
		stage->set_name( "PreviewStage" );
		auto* player = memnew( Node3D );
		player->set_name( "player_0" );
		auto* hand = memnew( Node3D );
		hand->set_name( "RightHand" );
		player->add_child( hand );
		// The scene as it is now, unsaved edits too; without what the preview itself put in it.
		Node* copy = duplicate();
		std::vector<Node*> own;
		for ( int i = 0; i < copy->get_child_count( true ); ++i )
		{
			own.push_back( copy->get_child( i, true ) );
		}
		for ( int i = 0; i < copy->get_child_count( false ); ++i )
		{
			own.erase( std::remove( own.begin(), own.end(), copy->get_child( i, false ) ), own.end() );
		}
		for ( Node* extra : own )
		{
			copy->remove_child( extra );
			extra->queue_free();
		}
		copy->set_name( "Item" );
		if ( auto* spatial = Object::cast_to<Node3D>( copy ) )
		{
			spatial->set_transform( Transform3D() );
		}
		hand->add_child( copy );
		stage->add_child( player );
		under->add_child( stage, false, internal ? INTERNAL_MODE_BACK : INTERNAL_MODE_DISABLED );
		stage->add_entity( player, "player", "" );
		stage->add_entity( copy, "item", m_kind.strip_edges() );
		stage->set_local( player );
		Dictionary holder;
		holder["first_person"] = firstPerson;
		holder[m_kind.strip_edges()] = true;
		stage->set_state( player, holder );
		Dictionary held;
		held["in_use"] = true;
		stage->set_state( copy, held );
		stage->set_meta( kAge, 0 );
		// The mod's world reactions too (res://vfx/reactions*.tscn: a swing's sound, a hit's puff),
		// as the game has them under its director. Once: beside the figure, not in the picture
		// of the view as well (a sound would play twice).
		if ( firstPerson == false && DirAccess::dir_exists_absolute( "res://vfx" ) )
		{
			PackedStringArray files = DirAccess::get_files_at( "res://vfx" );
			for ( const String& file : files )
			{
				if ( file.begins_with( "reactions" ) == false || file.ends_with( ".tscn" ) == false )
				{
					continue;
				}
				Ref<PackedScene> world = ResourceLoader::get_singleton()->load( String( "res://vfx/" ) + file, "PackedScene" );
				if ( Node* reactions = world.is_valid() ? world->instantiate() : nullptr )
				{
					stage->add_child( reactions );
				}
			}
		}
		if ( firstPerson )
		{
			run.viewStage = ObjectID( stage->get_instance_id() );
		}
		else
		{
			m_previewCopy = ObjectID( stage->get_instance_id() );
		}
	}
	stage->set_meta( kAge, int( stage->get_meta( kAge, 0 ) ) + 1 );
	if ( auto* hand = Object::cast_to<Node3D>( stage->get_node_or_null( "player_0/RightHand" ) ) )
	{
		hand->set_transform( handAt );
	}
	stage->update();
}

Transform3D CbItem::HandSocketFrame()
{
	Basis turn = Basis::from_euler( Vector3( Math::deg_to_rad( -90.0 ), Math::deg_to_rad( 180.0 ), 0.0 ) );
	return Transform3D( turn, Vector3( 0, -0.06, 0 ) );
}

void CbItem::set_preview_hold( bool v )
{
	// (The editor's; a check outside it asks with the meta "cb_preview".)
	m_previewHold = v && ( Engine::get_singleton()->is_editor_hint() || has_meta( "cb_preview" ) );
	set_process( m_previewHold );
	if ( m_previewHold == false )
	{
		EndPreview();
	}
}

void CbItem::set_preview_pack( const String& v )
{
	m_previewPack = v.strip_edges();
	m_previewLoaded.clear(); // looked for again
}

void CbItem::EndPreview()
{
	if ( auto* figure = Object::cast_to<Node>( ObjectDB::get_instance( m_previewFigure ) ) )
	{
		figure->queue_free();
	}
	m_previewFigure = ObjectID();
	if ( auto* copy = Object::cast_to<Node>( ObjectDB::get_instance( m_previewCopy ) ) )
	{
		copy->queue_free();
	}
	m_previewCopy = ObjectID();
	if ( m_previewRun != nullptr )
	{
		if ( auto* stage = Object::cast_to<Node>( ObjectDB::get_instance( m_previewRun->viewStage ) ) )
		{
			stage->queue_free();
		}
	}
	m_previewRun.reset();
	m_previewPose.reset();
	m_previewGraph.reset();
	m_previewSet.reset();
	m_previewLoaded.clear();
}

void CbItem::_process( double delta )
{
	if ( m_previewHold == false || is_inside_tree() == false )
	{
		return;
	}
	// The pack: the one named, or the item's own (its Animation Pack), or the first the project has baked.
	String pack = m_previewPack.is_empty() ? m_animationPack : m_previewPack;
	if ( pack.is_empty() )
	{
		PackedStringArray packs = DirAccess::get_directories_at( "res://anim" );
		packs.sort();
		pack = packs.is_empty() ? String() : packs[0];
	}
	std::string folder = std::string( ( String( "res://anim/" ) + pack ).utf8().get_data() );
	if ( folder != m_previewLoaded )
	{
		m_previewLoaded = folder;
		m_previewPose.reset();
		m_previewSet.reset();
		m_previewGraph.reset();
		if ( pack.is_empty() )
		{
			UtilityFunctions::push_warning( "Item preview: this project has no baked animation pack (res://anim/<pack>/). Save a CbAnimPack scene first." );
			return;
		}
		String base = String( "res://anim/" ) + pack;
		anim::FileReader read = [base]( const std::string& file, std::string& bytes ) {
			PackedByteArray data = FileAccess::get_file_as_bytes( base.path_join( String::utf8( file.c_str() ) ) );
			bytes.assign( reinterpret_cast<const char*>( data.ptr() ), size_t( data.size() ) );
			return data.is_empty() == false;
		};
		std::string error, warnings;
		std::unique_ptr<anim::AnimSet> set = anim::AnimSet::Load( read, folder, error, warnings );
		if ( set == nullptr )
		{
			UtilityFunctions::push_warning( "Item preview: ", base, " does not load: ", String::utf8( error.c_str() ) );
			return;
		}
		// (Its conditions name the mod's events and stances, which nothing declares here: the
		// preview stays in the state the pack starts in.)
		// The names its conditions read are this checkout's mods' (the file the server's build writes
		// into the project): a state entered on "pistol.fired", a stance "melee_swing".
		auto run = std::make_shared<PreviewRun>();
		PackedStringArray declared = FileAccess::get_file_as_string( "res://cinderbox_names.cfg" ).split( "\n" );
		for ( const String& line : declared )
		{
			PackedStringArray parts = line.strip_edges().split( "\t" );
			if ( parts.size() < 2 )
			{
				continue;
			}
			std::string name = std::string( parts[1].utf8().get_data() );
			if ( parts[0] == String( "event" ) )
			{
				run->schema.events.push_back( name );
			}
			else if ( parts[0] == String( "stance" ) )
			{
				run->schema.stances.push_back( name );
			}
			else if ( parts[0] == String( "layer" ) )
			{
				run->schema.layers.push_back( name );
			}
		}
		auto graph = CompileAnimGraph( set->GraphText(), run->schema, error, warnings );
		if ( graph == nullptr || graph->layers.empty() )
		{
			UtilityFunctions::push_warning( "Item preview: the state machine of ", base, " does not load: ", String::utf8( error.c_str() ) );
			return;
		}
		// The hands' sockets are the character's: the one this checkout's server plays by default
		// (the project is <checkout>/server_mods/<mod>/client), if it is there to read. They are
		// given to the pack's own set, so the item's place in the hand and the place the other
		// hand reaches for are worked out from the same socket, as in the game. (A pack has none
		// of its own: with the built-in one for the reach and the character's for the item, the
		// other hand held the air beside it.)
		String checkout = ProjectSettings::get_singleton()->globalize_path( "res://" ).path_join( "../../.." ).simplify_path();
		for ( const char* character : { "ual_mannequin", "mannequin" } )
		{
			String cfg = checkout.path_join( "godot/characters" ).path_join( character ).path_join( "anim.cfg" );
			if ( FileAccess::file_exists( cfg ) == false )
			{
				continue;
			}
			PackedStringArray lines = FileAccess::get_file_as_string( cfg ).split( "\n" );
			for ( const String& line : lines )
			{
				bool right = line.begins_with( "socket.RightHand" );
				if ( ( right == false && line.begins_with( "socket.LeftHand" ) == false ) || line.contains( "=" ) == false )
				{
					continue;
				}
				PackedFloat64Array v = line.get_slice( "=", 1 ).strip_edges().split_floats( " ", false );
				if ( v.size() == 7 )
				{
					anim::AnimSet::HandSocket socket;
					socket.position = { float( v[0] ), float( v[1] ), float( v[2] ) };
					socket.rotation = b3NormalizeQuat( b3Quat{ { float( v[3] ), float( v[4] ), float( v[5] ) }, float( v[6] ) } );
					set->SetHandSocket( right == false, socket );
				}
			}
			break;
		}
		const anim::AnimSet::HandSocket& holding = set->HandSocketOf( false );
		m_previewSocket = Transform3D( Basis( Quaternion( holding.rotation.v.x, holding.rotation.v.y, holding.rotation.v.z, holding.rotation.s ) ),
									   Vector3( holding.position.x, holding.position.y, holding.position.z ) );
		m_previewSet = std::move( set );
		m_previewGraph = graph;
		m_previewRun = run;
		m_previewPose = std::make_shared<anim::PoseEvaluator>( *m_previewSet );
		m_previewPose->SetGraph( m_previewGraph, warnings );
	}
	if ( m_previewPose == nullptr )
	{
		return;
	}
	auto* figure = Object::cast_to<CinderboxSkeleton>( ObjectDB::get_instance( m_previewFigure ) );
	if ( figure == nullptr )
	{
		figure = memnew( CinderboxSkeleton );
		figure->set_name( "HoldPreview" );
		figure->set_draw_bone_boxes( true );
		figure->set_use_slot_color( false );
		figure->set_body_color( Color( 0.75f, 0.78f, 0.82f ) );
		add_child( figure, false, INTERNAL_MODE_BACK );
		m_previewFigure = ObjectID( figure->get_instance_id() );
	}

	// The pack's state machine runs: the state it starts in plays, and a cue moves it on.
	PreviewAdvance( delta );
	const Transform3D carry = CarryFrameUnder( this );
	PreviewEvaluate( m_previewAim );
	figure->ApplyPose( *m_previewSet, m_previewPose->Models() );

	// The figure stands upright beside the item, facing where the item points (-Z), its feet a
	// metre under the item's origin; a copy of the item's meshes is in its right hand, by the
	// carrying grip.
	figure->set_transform( Transform3D( Basis( Vector3( 0, 1, 0 ), float( Math_PI ) ), Vector3( 1.0f, -1.0f, 0.0f ) ) );
	// The item in its hand: a whole copy of the scene, by the carrying grip.
	Transform3D hand;
	if ( figure->JointTransform( "RightHand", hand ) )
	{
		PreviewStage( this, figure->get_transform() * hand * m_previewSocket * carry.affine_inverse(), false, true );
	}
}

void CbItem::PreviewEvaluate( bool aiming )
{
	// Where the pack's state machine is now.
	AnimState state = m_previewRun->anim;

	// The other hand, as the bake says it and the game solves it.
	const Transform3D carry = CarryFrameUnder( this );
	anim::HandGrip grip;
	bool held = m_otherHand != OTHER_FREE;
	if ( held )
	{
		auto* other = m_otherGrip.is_empty() ? nullptr : Object::cast_to<Node3D>( get_node_or_null( m_otherGrip ) );
		Transform3D at = other != nullptr ? ( carry.affine_inverse() * InSceneFrame( other, this ) ).orthonormalized() : Transform3D();
		Quaternion turn = at.basis.get_rotation_quaternion();
		grip.align = m_otherHand == OTHER_AT_MARKER_TURNED;
		grip.asAnimated = m_otherHand == OTHER_AS_ANIMATED;
		grip.position = { float( at.origin.x ), float( at.origin.y ), float( at.origin.z ) };
		grip.rotation = { { float( turn.x ), float( turn.y ), float( turn.z ) }, float( turn.w ) };
		held = grip.asAnimated || other != nullptr;
	}
	state.aiming = aiming ? 1 : 0;
	m_previewPose->Evaluate( state, held ? &grip : nullptr );
}

namespace
{

b3Vec3 PlaceOf( const ozz::math::Float4x4& m )
{
	float v[4];
	ozz::math::StorePtrU( m.cols[3], v );
	return b3Vec3{ v[0], v[1], v[2] };
}

b3Quat TurnOf( const ozz::math::Float4x4& m )
{
	float x[4], y[4], z[4];
	ozz::math::StorePtrU( m.cols[0], x );
	ozz::math::StorePtrU( m.cols[1], y );
	ozz::math::StorePtrU( m.cols[2], z );
	Quaternion q = Basis( Vector3( x[0], x[1], x[2] ), Vector3( y[0], y[1], y[2] ), Vector3( z[0], z[1], z[2] ) ).orthonormalized().get_rotation_quaternion();
	return b3Quat{ { float( q.x ), float( q.y ), float( q.z ) }, float( q.w ) };
}

// Bends an arm of a pose so that its hand is at `to` and turned as `turn`, the elbow to the side
// it was: what the game does to the view's arms for an item's offset (CinderboxSkeleton BendArm),
// on the pose itself.
void ReachTo( const anim::AnimSet& set, anim::Models& models, const anim::AnimSet::Arm& arm, b3Vec3 to, b3Quat turn )
{
	if ( arm.upper < 0 || arm.lower < 0 || arm.hand < 0 )
	{
		return;
	}
	b3Vec3 shoulder = PlaceOf( models[size_t( arm.upper )] );
	b3Vec3 elbow = PlaceOf( models[size_t( arm.lower )] );
	b3Vec3 wrist = PlaceOf( models[size_t( arm.hand )] );
	float first = b3Distance( shoulder, elbow );
	float second = b3Distance( elbow, wrist );
	b3Vec3 reach = b3Sub( to, shoulder );
	float far = b3Length( reach );
	if ( first < 1e-4f || second < 1e-4f || far < 1e-4f )
	{
		return;
	}
	float d = std::clamp( far, std::abs( first - second ) + 1e-4f, ( first + second ) * 0.999f );
	b3Vec3 along = b3MulSV( 1.0f / far, reach );
	float ahead = ( first * first - second * second + d * d ) / ( 2.0f * d );
	float out = std::sqrt( std::max( 0.0f, first * first - ahead * ahead ) );
	b3Vec3 pole = b3Sub( elbow, shoulder );
	b3Vec3 side = b3Sub( pole, b3MulSV( b3Dot( pole, along ), along ) );
	side = b3Length( side ) > 1e-4f ? b3Normalize( side ) : b3Vec3{ 0.0f, -1.0f, 0.0f };
	b3Vec3 bent = b3Add( shoulder, b3Add( b3MulSV( ahead, along ), b3MulSV( out, side ) ) );
	anim::RotateSubtreeAbout( set, models, arm.upper, shoulder, anim::Arc( b3Normalize( b3Sub( elbow, shoulder ) ), b3Normalize( b3Sub( bent, shoulder ) ) ) );
	b3Vec3 held = b3Add( shoulder, b3MulSV( d, along ) );
	wrist = PlaceOf( models[size_t( arm.hand )] );
	anim::RotateSubtreeAbout( set, models, arm.lower, bent, anim::Arc( b3Normalize( b3Sub( wrist, bent ) ), b3Normalize( b3Sub( held, bent ) ) ) );
	wrist = PlaceOf( models[size_t( arm.hand )] );
	b3Quat now = TurnOf( models[size_t( arm.hand )] );
	b3Quat fix = b3MulQuat( turn, b3Quat{ { -now.v.x, -now.v.y, -now.v.z }, now.s } );
	anim::RotateSubtreeAbout( set, models, arm.hand, wrist, fix );
}

} // namespace

Dictionary CbItem::preview_view( Node* armsNode, Node* heldNode )
{
	Dictionary out;
	auto* arms = Object::cast_to<CinderboxSkeleton>( armsNode );
	auto* held = Object::cast_to<Node3D>( heldNode );
	if ( m_previewHold == false || m_previewPose == nullptr || m_previewSet == nullptr || m_previewRun == nullptr || arms == nullptr || held == nullptr )
	{
		return out;
	}
	const anim::AnimSet& set = *m_previewSet;
	// The pose as its animations have it, not yet aimed (CinderboxClient::FirstPersonBody, for a
	// viewer that looks level and straight ahead: the body's frame faces +Z, its right is -X).
	PreviewEvaluate( false );
	anim::Models models = m_previewPose->Models();
	int head = anim::FindJoint( set, "Head" );
	int spine = anim::FindJoint( set, "Spine" );
	int shoulders[2] = { anim::FindJoint( set, "LeftShoulder" ), anim::FindJoint( set, "RightShoulder" ) };
	if ( head < 0 || spine < 0 || shoulders[0] < 0 || shoulders[1] < 0 )
	{
		return out;
	}
	const auto& rest = set.RestModels();
	const b3Vec3 forward = { 0.0f, 0.0f, 1.0f }, right = { -1.0f, 0.0f, 0.0f }, up = { 0.0f, 1.0f, 0.0f };
	int aimedFrom = m_previewAim && set.AimJoints().empty() == false ? set.AimJoints().back().first : -1;
	auto between = [&]( const anim::Models& from ) {
		return b3MulSV( 0.5f, b3Add( PlaceOf( from[size_t( shoulders[0] )] ), PlaceOf( from[size_t( shoulders[1] )] ) ) );
	};
	auto pinned = [&]() { return aimedFrom >= 0 ? PlaceOf( models[size_t( aimedFrom )] ) : between( models ); };
	b3Vec3 pinnedAtRest = aimedFrom >= 0 ? PlaceOf( rest[size_t( aimedFrom )] ) : between( rest );
	if ( aimedFrom >= 0 && set.AimTip() >= 0 )
	{
		// Aiming: the whole piece is turned so that the aimed arm is on the line of sight.
		b3Vec3 line = b3Sub( PlaceOf( models[size_t( set.AimTip() )] ), pinned() );
		if ( b3LengthSquared( line ) > 1e-8f )
		{
			anim::RotateSubtreeAbout( set, models, spine, pinned(), anim::Arc( b3Normalize( line ), forward ) );
		}
	}
	// Its held point is where the rest pose has it under the head.
	b3Vec3 restHead = PlaceOf( rest[size_t( head )] );
	b3Vec3 eye = { 0.0f, restHead.y, 0.0f };
	anim::TranslateSubtree( set, models, spine, b3Sub( b3Add( eye, b3Sub( pinnedAtRest, restHead ) ), pinned() ) );

	// The item's place in the view: the hands turned about the carrying one and moved, the arms
	// reaching from shoulders that stay.
	if ( m_viewOffset.is_zero_approx() == false || m_viewRotation.is_zero_approx() == false )
	{
		b3Vec3 placed = b3Add( b3MulSV( float( m_viewOffset.x ), right ), b3Add( b3MulSV( float( m_viewOffset.y ), up ), b3MulSV( float( m_viewOffset.z ), forward ) ) );
		Basis axes( Vector3( right.x, right.y, right.z ), Vector3( up.x, up.y, up.z ), Vector3( -forward.x, -forward.y, -forward.z ) );
		Quaternion turned = ( axes * Basis::from_euler( m_viewRotation * float( Math_PI / 180.0 ) ) * axes.inverse() ).orthonormalized().get_rotation_quaternion();
		b3Quat turn = { { float( turned.x ), float( turned.y ), float( turned.z ) }, float( turned.w ) };
		const anim::AnimSet::Arm& carrying = set.ArmJoints( false );
		if ( carrying.hand >= 0 )
		{
			b3Vec3 about = PlaceOf( models[size_t( carrying.hand )] );
			for ( bool left : { false, true } )
			{
				const anim::AnimSet::Arm& arm = set.ArmJoints( left );
				if ( arm.hand < 0 )
				{
					continue;
				}
				b3Vec3 at = PlaceOf( models[size_t( arm.hand )] );
				b3Vec3 to = b3Add( b3Add( about, b3RotateVector( turn, b3Sub( at, about ) ) ), placed );
				ReachTo( set, models, arm, to, b3MulQuat( turn, TurnOf( models[size_t( arm.hand )] ) ) );
			}
		}
	}

	arms->set_draw_bone_boxes( true );
	arms->set_boxes_arms_only( true, m_viewLeftHand, m_viewRightHand );
	arms->ApplyPose( set, models );
	// The item in the hand: a whole copy of the scene, as in the figure's.
	Transform3D hand;
	if ( arms->JointTransform( "RightHand", hand ) )
	{
		PreviewStage( held, arms->get_transform() * hand * m_previewSocket * CarryFrameUnder( this ).affine_inverse(), true, false );
	}
	// The eye: where the game's camera is for a look level and ahead (get_view_position).
	Transform3D camera( Basis( Vector3( 0, 1, 0 ), float( Math_PI ) ), Vector3( 0.0f, anim::EyeHeight( set ), kEyeAhead ) );
	out["eye"] = arms->get_transform() * camera;
	out["fov"] = m_viewFov > 1.0 ? m_viewFov : 60.0;
	return out;
}

Transform3D CbItem::CarryFrameUnder( const Node* root )
{
	auto* item = Object::cast_to<CbItem>( root );
	auto* grip = item != nullptr && item->m_carryGrip.is_empty() == false ? Object::cast_to<Node3D>( item->get_node_or_null( item->m_carryGrip ) )
																	  : nullptr;
	return grip != nullptr && grip != root ? InSceneFrame( grip, root ).orthonormalized() : Transform3D();
}

PackedStringArray CbItem::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	if ( get_owner() != nullptr )
	{
		warnings.push_back( "A CbItem is the root of an item's own scene: that scene is the item." );
	}
	String kind = m_kind.strip_edges();
	if ( kind.is_empty() || kind.contains( " " ) || kind.contains( "/" ) || kind.contains( "\\" ) )
	{
		warnings.push_back( "Name its kind in one word (melee.bat): the kind a server mod declares. The baked file is named after it." );
	}
	String problem = ShapeProblem( BodyOf( this ) );
	if ( problem.is_empty() == false )
	{
		warnings.push_back( problem );
	}
	// The root is where the game puts the item: its own transform is not part of it.
	if ( get_transform().is_equal_approx( Transform3D() ) == false )
	{
		warnings.push_back( "The item's own transform is not used by the game. To turn the item in the hand, turn its carrying grip." );
	}
	if ( m_carryGrip.is_empty() == false && Object::cast_to<Node3D>( get_node_or_null( m_carryGrip ) ) == nullptr )
	{
		warnings.push_back( "carry_grip names no Node3D of this scene." );
	}
	bool atMarker = m_otherHand == OTHER_AT_MARKER || m_otherHand == OTHER_AT_MARKER_TURNED;
	if ( atMarker && Object::cast_to<Node3D>( get_node_or_null( m_otherGrip ) ) == nullptr )
	{
		warnings.push_back( "The other hand is held at a marker: name it (other_grip), a Node3D of this scene." );
	}
	return warnings;
}

Dictionary CbItem::bake() const
{
	Dictionary out;
	auto fail = [&]( const String& why ) {
		out["text"] = "";
		out["error"] = why;
		return out;
	};
	String kind = m_kind.strip_edges();
	if ( kind.is_empty() || kind.contains( " " ) || kind.contains( "/" ) || kind.contains( "\\" ) || kind.contains( "\t" ) )
	{
		return fail( "name its kind in one word (melee.bat)" );
	}
	const bool simple = OneSimpleShape( this );
	const CollisionShape3D* body = BodyOf( this );
	String problem = simple ? String() : ShapeProblem( body );
	if ( problem.is_empty() == false )
	{
		return fail( problem );
	}
	Vector3 half;
	String shapeName = "box";
	if ( simple )
	{
		if ( auto* box = Object::cast_to<BoxShape3D>( body->get_shape().ptr() ) )
		{
			half = box->get_size() * 0.5f;
		}
		else
		{
			float radius = Object::cast_to<SphereShape3D>( body->get_shape().ptr() )->get_radius();
			half = Vector3( radius, radius, radius );
			shapeName = "sphere";
		}
	}
	if ( m_carryGrip.is_empty() == false && Object::cast_to<Node3D>( get_node_or_null( m_carryGrip ) ) == nullptr )
	{
		return fail( "carry_grip names no Node3D of this scene" );
	}
	const bool atMarker = m_otherHand == OTHER_AT_MARKER || m_otherHand == OTHER_AT_MARKER_TURNED;
	auto* other = Object::cast_to<Node3D>( get_node_or_null( m_otherGrip ) );
	if ( atMarker && other == nullptr )
	{
		return fail( "the other hand is held at a marker: name it (other_grip), a Node3D of this scene" );
	}
	// Everything is written in the frame the item is carried in.
	const Transform3D toCarried = CarryFrameUnder( this ).affine_inverse();
	String text = "# Baked from the item's CbItem (its scene's root). Edit the scene and bake again.\n";
	Quaternion turn;
	double volume = 0.0;
	if ( simple )
	{
		const Transform3D inCarried = ( toCarried * InSceneFrame( body, this ) ).orthonormalized();
		Vector3 center = inCarried.origin;
		turn = inCarried.basis.get_rotation_quaternion();
		text += "shape " + shapeName + "\n";
		text += vformat( "half %.4f %.4f %.4f\n", half.x, half.y, half.z );
		text += vformat( "center %.4f %.4f %.4f\n", center.x, center.y, center.z );
		volume = shapeName == "sphere" ? 4.18879 * half.x * half.x * half.x : 8.0 * half.x * half.y * half.z;
	}
	else
	{
		// A body of several shapes (or of one a box or ball cannot say): each a part, where it is in
		// the frame the item is carried in, its size with the scale above it.
		Array warnings;
		int parts = 0;
		for ( const CollisionShape3D* node : ShapesOf( this ) )
		{
			BakedShape baked;
			String why = BakeShape( node, toCarried * InSceneFrame( node, this ), node->get_name(), baked, warnings );
			if ( why.is_empty() && ( baked.kind == ShapeKind::Mesh || baked.kind == ShapeKind::HeightField ) )
			{
				why = "a concave polygon or a height map is level geometry only: Box3D moves no body of one";
			}
			if ( why.is_empty() == false )
			{
				return fail( String( node->get_name() ) + ": " + why );
			}
			static const char* const kKinds[] = { "box", "sphere", "capsule", "hull" };
			const Vector3 h = baked.half;
			const Vector3 c = baked.at.origin;
			const Quaternion q = baked.at.basis.get_rotation_quaternion();
			String line = vformat( "part %s ", kKinds[int( baked.kind )] );
			if ( baked.kind == ShapeKind::Hull )
			{
				if ( baked.points.size() < 4 || baked.points.size() > kItemHullPointLimit )
				{
					return fail( String( node->get_name() ) + vformat( ": a hull needs 4 to %d points", int( kItemHullPointLimit ) ) );
				}
				Vector3 bounds( 0.005f, 0.005f, 0.005f );
				for ( const Vector3& point : baked.points )
				{
					bounds = bounds.max( point.abs() );
				}
				line += vformat( "%.4f %.4f %.4f ", bounds.x, bounds.y, bounds.z );
				volume += 8.0 * bounds.x * bounds.y * bounds.z * 0.5; // (about half its bounds)
			}
			else
			{
				line += vformat( "%.4f %.4f %.4f ", h.x, h.y, h.z );
				volume += baked.kind == ShapeKind::Box		? 8.0 * h.x * h.y * h.z
						  : baked.kind == ShapeKind::Sphere ? 4.18879 * h.x * h.x * h.x
															: 3.14159 * h.x * h.x * ( 2.0 * h.y + 4.18879 / 3.14159 * h.x );
			}
			line += vformat( "%.4f %.4f %.4f %.5f %.5f %.5f %.5f", c.x, c.y, c.z, q.x, q.y, q.z, q.w );
			if ( baked.kind == ShapeKind::Hull )
			{
				line += vformat( " %d", int( baked.points.size() ) );
				for ( const Vector3& point : baked.points )
				{
					line += vformat( " %.4f %.4f %.4f", point.x, point.y, point.z );
				}
			}
			text += line + "\n";
			parts += 1;
		}
		if ( parts == 0 )
		{
			return fail( ShapeProblem( nullptr ) );
		}
		if ( parts > int( kItemPartLimit ) )
		{
			return fail( vformat( "an item's body has at most %d shapes", int( kItemPartLimit ) ) );
		}
	}
	// Its body, as any body says it: a weight (its own, or its density's worth), and how it grips
	// and bounces when they are not the usual.
	text += vformat( "mass %.3f\n", MassFor( volume ) );
	if ( m_friction != 0.6 )
	{
		text += vformat( "friction %.3f\n", m_friction );
	}
	if ( m_bounce != 0.0 )
	{
		text += vformat( "bounce %.3f\n", m_bounce );
	}
	if ( m_rolling != 0.0 )
	{
		text += vformat( "rolling %.3f\n", m_rolling );
	}
	if ( m_drag != 0.0 )
	{
		text += vformat( "drag %.3f\n", m_drag );
	}
	if ( turn.is_equal_approx( Quaternion() ) == false )
	{
		text += vformat( "turn %.5f %.5f %.5f %.5f\n", turn.x, turn.y, turn.z, turn.w );
	}
	// Sorted, so the same scene always bakes the same file (the item's hash depends on it). How it
	// is used and where it goes are properties the engine reads; said here, they are the item's.
	Dictionary all = m_properties.duplicate();
	if ( m_use == USE_PRESS || m_use == USE_HOLD )
	{
		all["use"] = m_use;
	}
	if ( m_slot > 0 )
	{
		all["slot"] = m_slot;
	}
	if ( m_hand == HAND_LEFT )
	{
		all["hand"] = m_hand;
	}
	if ( m_bullet )
	{
		all["bullet"] = 1;
	}
	if ( m_breakSpeed > 0.0 )
	{
		all["break_speed"] = m_breakSpeed;
		all["break_hole"] = m_breakHole;
		all["break_through"] = m_breakThrough;
	}
	if ( m_slotKeyUses )
	{
		if ( m_use == USE_TOOL )
		{
			return fail( "slot_key_uses is for an item used once a press or while held (use)" );
		}
		all["slot_key_uses"] = 1;
	}
	if ( m_stack > 1 )
	{
		all["stack"] = m_stack;
	}
	Array names = all.keys();
	names.sort();
	for ( int i = 0; i < names.size(); ++i )
	{
		String name = String( names[i] ).strip_edges();
		Variant value = all[names[i]];
		bool number = value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT;
		if ( name.is_empty() || name.contains( " " ) || name.contains( "\t" ) || number == false || std::isfinite( double( value ) ) == false )
		{
			return fail( "property \"" + name + "\" needs a name without spaces and a number" );
		}
		text += "property " + name + " " + String::num( double( value ), 4 ) + "\n";
	}
	// Its own fields, sorted: "field gun.rounds int 30".
	Array fieldNames = m_fields.keys();
	fieldNames.sort();
	for ( int i = 0; i < fieldNames.size(); ++i )
	{
		String name = String( fieldNames[i] ).strip_edges();
		Variant value = m_fields[fieldNames[i]];
		const char* type = value.get_type() == Variant::BOOL ? "bool" : value.get_type() == Variant::INT ? "int" : value.get_type() == Variant::FLOAT ? "float" : nullptr;
		if ( name.is_empty() || name.contains( " " ) || name.contains( "\t" ) || type == nullptr || std::isfinite( double( value ) ) == false )
		{
			return fail( "field \"" + name + "\" needs a name without spaces and a value: a whole number, a decimal, or true / false" );
		}
		text += "field " + name + " " + String( type ) + " " + String::num( double( value ), 4 ) + "\n";
	}
	// What the other hand does: at the marker (0), turned with it (1), or as the animations have it
	// (2; no place of its own).
	if ( m_otherHand != OTHER_FREE )
	{
		Transform3D t = atMarker ? ( toCarried * InSceneFrame( other, this ) ).orthonormalized() : Transform3D();
		Quaternion q = t.basis.get_rotation_quaternion();
		text += vformat( "grip %.4f %.4f %.4f %.5f %.5f %.5f %.5f %d\n", t.origin.x, t.origin.y, t.origin.z, q.x, q.y, q.z, q.w,
						 m_otherHand == OTHER_AS_ANIMATED ? 2 : ( m_otherHand == OTHER_AT_MARKER_TURNED ? 1 : 0 ) );
	}
	// What a mod that takes its items from its look reads besides: the pack, and the holster.
	if ( m_animationPack.is_empty() == false && m_animationPack.contains( " " ) == false )
	{
		text += "pack " + m_animationPack + "\n";
	}
	if ( m_holster.is_empty() == false && m_holster.contains( " " ) == false )
	{
		text += "holster " + m_holster + "\n";
	}
	if ( m_useKey.is_empty() == false )
	{
		if ( m_useKey.contains( " " ) )
		{
			return fail( "use_key is one key (H)" );
		}
		text += "usekey " + m_useKey + "\n";
	}
	// Its own HUD, for the game (the server skips these lines).
	if ( m_hud.is_empty() == false )
	{
		if ( m_hud.begins_with( "res://" ) == false || m_hud.contains( " " ) )
		{
			return fail( "hud is a scene of this project (res://ui/my_hud.tscn)" );
		}
		text += "hud " + m_hud + "\n";
		text += String( "hudshows " ) + ( m_hudShows == HUD_CARRIED ? "carried" : "hand" ) + "\n";
	}
	// Its own motions (CbMotion nodes under it): a set named like the kind, baked beside the item.
	{
		String motionError;
		if ( BakeMotions( motionError ).empty() == false )
		{
			text += "motions 1\n";
		}
		else if ( motionError.is_empty() == false )
		{
			return fail( motionError );
		}
	}
	// What the game reads (the server skips these): which scene the kind is drawn as, what it is
	// called, and how its holder's arms sit in first person.
	String scene = get_scene_file_path();
	if ( scene.is_empty() == false )
	{
		text += "scene " + scene + "\n";
	}
	String shown = m_displayName.strip_edges().replace( "\n", " " );
	if ( shown.is_empty() == false )
	{
		text += "name " + shown + "\n";
	}
	if ( m_viewOffset.is_zero_approx() == false )
	{
		text += vformat( "view %.4f %.4f %.4f\n", m_viewOffset.x, m_viewOffset.y, m_viewOffset.z );
	}
	if ( m_viewRotation.is_zero_approx() == false )
	{
		text += vformat( "viewturn %.2f %.2f %.2f\n", m_viewRotation.x, m_viewRotation.y, m_viewRotation.z );
	}
	double fov = m_viewFov;
	if ( m_viewLeftHand == false || m_viewRightHand == false )
	{
		text += vformat( "viewhands %d %d\n", m_viewLeftHand ? 1 : 0, m_viewRightHand ? 1 : 0 );
	}
	// Where the viewer's eye is against the item while it floats in the first-person view (the
	// scene's frame: the game moves the scene's root).
	if ( m_viewCamera.is_empty() == false )
	{
		auto* eye = Object::cast_to<Node3D>( get_node_or_null( m_viewCamera ) );
		if ( eye == nullptr || eye == this )
		{
			return fail( "view_camera is not a Node3D in the item's scene" );
		}
		Transform3D t = InSceneFrame( eye, this ).orthonormalized();
		Quaternion q = t.basis.get_rotation_quaternion();
		text += vformat( "viewfrom %.4f %.4f %.4f %.5f %.5f %.5f %.5f\n", t.origin.x, t.origin.y, t.origin.z, q.x, q.y, q.z, q.w );
		// A camera's field of view is the view model's (what its Preview shows is what is drawn),
		// unless the item says its own.
		if ( auto* camera = Object::cast_to<Camera3D>( eye ); camera != nullptr && fov <= 0.0 )
		{
			fov = camera->get_fov();
		}
	}
	if ( fov > 0.0 )
	{
		text += vformat( "viewfov %.2f\n", fov );
	}
	out["text"] = text;
	out["error"] = "";
	return out;
}

Dictionary CbItem::bake_motions() const
{
	String error;
	std::string text = BakeMotions( error );
	Dictionary out;
	out["text"] = String::utf8( text.c_str() );
	out["error"] = error;
	return out;
}

std::string CbItem::BakeMotions( String& error ) const
{
	String kind = m_kind.strip_edges();
	std::string text = "cinderbox_motions\t2\n# Baked from the CbMotion nodes of the CbItem " + std::string( kind.utf8().get_data() ) +
					   ". Edit the item's scene and bake again.\n";
	int count = 0;
	// What using it does: the helper nodes under it, as one motion ("Use"); and a "use" rule of its
	// rules sheets (a CbRules under it), as the same: its conditions and changes are added to the When
	// used ones, its cooldown and emit say them, its parts are motion nodes made for the bake.
	PackedStringArray conditions = m_useConditions;
	PackedStringArray changes = m_useChanges;
	double cooldown = m_useCooldown;
	String emits = m_useEmits;
	std::vector<Node*> made;
	int uses = 0;
	TypedArray<Node> sheets = find_children( "*", "CbRules", true, false );
	for ( int64_t s = 0; s < sheets.size(); ++s )
	{
		String file = Object::cast_to<Node>( sheets[s] )->get( "file" );
		if ( file.is_empty() || FileAccess::file_exists( file ) == false )
		{
			continue;
		}
		PackedStringArray problems;
		for ( const cue::Rule& rule : cue::ParseRules( FileAccess::get_file_as_string( file ), problems ) )
		{
			if ( rule.type != "use" )
			{
				continue;
			}
			if ( ++uses > 1 )
			{
				error = file + " line " + String::num_int64( rule.line ) + ": an item has one use rule";
				break;
			}
			const Dictionary& p = rule.properties;
			conditions.append_array( p.get( "conditions", PackedStringArray() ) );
			changes.append_array( p.get( "changes", PackedStringArray() ) );
			cooldown = p.has( "cooldown" ) ? double( p["cooldown"] ) : cooldown;
			if ( p.has( "emits" ) && emits.is_empty() == false && emits != String( p["emits"] ) )
			{
				error = file + " line " + String::num_int64( rule.line ) + ": the item's use_emits says " + emits + " already";
				break;
			}
			emits = p.has( "emits" ) ? String( p["emits"] ) : emits;
			Array parts = p.get( "parts", Array() );
			for ( int64_t i = 0; i < parts.size(); ++i )
			{
				Dictionary part = parts[i];
				Node* node = Object::cast_to<Node>( ClassDB::instantiate( StringName( String( part["class"] ) ) ) );
				if ( node == nullptr )
				{
					continue;
				}
				Dictionary props = part["properties"];
				Array keys = props.keys();
				for ( int64_t k = 0; k < keys.size(); ++k )
				{
					node->set( StringName( String( keys[k] ) ), props[keys[k]] );
				}
				made.push_back( node );
			}
		}
	}
	// (Under the item for the bake, as a part placed there by hand would be; never owned, so never
	// saved, and taken out again at once.)
	CbItem* self = const_cast<CbItem*>( this );
	for ( Node* node : made )
	{
		self->add_child( node );
	}
	std::string use;
	if ( error.is_empty() )
	{
		use = BakeItemUse( this, kind, m_use, conditions, changes, cooldown, emits, error );
	}
	for ( Node* node : made )
	{
		self->remove_child( node );
		memdelete( node );
	}
	if ( use.empty() && error.is_empty() == false )
	{
		return std::string();
	}
	text += use;
	count += use.empty() ? 0 : 1;
	for ( int i = 0; i < get_child_count(); ++i )
	{
		auto* motion = Object::cast_to<CbMotion>( get_child( i ) );
		if ( motion == nullptr )
		{
			continue;
		}
		std::string lines = motion->BakeWith( error, motion->ItemCondition( kind ) );
		if ( lines.empty() )
		{
			return std::string();
		}
		text += lines;
		count += 1;
	}
	return count > 0 ? text : std::string();
}

void CbItem::bake_to_project()
{
	if ( get_scene_file_path().is_empty() )
	{
		UtilityFunctions::push_error( "Item bake: save the item's scene first." );
		return;
	}
	Dictionary baked = bake();
	if ( String( baked["text"] ).is_empty() )
	{
		UtilityFunctions::push_error( "Item bake failed (", get_scene_file_path(), "): ", baked["error"] );
		return;
	}
	DirAccess::make_dir_recursive_absolute( "res://items" );
	String path = "res://items/" + m_kind.strip_edges() + ".cfg";
	PackedByteArray bytes = String( baked["text"] ).to_utf8_buffer();
	BakeMotionsToProject();
	// A bake that changes nothing touches nothing: it runs on every save and every publish.
	if ( FileAccess::file_exists( path ) && FileAccess::get_file_as_bytes( path ) == bytes )
	{
		return;
	}
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::WRITE );
	if ( file.is_null() )
	{
		UtilityFunctions::push_error( "Item bake: cannot write ", path );
		return;
	}
	file->store_buffer( bytes );
	UtilityFunctions::print( "Baked item ", m_kind, " into ", path, ". Publish the mod for servers to get it." );
}

void CbItem::BakeMotionsToProject() const
{
	// (After the item itself: a failed bake of the item wrote nothing, and its motions failed with it.)
	String error;
	std::string text = BakeMotions( error );
	String path = "res://motions/" + m_kind.strip_edges() + ".cfg";
	if ( text.empty() )
	{
		// None any more: the file an earlier bake of this item left goes (never a CbMotionSet's).
		if ( error.is_empty() && FileAccess::file_exists( path ) && FileAccess::get_file_as_string( path ).contains( "of the CbItem " ) )
		{
			DirAccess::remove_absolute( path );
		}
		return;
	}
	DirAccess::make_dir_recursive_absolute( "res://motions" );
	PackedByteArray bytes = String::utf8( text.c_str() ).to_utf8_buffer();
	if ( FileAccess::file_exists( path ) && FileAccess::get_file_as_bytes( path ) == bytes )
	{
		return;
	}
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::WRITE );
	if ( file.is_null() )
	{
		UtilityFunctions::push_error( "Item bake: cannot write ", path );
		return;
	}
	file->store_buffer( bytes );
	UtilityFunctions::print( "Baked item ", m_kind, "'s motions into ", path );
}

} // namespace cb::gd
