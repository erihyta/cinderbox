#include "cinderbox_client.h"

#include "cinderbox_item_look.h"
#include "motions.h"
#include "ragdoll.h"
#include "joint_math.h"

#include "camera.h"
#include "cinderbox_character.h"
#include "cinderbox_track_player.h"
#include "cinderbox_skeleton.h"
#include "cue_guard.h"
#include "cue_prediction.h"
#include "detmath.h"
#include "object_source.h"
#include "pose_tools.h"
#include "view_file.h"
#include "types.h"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/bone_attachment3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_map.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

using namespace godot;

namespace cb::gd
{

namespace
{

const Color kSlotColors[] = {
	Color( 0.90f, 0.31f, 0.27f ), Color( 0.27f, 0.55f, 0.90f ), Color( 0.35f, 0.78f, 0.43f ), Color( 0.78f, 0.47f, 0.86f ),
	Color( 0.94f, 0.63f, 0.24f ), Color( 0.24f, 0.78f, 0.78f ), Color( 0.71f, 0.71f, 0.35f ), Color( 0.59f, 0.43f, 0.31f ),
};

Vector3 ToGodot( b3Vec3 v )
{
	return Vector3( v.x, v.y, v.z );
}

Quaternion ToGodot( b3Quat q )
{
	return Quaternion( q.v.x, q.v.y, q.v.z, q.s );
}

const char* KindName( present::VisualKind kind )
{
	switch ( kind )
	{
		case present::VisualKind::Static:
			return "static";
		case present::VisualKind::Player:
			return "player";
		case present::VisualKind::Ragdoll:
			return "ragdoll";
		case present::VisualKind::Item:
			return "item";
		case present::VisualKind::Prop:
		default:
			return "prop";
	}
}

std::string ToStd( const String& s )
{
	CharString utf8 = s.utf8();
	return std::string( utf8.get_data(), size_t( utf8.length() ) );
}

template <typename T>
T* FindInPrefab( Node* node )
{
	if ( auto* found = Object::cast_to<T>( node ) )
	{
		return found;
	}
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		if ( auto* found = FindInPrefab<T>( node->get_child( i ) ) )
		{
			return found;
		}
	}
	return nullptr;
}

CinderboxSkeleton* FindSkeleton( Node* node )
{
	return FindInPrefab<CinderboxSkeleton>( node );
}

} // namespace

CinderboxClient::CinderboxClient() = default;

CinderboxClient::~CinderboxClient() = default;

void CinderboxClient::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_source", "source" ), &CinderboxClient::set_source );
	ClassDB::bind_method( D_METHOD( "open_view", "path" ), &CinderboxClient::open_view );
	ClassDB::bind_method( D_METHOD( "stop" ), &CinderboxClient::stop );
	ClassDB::bind_method( D_METHOD( "is_running" ), &CinderboxClient::is_running );
	ClassDB::bind_method( D_METHOD( "control", "name", "value" ), &CinderboxClient::control );
	ClassDB::bind_method( D_METHOD( "takes_input" ), &CinderboxClient::takes_input );
	ClassDB::bind_method( D_METHOD( "send_intent", "kind", "a", "b" ), &CinderboxClient::send_intent );
	ClassDB::bind_method( D_METHOD( "set_local_value", "name", "value" ), &CinderboxClient::set_local_value );
	ClassDB::bind_method( D_METHOD( "get_local_value", "name" ), &CinderboxClient::get_local_value );
	ClassDB::bind_method( D_METHOD( "want_cursor", "who", "wanted" ), &CinderboxClient::want_cursor );
	ClassDB::bind_method( D_METHOD( "wants_cursor" ), &CinderboxClient::wants_cursor );
	ClassDB::bind_method( D_METHOD( "get_slot_count", "player" ), &CinderboxClient::get_slot_count );
	ClassDB::bind_method( D_METHOD( "get_selected_slot", "player" ), &CinderboxClient::get_selected_slot );
	ClassDB::bind_method( D_METHOD( "get_slot_item", "player", "slot" ), &CinderboxClient::get_slot_item );
	ClassDB::bind_method( D_METHOD( "set_input", "move", "camera_yaw", "camera_pitch", "jump", "sprint", "use", "actions", "view" ),
						  &CinderboxClient::set_input, DEFVAL( 0 ) );
	ClassDB::bind_method( D_METHOD( "get_actions" ), &CinderboxClient::get_actions );
	ClassDB::bind_method( D_METHOD( "get_mod_names" ), &CinderboxClient::get_mod_names );
	ClassDB::bind_method( D_METHOD( "get_field", "net_id", "name" ), &CinderboxClient::get_field );
	ClassDB::bind_method( D_METHOD( "get_local_field", "name" ), &CinderboxClient::get_local_field );
	ClassDB::bind_method( D_METHOD( "check_conditions", "net_id", "conditions" ), &CinderboxClient::check_conditions );
	ClassDB::bind_method( D_METHOD( "check_local_conditions", "conditions" ), &CinderboxClient::check_local_conditions );
	ClassDB::bind_method( D_METHOD( "evaluate", "net_id", "expression" ), &CinderboxClient::evaluate );
	ClassDB::bind_method( D_METHOD( "evaluate_local", "expression" ), &CinderboxClient::evaluate_local );
	ClassDB::bind_method( D_METHOD( "format_local_fields", "format" ), &CinderboxClient::format_local_fields );
	ClassDB::bind_method( D_METHOD( "get_local_net_id" ), &CinderboxClient::get_local_net_id );
	ClassDB::bind_method( D_METHOD( "is_local_player_dead" ), &CinderboxClient::is_local_player_dead );
	ClassDB::bind_method( D_METHOD( "get_tick_time" ), &CinderboxClient::get_tick_time );
	ClassDB::bind_method( D_METHOD( "get_tick_rate" ), &CinderboxClient::get_tick_rate );
	ClassDB::bind_method( D_METHOD( "get_camera_target" ), &CinderboxClient::get_camera_target );
	ClassDB::bind_method( D_METHOD( "get_view_position", "view", "camera" ), &CinderboxClient::get_view_position );
	ClassDB::bind_method( D_METHOD( "set_first_person", "value" ), &CinderboxClient::set_first_person );
	ClassDB::bind_method( D_METHOD( "get_first_person" ), &CinderboxClient::get_first_person );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "first_person", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE ), "set_first_person",
				  "get_first_person" );
	ClassDB::bind_method( D_METHOD( "get_camera_distance", "target", "direction", "max_distance", "radius" ),
						  &CinderboxClient::get_camera_distance );
	ClassDB::bind_method( D_METHOD( "get_bone_position", "net_id", "bone" ), &CinderboxClient::get_bone_position );
	ClassDB::bind_method( D_METHOD( "get_kind", "net_id" ), &CinderboxClient::get_kind );
	ClassDB::bind_method( D_METHOD( "get_entity_template_name", "net_id" ), &CinderboxClient::get_entity_template_name );
	ClassDB::bind_method( D_METHOD( "get_entity_node", "net_id" ), &CinderboxClient::get_entity_node );
	ClassDB::bind_method( D_METHOD( "get_players" ), &CinderboxClient::get_players );
	ClassDB::bind_method( D_METHOD( "add_item", "kind", "config" ), &CinderboxClient::add_item );
	ClassDB::bind_method( D_METHOD( "get_items", "kind", "holder" ), &CinderboxClient::get_items );
	ClassDB::bind_method( D_METHOD( "get_entity_name", "net_id" ), &CinderboxClient::get_entity_name );
	ClassDB::bind_method( D_METHOD( "get_player_name", "net_id" ), &CinderboxClient::get_player_name );
	ClassDB::bind_method( D_METHOD( "format_fields", "net_id", "format" ), &CinderboxClient::format_fields );
	ClassDB::bind_method( D_METHOD( "get_required_items" ), &CinderboxClient::get_required_items );
	ClassDB::bind_method( D_METHOD( "get_character" ), &CinderboxClient::get_character );
	ClassDB::bind_method( D_METHOD( "use_character", "name" ), &CinderboxClient::use_character );
	ClassDB::bind_method( D_METHOD( "add_world_scene", "scene" ), &CinderboxClient::add_world_scene );
	ClassDB::bind_method( D_METHOD( "clear_world_scenes" ), &CinderboxClient::clear_world_scenes );
	ClassDB::bind_method( D_METHOD( "get_director" ), &CinderboxClient::get_director );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &CinderboxClient::get_stats );
	ClassDB::bind_method( D_METHOD( "get_source_state" ), &CinderboxClient::get_source_state );
	ClassDB::bind_method( D_METHOD( "has_local_player" ), &CinderboxClient::has_local_player );
	ClassDB::bind_method( D_METHOD( "get_local_player_position" ), &CinderboxClient::get_local_player_position );
	ClassDB::bind_method( D_METHOD( "get_visual_node", "visual_id" ), &CinderboxClient::get_visual_node );

	ClassDB::bind_method( D_METHOD( "set_map_dir", "dir" ), &CinderboxClient::set_map_dir );
	ClassDB::bind_method( D_METHOD( "get_map_dir" ), &CinderboxClient::get_map_dir );
	ClassDB::bind_method( D_METHOD( "get_map_name" ), &CinderboxClient::get_map_name );
	ClassDB::bind_method( D_METHOD( "set_prefab_dir", "dir" ), &CinderboxClient::set_prefab_dir );
	ClassDB::bind_method( D_METHOD( "get_prefab_dir" ), &CinderboxClient::get_prefab_dir );

	ADD_PROPERTY( PropertyInfo( Variant::STRING, "prefab_dir", PROPERTY_HINT_DIR ), "set_prefab_dir", "get_prefab_dir" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "map_dir", PROPERTY_HINT_DIR ), "set_map_dir", "get_map_dir" );

	// visual_id is stable for the whole life of a visual (including its destroy effect).
	ADD_SIGNAL( MethodInfo( "visual_spawned", PropertyInfo( Variant::INT, "visual_id" ), PropertyInfo( Variant::INT, "net_id" ),
							PropertyInfo( Variant::STRING, "kind" ), PropertyInfo( Variant::OBJECT, "node" ),
							PropertyInfo( Variant::VECTOR3, "position" ), PropertyInfo( Variant::BOOL, "with_effect" ),
							PropertyInfo( Variant::STRING, "template_name" ) ) );
	ADD_SIGNAL( MethodInfo( "visual_destroying", PropertyInfo( Variant::INT, "visual_id" ), PropertyInfo( Variant::INT, "net_id" ),
							PropertyInfo( Variant::STRING, "kind" ), PropertyInfo( Variant::VECTOR3, "position" ),
							PropertyInfo( Variant::STRING, "template_name" ) ) );
	ADD_SIGNAL( MethodInfo( "visual_removed", PropertyInfo( Variant::INT, "visual_id" ) ) );
	ADD_SIGNAL( MethodInfo( "player_jumped", PropertyInfo( Variant::INT, "net_id" ), PropertyInfo( Variant::VECTOR3, "position" ),
							PropertyInfo( Variant::BOOL, "is_local" ) ) );
	ADD_SIGNAL( MethodInfo( "player_landed", PropertyInfo( Variant::INT, "net_id" ), PropertyInfo( Variant::VECTOR3, "position" ),
							PropertyInfo( Variant::BOOL, "is_local" ) ) );
	ADD_SIGNAL( MethodInfo( "footstep", PropertyInfo( Variant::INT, "net_id" ), PropertyInfo( Variant::VECTOR3, "position" ),
							PropertyInfo( Variant::BOOL, "is_local" ) ) );
	// strength is the approach speed in m/s, so an effect can be chosen by how hard the hit was.
	ADD_SIGNAL( MethodInfo( "impact", PropertyInfo( Variant::INT, "net_id" ), PropertyInfo( Variant::VECTOR3, "position" ),
							PropertyInfo( Variant::FLOAT, "strength" ), PropertyInfo( Variant::STRING, "kind" ),
							PropertyInfo( Variant::STRING, "template_name" ) ) );
	ADD_SIGNAL( MethodInfo( "source_state_changed", PropertyInfo( Variant::STRING, "state" ) ) );
	// The server's mods changed (a first join, or a different server): actions and fields to rebind.
	ADD_SIGNAL( MethodInfo( "schema_changed" ) );
	// A server mod announced something. a is who it is about, b the other entity (0 if none).
	ADD_SIGNAL( MethodInfo( "mod_event", PropertyInfo( Variant::STRING, "name" ), PropertyInfo( Variant::INT, "net_id_a" ),
							PropertyInfo( Variant::INT, "net_id_b" ), PropertyInfo( Variant::INT, "value" ),
							PropertyInfo( Variant::VECTOR3, "position" ), PropertyInfo( Variant::VECTOR3, "vector" ) ) );
	// The local player just pressed a mod action; on a live connection, before the server answered.
	ADD_SIGNAL( MethodInfo( "action_pressed", PropertyInfo( Variant::STRING, "name" ) ) );
	// Someone joined, left or was renamed.
	ADD_SIGNAL( MethodInfo( "names_changed" ) );
}

void CinderboxClient::EnsureAnimations()
{
	if ( m_animSet )
	{
		return;
	}
	m_animSet = anim::AnimSet::CreateProcedural();
}

void CinderboxClient::Open( std::unique_ptr<present::ViewSource> source )
{
	m_source.reset();
	EnsureAnimations();
	if ( !m_mirror )
	{
		m_mirror = std::make_unique<present::Mirror>( m_animSet );
	}
	m_frame = present::ViewFrame();
	m_haveFrame = false;
	m_lastActions = 0;
	m_sourceCount += 1;
	m_source = std::move( source );
}

void CinderboxClient::set_source( const Variant& source )
{
	if ( Engine::get_singleton()->is_editor_hint() )
	{
		return;
	}
	if ( source.get_validated_object() == nullptr )
	{
		UtilityFunctions::push_warning( "Cinderbox: set_source needs an object (see object_source.h); use stop() to drop one" );
		return;
	}
	Open( std::make_unique<ObjectSource>( source ) );
}

void CinderboxClient::open_view( const String& path )
{
	if ( Engine::get_singleton()->is_editor_hint() )
	{
		return;
	}
	Open( std::make_unique<present::ViewFileSource>( ToStd( ProjectSettings::get_singleton()->globalize_path( path ) ) ) );
}

void CinderboxClient::stop()
{
	m_source.reset();
}

bool CinderboxClient::is_running() const
{
	return m_source != nullptr;
}

void CinderboxClient::control( const String& name, double value )
{
	if ( m_source )
	{
		m_source->Control( ToStd( name ), value );
	}
}

bool CinderboxClient::takes_input() const
{
	return m_source != nullptr && m_source->TakesInput();
}

void CinderboxClient::_exit_tree()
{
	m_source.reset();
}

void CinderboxClient::_enter_tree()
{
	// HUD nodes (CbFieldLabel) find the client through this group.
	add_to_group( "cinderbox_client" );
}

void CinderboxClient::set_local_value( const String& name, double value )
{
	std::string key = ToStd( name.strip_edges() );
	if ( key.rfind( "ui.", 0 ) == 0 && key.size() > 3 && key.size() <= 64 && ( m_localValues.count( key ) != 0 || m_localValues.size() < 256 ) )
	{
		m_localValues[key] = value;
	}
}

double CinderboxClient::get_local_value( const String& name ) const
{
	auto found = m_localValues.find( ToStd( name.strip_edges() ) );
	return found != m_localValues.end() ? found->second : 0.0;
}

void CinderboxClient::want_cursor( int64_t who, bool wanted )
{
	auto found = std::find( m_cursorWanters.begin(), m_cursorWanters.end(), who );
	if ( wanted && found == m_cursorWanters.end() )
	{
		m_cursorWanters.push_back( who );
	}
	else if ( wanted == false && found != m_cursorWanters.end() )
	{
		m_cursorWanters.erase( found );
	}
}

bool CinderboxClient::wants_cursor() const
{
	// (A screen that was freed without saying so no longer asks.)
	for ( int64_t who : m_cursorWanters )
	{
		if ( ObjectDB::get_instance( ObjectID( uint64_t( who ) ) ) != nullptr )
		{
			return true;
		}
	}
	return false;
}

int64_t CinderboxClient::get_slot_count( int64_t player ) const
{
	flecs::entity ve = m_mirror ? m_mirror->VisualOf( uint32_t( player ) ) : flecs::entity();
	return ve.is_valid() ? int64_t( ve.get<present::Visual>().slotCount ) : 0;
}

int64_t CinderboxClient::get_selected_slot( int64_t player ) const
{
	flecs::entity ve = m_mirror ? m_mirror->VisualOf( uint32_t( player ) ) : flecs::entity();
	uint8_t selected = ve.is_valid() ? ve.get<present::Visual>().slotIndex : kNoSlot;
	return ve.is_valid() && selected < ve.get<present::Visual>().slotCount ? int64_t( selected ) : -1;
}

int64_t CinderboxClient::get_slot_item( int64_t player, int64_t slot ) const
{
	int64_t found = 0;
	if ( !m_mirror || player == 0 || slot < 0 || slot >= kMaxSlots )
	{
		return 0;
	}
	m_mirror->ForEach( [&]( uint64_t, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		if ( v.kind == present::VisualKind::Item && int64_t( v.holder ) == player && int64_t( v.slotIndex ) == slot )
		{
			found = int64_t( v.netId );
		}
	} );
	return found;
}

void CinderboxClient::send_intent( int64_t kind, int64_t a, int64_t b )
{
	if ( kind >= 1 && kind <= int64_t( kLastSlotIntent ) && a >= 0 && a <= 255 && b >= 0 && b <= 255 && m_intents.size() < 16 )
	{
		m_intents.push_back( { uint8_t( kind ), uint8_t( a ), uint8_t( b ) } );
	}
}

void CinderboxClient::set_input( const Vector2& move, double camera_yaw, double camera_pitch, bool jump, bool sprint,
								 bool use, int64_t actions, int64_t view )
{
	if ( takes_input() == false )
	{
		return;
	}
	// Godot's camera yaw: 0 looks down -Z. The simulation's: 0 looks down +Z, positive turns left
	// (toward +X). A Godot camera with rotation.y = r looks along (-sin r, 0, -cos r), which is
	// the simulation's yaw r + pi.
	PlayerInput in;
	in.moveRight = int8_t( std::clamp( int( std::lround( move.x * 127.0 ) ), -127, 127 ) );
	in.moveForward = int8_t( std::clamp( int( std::lround( move.y * 127.0 ) ), -127, 127 ) );
	in.cameraYaw = detmath::RadiansToYaw( float( camera_yaw ) + detmath::kPi );
	in.buttons = uint8_t( ( jump ? BtnJump : 0 ) | ( sprint ? BtnSprint : 0 ) | ( use ? BtnUse : 0 ) );
	// Pitch: a positive Godot rotation.x looks up, which is the simulation's convention too.
	double pitchTurns = std::clamp( camera_pitch / ( 2.0 * detmath::kPi ), -0.24, 0.24 );
	in.cameraPitch = int16_t( std::clamp( int( std::lround( pitchTurns * 65536.0 ) ), -int( kMaxCameraPitch ), int( kMaxCameraPitch ) ) );
	in.actions = ActionBits( actions );
	in.view = view >= 0 && view < int64_t( kViewModes ) ? uint8_t( view ) : uint8_t( 0 );
	// The next intent, once the last one has had a tick to itself: the simulation carries one out
	// when the count changes, so two in one tick would be one.
	double now = double( Time::get_singleton()->get_ticks_usec() ) / 1e6;
	if ( m_intentSeqSet == false )
	{
		// Not from 0: a player who comes back into its slot must not repeat a count it left there.
		m_intentSeqSet = true;
		m_intentSeq = uint8_t( Time::get_singleton()->get_ticks_usec() >> 8 );
	}
	if ( m_intents.empty() == false && now - m_intentAt >= 2.0 / std::max( get_tick_rate(), 10.0 ) )
	{
		m_intent[0] = m_intents.front()[0];
		m_intent[1] = m_intents.front()[1];
		m_intent[2] = m_intents.front()[2];
		m_intents.erase( m_intents.begin() );
		m_intentSeq = uint8_t( m_intentSeq + 1 );
		m_intentAt = now;
	}
	in.intent = m_intent[0];
	in.intentA = m_intent[1];
	in.intentB = m_intent[2];
	in.intentSeq = m_intentSeq;
	m_source->SetInput( in );

	// Presses are announced here, before the server has seen them, so feedback does not wait.
	ActionBits pressed = in.actions & ~m_lastActions;
	m_lastActions = in.actions;
	AnnouncePresses( pressed );
	// The use button is the engine's, and a look predicts it like an action: by the name "use".
	if ( use && m_lastUse == false )
	{
		emit_signal( "action_pressed", String( "use" ) );
		if ( m_mirror )
		{
			Director()->press( String( "use" ) );
		}
	}
	else if ( use && m_mirror )
	{
		Director()->hold( String( "use" ) );
	}
	m_lastUse = use;
	// What stays down keeps predicting where a look says so (CbPrediction.while_held: automatic fire).
	if ( m_mirror )
	{
		for ( const ModAction& a : m_frame.schema.actions )
		{
			if ( ( in.actions & ~pressed ) & ( ActionBits( 1 ) << a.bit ) )
			{
				Director()->hold( String( a.name.c_str() ) );
			}
		}
	}
}

void CinderboxClient::AnnouncePresses( ActionBits pressed )
{
	for ( const ModAction& a : m_frame.schema.actions )
	{
		if ( pressed & ( ActionBits( 1 ) << a.bit ) )
		{
			emit_signal( "action_pressed", String( a.name.c_str() ) );
			// The looks' predictions (CbPrediction) say what the server will answer, and show it now.
			if ( m_mirror )
			{
				Director()->press( String( a.name.c_str() ) );
			}
		}
	}
}

Ref<PackedScene> CinderboxClient::LoadPrefab( const char* name )
{
	auto it = m_prefabs.find( name );
	if ( it != m_prefabs.end() )
	{
		return it->second;
	}
	String path = m_prefabDir.path_join( String( name ) + ".tscn" );
	Ref<PackedScene> scene = ResourceLoader::get_singleton()->load( path, "PackedScene" );
	if ( scene.is_null() )
	{
		UtilityFunctions::push_warning( "Cinderbox: missing prefab ", path );
	}
	m_prefabs[name] = scene;
	return scene;
}

Ref<PackedScene> CinderboxClient::Prefab( const present::Visual& v )
{
	// An entity made from a map template draws as whatever the template names, so a map author
	// picks the look without touching the client.
	if ( v.templateIndex < m_frame.templateVisuals.size() )
	{
		const std::string& visual = m_frame.templateVisuals[v.templateIndex];
		if ( visual.empty() == false )
		{
			Ref<PackedScene> scene = LoadPrefab( visual.c_str() );
			if ( scene.is_valid() )
			{
				return scene;
			}
			// Fall through to the shape's default prefab, so a missing one is not an invisible entity.
		}
	}

	switch ( v.kind )
	{
		case present::VisualKind::Static:
			return LoadPrefab( "static_box" );
		case present::VisualKind::Player:
			if ( m_characterFolder.is_empty() == false )
			{
				return LoadScene( m_characterFolder + "character.tscn" );
			}
			return LoadPrefab( "player" );
		case present::VisualKind::Ragdoll:
		{
			if ( m_characterFolder.is_empty() == false )
			{
				String own = m_characterFolder + "ragdoll.tscn";
				return LoadScene( ResourceLoader::get_singleton()->exists( own ) ? own : m_characterFolder + "character.tscn" );
			}
			String path = m_prefabDir.path_join( "ragdoll.tscn" );
			return ResourceLoader::get_singleton()->exists( path ) ? LoadPrefab( "ragdoll" ) : LoadPrefab( "player" );
		}
		case present::VisualKind::Item:
		{
			// The look a mod gave this kind of item; nothing drawn without one.
			if ( v.itemKind < m_frame.schema.itemKinds.size() )
			{
				auto it = m_itemLooks.find( m_frame.schema.itemKinds[v.itemKind] );
				if ( it != m_itemLooks.end() )
				{
					return LoadScene( it->second );
				}
			}
			return Ref<PackedScene>();
		}
		case present::VisualKind::Prop:
		default:
			return LoadPrefab( v.shape == ShapeKind::Sphere ? "prop_sphere" : "prop_box" );
	}
}

void CinderboxClient::HandleEvents()
{
	const auto& visuals = m_mirror->World();
	for ( const present::Event& e : m_mirror->Events() )
	{
		Vector3 position = ToGodot( e.position );
		switch ( e.type )
		{
			case present::EventType::Spawned:
			{
				flecs::entity ve( visuals, e.visual );
				if ( ve.is_alive() == false )
				{
					break;
				}
				const present::Visual& v = ve.get<present::Visual>();
				if ( v.kind == present::VisualKind::Static && m_hideStaticBoxes )
				{
					// The map scene already draws this geometry.
					break;
				}
				Node3D* node = CreateNode( e.visual, v );
				emit_signal( "visual_spawned", int64_t( e.visual ), int64_t( e.netId ), String( KindName( e.kind ) ), node, position,
							 e.withEffect, TemplateName( v.templateIndex ) );
				break;
			}
			case present::EventType::Destroying:
			{
				flecs::entity ve( visuals, e.visual );
				if ( ve.is_alive() && ve.get<present::Visual>().kind == present::VisualKind::Item )
				{
					// Out of the hand now, not when its destroy effect has played out: a new item may
					// take the socket this very frame.
					auto found = m_nodes.find( e.visual );
					if ( auto* node = found != m_nodes.end() ? Object::cast_to<Node3D>( ObjectDB::get_instance( found->second ) ) : nullptr )
					{
						node->set_name( "Leaving" );
						node->set_visible( false );
					}
					ItemsChanged( ve.get<present::Visual>().holder );
				}
				uint32_t templateIndex = ve.is_alive() ? ve.get<present::Visual>().templateIndex : kNoTemplate;
				emit_signal( "visual_destroying", int64_t( e.visual ), int64_t( e.netId ), String( KindName( e.kind ) ), position,
							 TemplateName( templateIndex ) );
				break;
			}
			case present::EventType::Removed:
			{
				m_stateHashes.erase( e.visual );
				m_itemHolders.erase( e.visual );
				m_sockets.erase( e.visual );
				auto it = m_nodes.find( e.visual );
				if ( it != m_nodes.end() )
				{
					if ( auto* node = Object::cast_to<Node>( ObjectDB::get_instance( it->second ) ) )
					{
						node->set_name( "Removed" );
						node->queue_free();
					}
					m_nodes.erase( it );
				}
				emit_signal( "visual_removed", int64_t( e.visual ) );
				break;
			}
			case present::EventType::Jumped:
			case present::EventType::Landed:
			{
				bool isLocal = e.netId == m_frame.frame.localNetId;
				emit_signal( e.type == present::EventType::Jumped ? "player_jumped" : "player_landed", int64_t( e.netId ),
							 position - Vector3( 0, present::kFeetOffset, 0 ), isLocal );
				break;
			}
			case present::EventType::Footstep:
			{
				bool isLocal = e.netId == m_frame.frame.localNetId;
				emit_signal( "footstep", int64_t( e.netId ), position - Vector3( 0, present::kFeetOffset, 0 ), isLocal );
				break;
			}
			case present::EventType::Impact:
			{
				flecs::entity ve( visuals, e.visual );
				uint32_t templateIndex = ve.is_alive() ? ve.get<present::Visual>().templateIndex : kNoTemplate;
				emit_signal( "impact", int64_t( e.netId ), position, e.strength, String( KindName( e.kind ) ),
							 TemplateName( templateIndex ) );
				break;
			}
			case present::EventType::Mod:
			{
				if ( e.modType >= m_frame.schema.events.size() )
				{
					break;
				}
				String name( m_frame.schema.events[e.modType].c_str() );
				emit_signal( "mod_event", name, int64_t( e.netId ), int64_t( e.otherNetId ), int64_t( e.value ), position,
							 ToGodot( e.vector ) );
				break;
			}
		}

		// Reactions hear the same events as cues, by name (after a spawned entity's node exists).
		std::string cue;
		uint32_t other = 0;
		Dictionary args;
		args["point"] = position;
		switch ( e.type )
		{
			case present::EventType::Spawned:
				cue = e.withEffect ? "spawned" : "";
				break;
			case present::EventType::Destroying:
				cue = "destroying";
				break;
			case present::EventType::Jumped:
			case present::EventType::Landed:
			case present::EventType::Footstep:
				cue = e.type == present::EventType::Jumped ? "jumped" : e.type == present::EventType::Landed ? "landed" : "footstep";
				args["point"] = position - Vector3( 0, present::kFeetOffset, 0 );
				break;
			case present::EventType::Impact:
				cue = "impact";
				other = e.otherNetId;
				args["strength"] = e.strength;
				break;
			case present::EventType::Mod:
				cue = e.modType < m_frame.schema.events.size() ? m_frame.schema.events[e.modType] : "";
				other = e.otherNetId;
				args["value"] = e.value;
				args["end"] = ToGodot( e.vector );
				break;
			default:
				break;
		}
		if ( cue.empty() == false )
		{
			Cue( cue, e.netId, other, args );
		}
	}
}

// The viewer's own body in first person (see set_first_person): from the spine up it is a steady
// pose of the same animations, put as one piece under the camera's eye, which stands still above
// the feet, and turned so that it follows the camera; the arms are moved by the held item's view offset; and its skeleton draws the
// arms and the legs only.
void CinderboxClient::FirstPersonBody( uint32_t netId, Node* node, const AnimState* state, present::Models& models )
{
	CinderboxSkeleton* skeleton = FindSkeleton( node );
	bool on = m_firstPerson && state != nullptr && skeleton != nullptr && m_animSet != nullptr;
	if ( auto* was = Object::cast_to<CinderboxSkeleton>( ObjectDB::get_instance( m_firstPersonSkeleton ) ) )
	{
		if ( was != skeleton || on == false )
		{
			was->set_first_person_body( false );
		}
	}
	m_firstPersonSkeleton = on ? ObjectID( skeleton->get_instance_id() ) : ObjectID();
	if ( on == false )
	{
		return;
	}
	skeleton->set_first_person_body( true );
	int head = anim::FindJoint( *m_animSet, "Head" );
	int spine = anim::FindJoint( *m_animSet, "Spine" );
	int chest = anim::FindJoint( *m_animSet, "UpperChest" );
	int arms[2] = { anim::FindJoint( *m_animSet, "LeftShoulder" ), anim::FindJoint( *m_animSet, "RightShoulder" ) };
	if ( head < 0 || spine < 0 || chest < 0 || arms[0] < 0 || arms[1] < 0 || size_t( head ) >= models.size() )
	{
		return;
	}
	const auto& restModels = m_animSet->RestModels();
	auto placeOf = []( const ozz::math::Float4x4& m ) {
		float v[4];
		ozz::math::StorePtrU( m.cols[3], v );
		return b3Vec3{ v[0], v[1], v[2] };
	};
	auto between = [&]( const present::Models& from ) {
		return b3MulSV( 0.5f, b3Add( placeOf( from[size_t( arms[0] )] ), placeOf( from[size_t( arms[1] )] ) ) );
	};

	// The upper body comes from a pose of its own: the same animations, but standing still and not
	// yet aimed. What the walk and the sprint do to the body (the hips sway, lean and turn, and the
	// spine and the arms with them) would tilt what is held with every step, and the pose's aim turns
	// the carrying arm alone against that swaying chest. Here the upper body is steady and is aimed
	// as one piece, so the arms and the hands on an item are exactly as the animations have them
	// (the stance's own turn of the shoulders, the other hand's place). The hips and the legs below
	// stay the real pose's: they walk.
	int aimedFrom = state->aiming != 0 && m_animSet->AimJoints().empty() == false ? m_animSet->AimJoints().back().first : -1;
	{
		const auto& library = m_mirror->World().get<present::AnimLibrary>();
		if ( m_viewPose == nullptr || m_viewPoseSet != m_animSet.get() || m_viewPoseGraph != library.graph.get() )
		{
			m_viewPose = std::make_unique<anim::PoseEvaluator>( *m_animSet );
			std::string ignored;
			m_viewPose->SetGraph( library.graph, ignored );
			m_viewPose->SetPacks( library.packs, library.packClips );
			m_viewPoseSet = m_animSet.get();
			m_viewPoseGraph = library.graph.get();
		}
		AnimState still = *state;
		still.aiming = 0;
		still.legYaw = 0.0f;
		still.legsBackward = 0;
		still.groundSpeed = 0.0f;
		still.moveForward = 0.0f;
		still.moveRight = 0.0f;
		if ( library.graph != nullptr && library.graph->layers.empty() == false )
		{
			// The base layer where it starts (standing), at one moment of it.
			AnimGraphLayerState base;
			base.started = 1;
			base.state = base.previous = uint8_t( library.graph->layers[0].start );
			base.weight = 1.0f;
			base.stateTime = 1.0f;
			still.graph[0] = base;
		}
		// A grip with a place on the item is solved on it; hands "as animated" are already.
		anim::HandGrip grip;
		bool placed = m_mirror->GripOf( netId, grip ) && grip.asAnimated == false;
		m_viewPose->Evaluate( still, placed ? &grip : nullptr );
		const present::Models& steady = m_viewPose->Models();
		auto parents = m_animSet->Skeleton().joint_parents();
		std::vector<bool> above( models.size(), false );
		for ( size_t joint = size_t( spine ); joint < models.size() && joint < steady.size(); ++joint )
		{
			int parent = parents[joint];
			above[joint] = int( joint ) == spine || ( parent >= 0 && above[size_t( parent )] );
			if ( above[joint] )
			{
				models[joint] = steady[joint];
			}
		}
	}

	b3Vec3 restHead = placeOf( restModels[size_t( head )] );
	// The camera's eye stands over the feet at the rest pose's head height (anim::EyeHeight).
	b3Vec3 eye = { 0.0f, restHead.y, 0.0f };
	// One point of it is held under the head: aiming, the joint the arm is aimed from (its shoulder),
	// so the hand and what it holds stand still on the screen; otherwise the point between the
	// shoulders.
	auto pinned = [&]() { return aimedFrom >= 0 ? placeOf( models[size_t( aimedFrom )] ) : between( models ); };
	b3Vec3 pinnedAtRest = aimedFrom >= 0 ? placeOf( restModels[size_t( aimedFrom )] ) : between( restModels );

	// The held items' offsets, along where the player looks (in the body's frame: it faces +Z and
	// its right is -X).
	Vector3 offset;
	auto held = m_heldKinds.find( netId );
	if ( held != m_heldKinds.end() )
	{
		for ( uint16_t kind : held->second )
		{
			if ( kind < m_frame.schema.itemKinds.size() )
			{
				auto it = m_itemViewOffsets.find( m_frame.schema.itemKinds[kind] );
				offset += it != m_itemViewOffsets.end() ? it->second : Vector3();
			}
		}
	}
	b3CosSin yaw = detmath::CosSin( state->aimYaw );
	b3CosSin pitch = detmath::CosSin( state->aimPitch );
	b3Vec3 forward = { yaw.sine * pitch.cosine, pitch.sine, yaw.cosine * pitch.cosine };
	b3Vec3 right = { -yaw.cosine, 0.0f, yaw.sine };
	b3Vec3 up = { -yaw.sine * pitch.sine, pitch.cosine, -yaw.cosine * pitch.sine };

	if ( aimedFrom >= 0 && m_animSet->AimTip() >= 0 )
	{
		// Aiming: the whole piece is turned so that the aimed arm is on the line of sight again.
		b3Vec3 line = b3Sub( placeOf( models[size_t( m_animSet->AimTip() )] ), pinned() );
		if ( b3LengthSquared( line ) > 1e-8f )
		{
			anim::RotateSubtreeAbout( *m_animSet, models, spine, pinned(), anim::Arc( b3Normalize( line ), forward ) );
		}
	}
	else
	{
		// Not aiming: it turns with the camera the rest of the way, like something the camera holds.
		// The pose has turned the shoulders by only a part of the pitch (the look chain's shares up
		// to the chest, while the character faces the camera).
		auto parents = m_animSet->Skeleton().joint_parents();
		float turned = 0.0f;
		for ( const auto& [joint, share] : m_animSet->LookJoints() )
		{
			for ( int j = arms[0]; j >= 0; j = parents[size_t( j )] )
			{
				turned += j == joint ? share : 0.0f;
			}
		}
		float pitchLeft = state->aimPitch * ( 1.0f - turned * float( state->look ) / 255.0f );
		anim::RotateSubtreeAbout( *m_animSet, models, spine, pinned(), b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, state->aimYaw ) );
		anim::RotateSubtreeAbout( *m_animSet, models, spine, pinned(), b3MakeQuatFromAxisAngle( right, pitchLeft ) );
	}
	// Its held point is where the rest pose has it under the head, carried round with the camera:
	// the arms come out of the same place on the screen wherever the player looks.
	b3Vec3 pivot = { 0.0f, anim::EyeHeight( *m_animSet ), 0.0f };
	b3Vec3 fromPivot = b3Sub( b3Add( eye, b3Sub( pinnedAtRest, restHead ) ), pivot );
	fromPivot = b3RotateVector( b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, state->aimYaw ), fromPivot );
	fromPivot = b3RotateVector( b3MakeQuatFromAxisAngle( right, state->aimPitch ), fromPivot );
	anim::TranslateSubtree( *m_animSet, models, spine, b3Sub( b3Add( pivot, fromPivot ), pinned() ) );
	// The held item's place in the view: the arms alone move there.
	b3Vec3 placed = b3Add( b3MulSV( offset.x, right ), b3Add( b3MulSV( offset.y, up ), b3MulSV( offset.z, forward ) ) );
	for ( int arm : arms )
	{
		anim::TranslateSubtree( *m_animSet, models, arm, placed );
	}
}

void CinderboxClient::UpdateNodes()
{
	m_mirror->ForEach( [&]( uint64_t id, const present::Visual& v, const present::RenderPose& pose, const present::PlayerAnim* anim,
							const present::RagdollAnim* ragdoll ) {
		auto it = m_nodes.find( id );
		if ( it == m_nodes.end() )
		{
			return;
		}
		auto* node = Object::cast_to<Node3D>( ObjectDB::get_instance( it->second ) );
		if ( node == nullptr )
		{
			return;
		}
		if ( v.kind == present::VisualKind::Item )
		{
			UpdateItem( id, v, pose, node );
			return;
		}

		float s = std::max( pose.scale, 0.0001f );
		Basis rotation( ToGodot( pose.rotation ) );
		if ( v.kind == present::VisualKind::Player || v.kind == present::VisualKind::Ragdoll )
		{
			// A dead player is its ragdoll now (if the mod left one); its own node waits unseen.
			node->set_visible( v.dead == false );
			if ( v.dead )
			{
				return;
			}
			Vector3 origin = ToGodot( pose.position );
			if ( v.kind == present::VisualKind::Player )
			{
				origin -= Vector3( 0, present::kFeetOffset, 0 );
			}
			node->set_transform( Transform3D( rotation.scaled( Vector3( s, s, s ) ), origin ) );

			// The pose: evaluated for players, hung off the parts for ragdolls.
			present::Models* models = nullptr;
			if ( anim != nullptr && anim->evaluator )
			{
				m_pose = anim->evaluator->Models();
				models = &m_pose;
			}
			else if ( ragdoll != nullptr && ragdoll->models.empty() == false )
			{
				m_pose = ragdoll->models;
				models = &m_pose;
			}

			if ( models != nullptr )
			{
				if ( v.kind == present::VisualKind::Player && v.netId == m_frame.frame.localNetId )
				{
					FirstPersonBody( v.netId, node, anim != nullptr ? &anim->shown : nullptr, *models );
				}
				if ( CinderboxSkeleton* skeleton = FindSkeleton( node ) )
				{
					skeleton->ApplyPose( *m_animSet, *models );
				}
			}
			// What the playing animations do besides moving bones.
			if ( anim != nullptr )
			{
				auto playerIt = m_trackPlayers.find( id );
				auto* tracks = playerIt != m_trackPlayers.end()
									  ? Object::cast_to<CbTrackPlayer>( ObjectDB::get_instance( playerIt->second ) )
									  : nullptr;
				if ( tracks != nullptr )
				{
					const auto& library = m_mirror->World().get<present::AnimLibrary>();
					const AnimState& state = anim->shown; // what the pose was evaluated from
					tracks->begin_frame();
					auto clips = library.graph ? anim::ActiveClips( state, *library.graph, library.packs ) : std::vector<anim::ActiveClip>{};
					for ( const anim::ActiveClip& clip : clips )
					{
						tracks->play_at( clip.channel, TrackClipName( String::utf8( clip.name.c_str() ) ), clip.time, clip.loops, clip.restarted );
					}
					tracks->end_frame();
				}
			}
			PlaceSockets( id, node );
			return;
		}

		Vector3 size;
		switch ( v.shape )
		{
			case ShapeKind::Sphere:
				size = Vector3( 2.0f * v.halfExtents.x, 2.0f * v.halfExtents.x, 2.0f * v.halfExtents.x );
				break;
			case ShapeKind::Capsule:
				size = Vector3( 2.0f * v.halfExtents.x, 2.0f * ( v.halfExtents.y + v.halfExtents.x ), 2.0f * v.halfExtents.x );
				break;
			case ShapeKind::Box:
			default:
				size = ToGodot( v.halfExtents ) * 2.0f;
				break;
		}
		// Local scale is applied before rotation, so boxes keep their shape when they turn.
		node->set_transform( Transform3D( rotation * Basis::from_scale( size * s ), ToGodot( pose.position ) ) );
	} );
}

// --- Mod data: fields and conditions ------------------------------------------------

const Blackboard* CinderboxClient::BoardOf( uint32_t netId ) const
{
	if ( !m_mirror )
	{
		return nullptr;
	}
	flecs::entity ve = m_mirror->VisualOf( netId );
	if ( ve.is_valid() == false )
	{
		return nullptr;
	}
	const present::Visual& v = ve.get<present::Visual>();
	return v.hasBoard ? &v.board : nullptr;
}

std::vector<std::string> CinderboxClient::Conditions( const PackedStringArray& conditions ) const
{
	std::vector<std::string> out;
	out.reserve( size_t( conditions.size() ) );
	for ( int64_t i = 0; i < conditions.size(); ++i )
	{
		out.push_back( ToStd( conditions[i] ) );
	}
	return out;
}

// --- Sockets and held items ------------------------------------------------------------------------

namespace
{

// Where a built-in hand socket sits in the items' hand frame (AnimSet::AttachFrame): the grip a
// little along the fingers, the item pointing where the hand points.
Transform3D HandSocketFrame()
{
	Basis turn = Basis::from_euler( Vector3( Math::deg_to_rad( -90.0 ), Math::deg_to_rad( 180.0 ), 0.0 ) );
	return Transform3D( turn, Vector3( 0, -0.06, 0 ) );
}

void CollectSocketNodes( Node* node, std::vector<CbSocket*>& out )
{
	if ( auto* socket = Object::cast_to<CbSocket>( node ) )
	{
		out.push_back( socket );
	}
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		CollectSocketNodes( node->get_child( i ), out );
	}
}

} // namespace

void CinderboxClient::CollectSockets( uint64_t visual, Node3D* node )
{
	std::vector<SocketPlace>& places = m_sockets[visual];
	places.clear();
	m_socketMoves.clear();
	std::vector<CbSocket*> found;
	CollectSocketNodes( node, found );
	for ( CbSocket* socket : found )
	{
		// What sits in a socket in the editor is a preview.
		for ( int i = socket->get_child_count() - 1; i >= 0; --i )
		{
			Node* preview = socket->get_child( i );
			socket->remove_child( preview );
			preview->queue_free();
		}
		SocketPlace place;
		place.node = socket->get_instance_id();
		place.name = socket->get_name();
		place.bone = socket->get_bone();
		place.local = socket->get_transform();
		if ( auto* attachment = Object::cast_to<BoneAttachment3D>( socket->get_parent() ) )
		{
			if ( place.bone.is_empty() )
			{
				place.bone = attachment->get_bone_name();
			}
		}
		else
		{
			place.local = Transform3D(); // not under a bone: at the bone itself
		}
		places.push_back( place );
		// In the game every socket is a child of the entity itself, placed from the pose every frame:
		// "^^/RightHand/Item" is the same path on every rig.
		if ( socket->get_parent() != node )
		{
			m_socketMoves.emplace_back( String( node->get_path_to( socket ) ), String( socket->get_name() ) );
			socket->get_parent()->remove_child( socket );
			node->add_child( socket );
		}
	}
	// Every character has hands to hold things in, and a head to hang things on.
	for ( const char* hand : { "RightHand", "LeftHand", "Head" } )
	{
		bool have = false;
		for ( const SocketPlace& place : places )
		{
			have |= place.name == hand;
		}
		if ( have )
		{
			continue;
		}
		auto* socket = memnew( CbSocket );
		socket->set_name( hand );
		socket->set_bone( hand );
		node->add_child( socket );
		SocketPlace place;
		place.node = socket->get_instance_id();
		place.name = hand;
		place.bone = hand;
		bool isHand = String( hand ) != "Head";
		place.local = isHand ? HandSocketFrame() : Transform3D();
		place.itemFrame = isHand;
		places.push_back( place );
	}
}

void CinderboxClient::PlaceSockets( uint64_t visual, Node3D* node )
{
	auto it = m_sockets.find( visual );
	CinderboxSkeleton* skeleton = FindSkeleton( node );
	if ( it == m_sockets.end() || skeleton == nullptr )
	{
		return;
	}
	// From the pose itself, so what a socket holds is where the server's pose has the bone.
	for ( const SocketPlace& place : it->second )
	{
		auto* socket = Object::cast_to<Node3D>( ObjectDB::get_instance( place.node ) );
		Transform3D joint;
		if ( socket == nullptr || skeleton->JointTransform( place.bone, joint, place.itemFrame ) == false )
		{
			continue;
		}
		socket->set_global_transform( skeleton->get_global_transform() * joint * place.local );
	}
}

Node3D* CinderboxClient::SocketNode( uint32_t holderNetId, uint8_t socket ) const
{
	if ( socket >= m_frame.schema.sockets.size() )
	{
		return nullptr;
	}
	flecs::entity holder = m_mirror ? m_mirror->VisualOf( holderNetId ) : flecs::entity();
	if ( holder.is_valid() == false )
	{
		return nullptr;
	}
	auto it = m_sockets.find( holder.id() );
	if ( it == m_sockets.end() )
	{
		return nullptr;
	}
	String name = String::utf8( m_frame.schema.sockets[socket].c_str() );
	for ( const SocketPlace& place : it->second )
	{
		if ( place.name == name )
		{
			return Object::cast_to<Node3D>( ObjectDB::get_instance( place.node ) );
		}
	}
	return nullptr; // this character has no such socket
}

void CinderboxClient::PlaceItem( uint32_t holderNetId, Node3D* socket, Node3D* item )
{
	// One "Item" per socket: whatever still carries the name is on its way out.
	if ( Node* old = socket->get_node_or_null( "Item" ); old != nullptr && old != item )
	{
		old->set_name( "Leaving" );
	}
	if ( item->get_parent() == nullptr )
	{
		socket->add_child( item );
	}
	else if ( item->get_parent() != socket )
	{
		item->reparent( socket, false );
	}
	item->set_name( "Item" );
	ItemsChanged( holderNetId );
}

void CinderboxClient::ItemsChanged( uint32_t holderNetId )
{
	// The holder's animations reach its items by path; Godot's players cache what a path found.
	flecs::entity holder = m_mirror ? m_mirror->VisualOf( holderNetId ) : flecs::entity();
	if ( holder.is_valid() == false )
	{
		return;
	}
	auto it = m_trackPlayers.find( holder.id() );
	if ( auto* tracks = it != m_trackPlayers.end() ? Object::cast_to<CbTrackPlayer>( ObjectDB::get_instance( it->second ) ) : nullptr )
	{
		tracks->clear_caches();
	}
}

namespace
{

// Where an item's scene goes in the frame it is carried in (a hand's socket, or its body's frame
// in the world): moved so that its carrying grip (CbItem::carry_grip) is at the origin. Looked up once per node.
Transform3D SceneInCarriedFrame( Node3D* node )
{
	static const StringName kKey( "cb_carried" );
	if ( node->has_meta( kKey ) == false )
	{
		node->set_meta( kKey, CbItem::CarryFrameUnder( node ).affine_inverse() );
	}
	return node->get_meta( kKey );
}

// Where an item's scene goes in a socket: in a hand, by its carrying grip; in any other socket (a
// holster on the back, on the hip), as the scene is, since a holster does not hold it by the grip.
Transform3D SceneInSocket( Node3D* node, uint8_t socket )
{
	return socket <= kSocketLeftHand ? SceneInCarriedFrame( node ) : Transform3D();
}

} // namespace

void CinderboxClient::UpdateItem( uint64_t visual, const present::Visual& v, const present::RenderPose& pose, Node3D* node )
{
	uint32_t& drawnWith = m_itemHolders[visual];
	if ( v.holder == 0 )
	{
		// Lying in the world, where its body is (the frame gives its grip).
		CbDirector* world = Director();
		if ( node->get_parent() != world )
		{
			node->set_name( "Leaving" ); // the socket's "Item" is free for the next one
			node->reparent( world, false );
			node->set_name( "item_" + String::num_int64( int64_t( v.netId ) ) );
		}
		if ( drawnWith != 0 )
		{
			ItemsChanged( drawnWith );
			drawnWith = 0;
		}
		node->set_visible( true );
		node->set_transform( Transform3D( Basis( ToGodot( pose.rotation ) ), ToGodot( pose.position ) ) * SceneInCarriedFrame( node ) );
		return;
	}
	drawnWith = v.holder;
	// In its holder's socket (the holder's node may have been rebuilt since).
	Node3D* socket = SocketNode( v.holder, v.socket );
	if ( socket == nullptr )
	{
		// Stowed out of sight (or this character has no such socket): the hand's "Item" is free for
		// what is taken out next, and the item stays under its holder, so "^^" is still the holder.
		if ( String( node->get_name() ) == String( "Item" ) )
		{
			node->set_name( "Stowed_" + String::num_int64( int64_t( v.netId ) ) );
			ItemsChanged( v.holder );
		}
		node->set_visible( false );
		return;
	}
	if ( node->get_parent() != socket )
	{
		PlaceItem( v.holder, socket, node );
		node->set_transform( SceneInSocket( node, v.socket ) );
	}
	node->set_visible( true );
}

// --- World: the director, cues and state ------------------------------------------------------------

namespace
{

uint64_t Mix( uint64_t hash, uint64_t value )
{
	return ( hash ^ value ) * 1099511628211ull;
}

Variant FieldVariant( const BoardField& field, int32_t raw )
{
	switch ( field.type )
	{
		case BoardType::Bool:
			return raw != 0;
		case BoardType::Float:
			return double( BoardToFloat( raw ) );
		case BoardType::Int:
		default:
			return int64_t( raw );
	}
}

} // namespace

CbDirector* CinderboxClient::Director()
{
	if ( auto* director = Object::cast_to<CbDirector>( ObjectDB::get_instance( m_director ) ) )
	{
		return director;
	}
	// At the origin, never moved: placed effects keep their world positions under it.
	auto* director = memnew( CbDirector );
	director->set_name( "World" );
	director->set_auto_update( false ); // updated after the entities have moved, see _process
	add_child( director );
	m_director = ObjectID( director->get_instance_id() );
	return director;
}

CbDirector* CinderboxClient::get_director()
{
	return Director();
}

String CinderboxClient::EntityName( const present::Visual& v ) const
{
	// Players by slot: stable while they are connected, so paths can name them.
	if ( v.kind == present::VisualKind::Player )
	{
		return "player_" + String::num_int64( int64_t( v.slot ) );
	}
	if ( v.kind == present::VisualKind::Item && v.holder != 0 )
	{
		return "Item"; // in its holder's socket
	}
	return String( KindName( v.kind ) ) + "_" + String::num_int64( int64_t( v.netId ) );
}

void CinderboxClient::add_world_scene( Node* scene )
{
	if ( scene == nullptr )
	{
		return;
	}
	Director()->add_child( scene );
	m_worldScenes.push_back( ObjectID( scene->get_instance_id() ) );
	TypedArray<Node> linkLooks = scene->find_children( "*", "CbLinkLook", true, false );
	if ( auto* self = Object::cast_to<CbLinkLook>( scene ) )
	{
		linkLooks.push_back( self );
	}
	for ( int64_t i = 0; i < linkLooks.size(); ++i )
	{
		auto* look = Object::cast_to<CbLinkLook>( Object::cast_to<Node>( linkLooks[i] ) );
		if ( look != nullptr && look->get_scene().is_empty() == false )
		{
			m_linkLooks[ToStd( look->get_motion().strip_edges() )] = { look->get_scene(), look->get_from().strip_edges() };
		}
	}
}

void CinderboxClient::add_item( const String& kind, const String& config )
{
	std::string key = ToStd( kind.strip_edges() );
	if ( key.empty() )
	{
		return;
	}
	// The lines the game reads; the rest is the server's (the body, the grip, the properties).
	PackedStringArray lines = config.split( "\n" );
	for ( const String& raw : lines )
	{
		String line = raw.strip_edges();
		if ( line.begins_with( "scene " ) )
		{
			m_itemLooks[key] = line.substr( 6 ).strip_edges();
		}
		else if ( line.begins_with( "name " ) )
		{
			m_itemNames[key] = line.substr( 5 ).strip_edges();
		}
		else if ( line.begins_with( "view " ) )
		{
			PackedFloat64Array v = line.substr( 5 ).split_floats( " ", false );
			if ( v.size() == 3 )
			{
				m_itemViewOffsets[key] = Vector3( float( v[0] ), float( v[1] ), float( v[2] ) );
			}
		}
	}
}

void CinderboxClient::clear_world_scenes()
{
	m_itemLooks.clear();
	m_itemNames.clear();
	m_itemViewOffsets.clear();
	m_linkLooks.clear();
	for ( ObjectID id : m_worldScenes )
	{
		if ( auto* scene = Object::cast_to<Node>( ObjectDB::get_instance( id ) ) )
		{
			scene->get_parent()->remove_child( scene );
			scene->queue_free();
		}
	}
	m_worldScenes.clear();
}

void CinderboxClient::Cue( const std::string& name, uint32_t a, uint32_t b, const Dictionary& args )
{
	Node* at = a != 0 ? get_entity_node( int64_t( a ) ) : nullptr;
	Node* other = b != 0 ? get_entity_node( int64_t( b ) ) : nullptr;
	Director()->cue( String::utf8( name.c_str() ), at, other, args );
}

void CinderboxClient::PushStates()
{
	CbDirector* director = Director();
	const ModSchema& schema = m_frame.schema;
	// Each entity's board as its state, when it changed: {"melee.hot": true, "pistol.ammo": 7}.
	m_mirror->ForEach( [&]( uint64_t id, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		auto it = m_nodes.find( id );
		auto* node = it != m_nodes.end() ? Object::cast_to<Node>( ObjectDB::get_instance( it->second ) ) : nullptr;
		// (An item has a state whether a mod wrote on its board or not: whether it is in use.)
		if ( node == nullptr || ( v.hasBoard == false && v.kind != present::VisualKind::Item ) )
		{
			return;
		}
		// (Its private fields too, for the viewer's own player: reactions and predictions read them.)
		const Blackboard* privates = PrivatesOf( v.netId );
		uint64_t hash = 1469598103934665603ull;
		for ( const BoardField& field : schema.fields )
		{
			if ( field.scope == BoardScope::Entity )
			{
				hash = Mix( hash, uint32_t( v.board.values[field.slot] ) );
			}
			else if ( field.scope == BoardScope::Private && privates != nullptr )
			{
				hash = Mix( hash, 0x20000u + uint32_t( privates->values[field.slot] ) );
			}
		}
		hash = Mix( hash, 0x30000u + ( v.linked ? 1u : 0u ) + ( v.linkHolds ? 2u : 0u ) );
		hash = Mix( hash, 0x40000u + ( v.kind == present::VisualKind::Item && v.holder != 0 && v.stowed == false ? 1u : 0u ) );
		auto held = m_heldKinds.find( v.netId );
		if ( held != m_heldKinds.end() )
		{
			for ( uint16_t kind : held->second )
			{
				hash = Mix( hash, 0x10000u + kind );
			}
		}
		auto found = m_stateHashes.find( id );
		if ( found != m_stateHashes.end() && found->second == hash )
		{
			return;
		}
		m_stateHashes[id] = hash;
		Dictionary state;
		for ( const BoardField& field : schema.fields )
		{
			if ( field.scope == BoardScope::Entity )
			{
				state[String::utf8( field.name.c_str() )] = FieldVariant( field, v.board.values[field.slot] );
			}
			else if ( field.scope == BoardScope::Private && privates != nullptr )
			{
				state[String::utf8( field.name.c_str() )] = FieldVariant( field, privates->values[field.slot] );
			}
		}
		// What it holds, by item kind, as in the state machines' conditions.
		if ( v.kind == present::VisualKind::Player )
		{
			// "linked": a link of its is out; "link_holds": and it has taken hold.
			state["linked"] = v.linked;
			state["link_holds"] = v.linked && v.linkHolds;
			for ( size_t kind = 0; kind < schema.itemKinds.size(); ++kind )
			{
				state[String::utf8( schema.itemKinds[kind].c_str() )] = Holds( v.netId, uint16_t( kind ) );
			}
		}
		// An item: whether it is in a hand (not lying, not put away). What is inside its scene asks.
		if ( v.kind == present::VisualKind::Item )
		{
			state["in_use"] = v.holder != 0 && v.stowed == false;
		}
		director->set_state( node, state );
	} );
	// The global board as the world's state; every declared name is known (?name).
	const BoardValues* globals = &m_mirror->GlobalBoard();
	uint64_t hash = Mix( 1469598103934665603ull, schema.fields.size() );
	for ( const BoardField& field : schema.fields )
	{
		hash = Mix( hash, field.scope == BoardScope::Global && globals != nullptr ? uint32_t( ( *globals )[field.slot] ) : field.slot );
	}
	if ( hash != m_worldStateHash )
	{
		m_worldStateHash = hash;
		Dictionary world;
		PackedStringArray known;
		for ( const std::string& kind : schema.itemKinds )
		{
			known.push_back( String::utf8( kind.c_str() ) );
		}
		for ( const BoardField& field : schema.fields )
		{
			known.push_back( String::utf8( field.name.c_str() ) );
			if ( field.scope == BoardScope::Global )
			{
				world[String::utf8( field.name.c_str() )] = FieldVariant( field, globals != nullptr ? ( *globals )[field.slot] : 0 );
			}
		}
		director->set_world_state( world );
		director->set_known( known );
	}
	director->set_local( m_frame.frame.localNetId != 0 ? get_entity_node( int64_t( m_frame.frame.localNetId ) ) : nullptr );
}

void CinderboxClient::RetargetTracks( Node* entity, Node* root )
{
	if ( m_trackLibrary.is_null() || m_socketMoves.empty() ||
		 ObjectID( m_trackLibrary->get_instance_id() ) == m_retargetedLibrary )
	{
		return;
	}
	m_retargetedLibrary = ObjectID( m_trackLibrary->get_instance_id() );
	// Track paths are from `root`; the moves are from the entity.
	String rootFromEntity = String( entity->get_path_to( root ) );
	String entityFromRoot = String( root->get_path_to( entity ) );
	TypedArray<StringName> names = m_trackLibrary->get_animation_list();
	for ( int64_t i = 0; i < names.size(); ++i )
	{
		Ref<Animation> animation = m_trackLibrary->get_animation( names[i] );
		for ( int32_t t = 0; animation.is_valid() && t < animation->get_track_count(); ++t )
		{
			String path = String( animation->track_get_path( t ) );
			String full = rootFromEntity == "." ? path : rootFromEntity + "/" + path;
			for ( const auto& [from, to] : m_socketMoves )
			{
				if ( full == from || full.begins_with( from + String( "/" ) ) || full.begins_with( from + String( ":" ) ) )
				{
					String moved = to + full.substr( from.length() );
					animation->track_set_path( t, NodePath( entityFromRoot == "." ? moved : entityFromRoot + "/" + moved ) );
					break;
				}
			}
		}
	}
}

Node3D* CinderboxClient::CreateNode( uint64_t visual, const present::Visual& v )
{
	Ref<PackedScene> prefab = Prefab( v );
	Node3D* node = nullptr;
	if ( prefab.is_valid() )
	{
		// (A scene the guard refuses is not drawn: an empty node stands in, like a missing prefab.)
		Node* instance = cue::Instantiate( prefab );
		node = Object::cast_to<Node3D>( instance );
		if ( node == nullptr && instance != nullptr )
		{
			memdelete( instance );
		}
	}
	if ( node == nullptr )
	{
		node = memnew( Node3D );
	}
	node->set_name( EntityName( v ) );
	// A character's AnimationTree is where its state machine was authored; the simulation runs the
	// baked one, so the tree stays off in the game (switched off before it enters the scene, so it
	// never sets itself up).
	if ( CbCharacter* character = FindInPrefab<CbCharacter>( node ); character != nullptr && character->get_animation_tree_path().is_empty() == false )
	{
		if ( auto* tree = Object::cast_to<AnimationTree>( character->get_node_or_null( character->get_animation_tree_path() ) ) )
		{
			tree->set_active( false );
		}
	}
	if ( v.kind == present::VisualKind::Item && v.holder == 0 )
	{
		Director()->add_child( node ); // lying in the world; UpdateItem places it
	}
	else if ( v.kind == present::VisualKind::Item )
	{
		// In its holder's socket if that is there yet; UpdateItem moves it there otherwise.
		Node3D* socket = SocketNode( v.holder, v.socket );
		node->set_visible( socket != nullptr );
		if ( socket != nullptr )
		{
			PlaceItem( v.holder, socket, node );
			// Like every later move: the scene root's own transform is not part of the item (it
			// used to show until the item first changed hands).
			node->set_transform( SceneInSocket( node, v.socket ) );
		}
		else
		{
			Director()->add_child( node );
		}
	}
	else
	{
		Director()->add_child( node );
	}
	m_nodes[visual] = node->get_instance_id();
	m_stateHashes.erase( visual );
	String templateName = v.kind == present::VisualKind::Item && v.itemKind < m_frame.schema.itemKinds.size()
							  ? String::utf8( m_frame.schema.itemKinds[v.itemKind].c_str() )
							  : TemplateName( v.templateIndex );
	Director()->add_entity( node, String( KindName( v.kind ) ), templateName, int64_t( v.netId ) );
	m_itemHolders[visual] = v.kind == present::VisualKind::Item ? v.holder : 0;
	if ( v.kind == present::VisualKind::Player )
	{
		CollectSockets( visual, node );
	}

	m_trackPlayers.erase( visual );
	if ( CbCharacter* character = v.kind == present::VisualKind::Player ? FindInPrefab<CbCharacter>( node ) : nullptr )
	{
		if ( m_trackLibraryBuilt == false )
		{
			m_trackLibraryBuilt = true;
			Ref<AnimationLibrary> library = character->build_track_library();
			if ( library->get_animation_list().is_empty() == false )
			{
				// Its method tracks call methods on the character's nodes: only listed ones.
				String problem = cue::CheckResource( library );
				if ( problem.is_empty() )
				{
					m_trackLibrary = library;
				}
				else
				{
					UtilityFunctions::push_warning( "Cinderbox: the animations of ", m_characterFolder,
													" play their bones only: a track has ", problem );
				}
			}
		}
		// The character's own AnimationPlayer names the root its tracks' paths start from.
		if ( m_trackLibrary.is_valid() )
		{
			auto* source = Object::cast_to<AnimationPlayer>( character->get_node_or_null( character->get_animation_player_path() ) );
			Node* root = source != nullptr ? source->get_node_or_null( source->get_root_node() ) : nullptr;
			if ( root != nullptr )
			{
				auto* tracks = memnew( CbTrackPlayer );
				tracks->set_name( "TrackPlayer" );
				source->get_parent()->add_child( tracks );
				RetargetTracks( node, root );
				tracks->setup( m_trackLibrary, root );
				m_trackPlayers[visual] = tracks->get_instance_id();
			}
		}
	}
	if ( v.kind == present::VisualKind::Player || v.kind == present::VisualKind::Ragdoll )
	{
		if ( CinderboxSkeleton* skeleton = FindSkeleton( node ) )
		{
			if ( skeleton->get_use_slot_color() )
			{
				skeleton->set_body_color( kSlotColors[v.slot % ( sizeof( kSlotColors ) / sizeof( kSlotColors[0] ) )] );
			}
		}
	}
	return node;
}

void CinderboxClient::RebuildCharacterNodes()
{
	if ( !m_mirror )
	{
		return;
	}
	std::vector<std::pair<uint64_t, present::Visual>> rebuild;
	m_mirror->ForEach( [&]( uint64_t id, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		if ( ( v.kind == present::VisualKind::Player || v.kind == present::VisualKind::Ragdoll ) && m_nodes.count( id ) )
		{
			rebuild.emplace_back( id, v );
		}
	} );
	for ( const auto& [id, v] : rebuild )
	{
		if ( auto* old = Object::cast_to<Node>( ObjectDB::get_instance( m_nodes[id] ) ) )
		{
			old->set_name( "Removed" ); // the new one takes its name now
			old->queue_free();
		}
		CreateNode( id, v );
	}
}

std::shared_ptr<const AnimGraph> CinderboxClient::ServerGraph( const anim::AnimSet& set, const String& name )
{
	const std::string& text = m_frame.schema.animGraph;
	if ( text.empty() )
	{
		return nullptr;
	}
	// The server's state machine, the one the simulation runs; the pose must follow the same one.
	std::string error, warnings;
	auto graph = CompileAnimGraph( text, m_frame.schema, error, warnings );
	if ( graph == nullptr )
	{
		UtilityFunctions::push_warning( "Cinderbox character ", name, ": the server's state machine does not load: ",
										String::utf8( error.c_str() ) );
		return nullptr;
	}
	if ( set.GraphText() != text )
	{
		UtilityFunctions::push_warning( "Cinderbox character ", name,
										": this copy's state machine differs from the server's; playing the server's" );
	}
	anim::PoseEvaluator probe( set );
	probe.SetGraph( graph, warnings );
	if ( warnings.empty() == false )
	{
		UtilityFunctions::push_warning( "Cinderbox character ", name, ": ", String::utf8( warnings.c_str() ) );
	}
	return graph;
}

void CinderboxClient::ServerPacks( const anim::AnimSet& set, AnimGraphPacks& packs,
								   std::vector<std::shared_ptr<const anim::PackClips>>& clips )
{
	std::string warnings;
	packs = CompileAnimPacks( m_frame.schema, warnings );
	clips.assign( packs.size(), nullptr );
	for ( size_t i = 0; i < packs.size(); ++i )
	{
		if ( !packs[i] )
		{
			continue;
		}
		// The pack's baked files, from its mod's item (mounted like every other item).
		const AnimPackInfo& info = m_frame.schema.animPacks[i];
		String folder = "res://anim/" + String::utf8( info.name.c_str() ) + "/";
		anim::FileReader read = [folder]( const std::string& file, std::string& bytes ) {
			String path = folder + String::utf8( file.c_str() );
			if ( FileAccess::file_exists( path ) == false )
			{
				return false;
			}
			PackedByteArray data = FileAccess::get_file_as_bytes( path );
			bytes.assign( reinterpret_cast<const char*>( data.ptr() ), size_t( data.size() ) );
			return true;
		};
		std::string error;
		std::shared_ptr<const anim::AnimSet> packSet = anim::AnimSet::Load( read, ToStd( folder ), error, warnings );
		if ( packSet == nullptr )
		{
			warnings += "animation pack " + info.name + ": " + error + "; ";
			continue;
		}
		clips[i] = anim::FitPack( packSet, *packs[i], set, warnings );
	}
	if ( warnings.empty() == false )
	{
		UtilityFunctions::push_warning( "Cinderbox animation packs: ", String::utf8( warnings.c_str() ) );
	}
}

String CinderboxClient::get_character() const
{
	return String::utf8( m_frame.schema.character.c_str() );
}

String CinderboxClient::use_character( const String& name )
{
	if ( name == m_character && m_animSet )
	{
		// Same character; the server's state machine and packs may still be new.
		auto graph = ServerGraph( *m_animSet, name );
		AnimGraphPacks packs;
		std::vector<std::shared_ptr<const anim::PackClips>> packClips;
		ServerPacks( *m_animSet, packs, packClips );
		if ( m_mirror )
		{
			m_mirror->SetAnimSet( m_animSet, graph, packs, packClips );
			m_mirror->SetItemShapes( m_frame.schema.itemShapes );
		}
		return String();
	}
	std::shared_ptr<const anim::AnimSet> set;
	String folder;
	if ( name.is_empty() )
	{
		m_animSet.reset();
		EnsureAnimations(); // the built-in rig, or --animations
		set = m_animSet;
	}
	else
	{
		folder = "res://characters/" + name + "/";
		anim::FileReader read = [folder]( const std::string& file, std::string& bytes ) {
			String path = folder + String::utf8( file.c_str() );
			if ( FileAccess::file_exists( path ) == false )
			{
				return false;
			}
			PackedByteArray data = FileAccess::get_file_as_bytes( path );
			bytes.assign( reinterpret_cast<const char*>( data.ptr() ), size_t( data.size() ) );
			return true;
		};
		std::string error, warnings;
		std::unique_ptr<anim::AnimSet> loaded = anim::AnimSet::Load( read, ToStd( folder ), error, warnings );
		if ( loaded == nullptr )
		{
			return String::utf8( ( "character " + ToStd( name ) + ": " + error ).c_str() );
		}
		if ( warnings.empty() == false )
		{
			UtilityFunctions::push_warning( "Cinderbox character ", name, ": ", String::utf8( warnings.c_str() ) );
		}
		if ( ResourceLoader::get_singleton()->exists( folder + "character.tscn" ) == false )
		{
			return "character " + name + " has no " + folder + "character.tscn";
		}
		set = std::move( loaded );
	}
	UtilityFunctions::print( "Cinderbox character: ", name.is_empty() ? String( "built-in" ) : name, " (",
							 String::utf8( set->Description().c_str() ), ")" );
	m_character = name;
	m_characterFolder = folder;
	m_trackLibrary.unref(); // read from the character's AnimationPlayer when the first one is drawn
	m_trackLibraryBuilt = false;
	m_animSet = set;
	auto graph = ServerGraph( *set, name );
	AnimGraphPacks packs;
	std::vector<std::shared_ptr<const anim::PackClips>> packClips;
	ServerPacks( *set, packs, packClips );
	if ( m_mirror )
	{
		m_mirror->SetAnimSet( set, graph, packs, packClips );
		m_mirror->SetItemShapes( m_frame.schema.itemShapes );
	}
	RebuildCharacterNodes();
	return String();
}

Ref<PackedScene> CinderboxClient::LoadScene( const String& path )
{
	std::string key = "scene:" + ToStd( path );
	auto it = m_prefabs.find( key );
	if ( it != m_prefabs.end() )
	{
		return it->second;
	}
	Ref<PackedScene> scene;
	if ( ResourceLoader::get_singleton()->exists( path ) )
	{
		scene = ResourceLoader::get_singleton()->load( path, "PackedScene" );
	}
	if ( scene.is_null() )
	{
		UtilityFunctions::push_warning( "Cinderbox: missing scene ", path );
	}
	m_prefabs[key] = scene;
	return scene;
}

Array CinderboxClient::get_actions() const
{
	Array out;
	for ( const ModAction& a : m_frame.schema.actions )
	{
		Dictionary d;
		d["name"] = String( a.name.c_str() );
		d["bit"] = int64_t( a.bit );
		d["key"] = String( a.key.c_str() );
		out.push_back( d );
	}
	return out;
}

PackedStringArray CinderboxClient::get_mod_names() const
{
	PackedStringArray out;
	for ( const std::string& name : m_frame.schema.mods )
	{
		out.push_back( String( name.c_str() ) );
	}
	return out;
}

Variant CinderboxClient::get_field( int64_t net_id, const String& name ) const
{
	const BoardValues* globals = m_mirror ? &m_mirror->GlobalBoard() : nullptr;
	present::FieldValue value =
		present::ReadField( m_frame.schema, ToStd( name ), BoardOf( uint32_t( net_id ) ), globals, PrivatesOf( uint32_t( net_id ) ) );
	if ( value.declared == false )
	{
		return Variant();
	}
	switch ( value.type )
	{
		case BoardType::Float:
			return value.AsFloat();
		case BoardType::Bool:
			return value.AsBool();
		case BoardType::Int:
		default:
			return int64_t( value.raw );
	}
}

Variant CinderboxClient::get_local_field( const String& name ) const
{
	return get_field( get_local_net_id(), name );
}

void CinderboxClient::RefreshHeldKinds()
{
	m_heldKinds.clear();
	if ( !m_mirror )
	{
		return;
	}
	m_mirror->ForEach( [&]( uint64_t, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		// In use only: a holstered pistol does not show its HUD.
		if ( v.kind == present::VisualKind::Item && v.holder != 0 && v.stowed == false )
		{
			m_heldKinds[v.holder].push_back( v.itemKind );
		}
	} );
}

bool CinderboxClient::Holds( uint32_t netId, uint16_t kind ) const
{
	auto it = m_heldKinds.find( netId );
	return it != m_heldKinds.end() && std::find( it->second.begin(), it->second.end(), kind ) != it->second.end();
}

bool CinderboxClient::check_conditions( int64_t net_id, const PackedStringArray& conditions ) const
{
	return CheckWith( net_id, conditions, Dictionary() );
}

// A name of the asker's own, or one of the viewer's own ("ui."): true with its value.
bool CinderboxClient::OwnName( const std::string& name, const Dictionary& extra, float& value ) const
{
	String key = String::utf8( name.c_str() );
	if ( extra.has( key ) )
	{
		value = float( double( extra[key] ) );
		return true;
	}
	if ( name.rfind( "ui.", 0 ) == 0 )
	{
		auto found = m_localValues.find( name );
		value = found != m_localValues.end() ? float( found->second ) : 0.0f;
		return true;
	}
	return false;
}

bool CinderboxClient::CheckWith( int64_t net_id, const PackedStringArray& conditions, const Dictionary& extra ) const
{
	const BoardValues* globals = m_mirror ? &m_mirror->GlobalBoard() : nullptr;
	// Item kinds are names too: "pistol.gun" holds while the player holds one.
	present::ExtraFields held = [&]( const std::string& name, float& value ) {
		if ( OwnName( name, extra, value ) )
		{
			return true;
		}
		// Whose it is: a row of a CbList asks whether it is the viewer's own.
		if ( name == "is_local" )
		{
			value = net_id != 0 && net_id == get_local_net_id() ? 1.0f : 0.0f;
			return true;
		}
		int kind = m_frame.schema.FindItemKind( name );
		if ( kind < 0 )
		{
			return false;
		}
		value = Holds( uint32_t( net_id ), uint16_t( kind ) ) ? 1.0f : 0.0f;
		return true;
	};
	for ( const std::string& condition : Conditions( conditions ) )
	{
		if ( present::CheckCondition( m_frame.schema, condition, BoardOf( uint32_t( net_id ) ), globals, &held, PrivatesOf( uint32_t( net_id ) ) ) ==
			 false )
		{
			return false;
		}
	}
	return true;
}

bool CinderboxClient::check_local_conditions( const PackedStringArray& conditions ) const
{
	return check_conditions( get_local_net_id(), conditions );
}

// The value of an expression over an entity's fields ("combat.health * 100 / combat.max_health").
// Null when it does not parse, or when it reads nothing this server declares (so a HUD node for a
// mod that is not running leaves its target alone).
Variant CinderboxClient::evaluate( int64_t net_id, const String& expression ) const
{
	return EvaluateWith( net_id, expression, Dictionary() );
}

Variant CinderboxClient::EvaluateWith( int64_t net_id, const String& expression, const Dictionary& extra ) const
{
	const BoardValues* globals = m_mirror ? &m_mirror->GlobalBoard() : nullptr;
	bool known = false;
	present::ExtraFields names = [&]( const std::string& name, float& value ) {
		if ( OwnName( name, extra, value ) )
		{
			known = true;
			return true;
		}
		if ( name == "is_local" )
		{
			known = true;
			value = net_id != 0 && net_id == get_local_net_id() ? 1.0f : 0.0f;
			return true;
		}
		int kind = m_frame.schema.FindItemKind( name );
		known |= kind >= 0 || m_frame.schema.FindField( name ) != nullptr;
		if ( kind < 0 )
		{
			return false;
		}
		value = Holds( uint32_t( net_id ), uint16_t( kind ) ) ? 1.0f : 0.0f;
		return true;
	};
	float value = 0.0f;
	if ( present::EvaluateFields( m_frame.schema, ToStd( expression ), BoardOf( uint32_t( net_id ) ), globals, value, &names,
								  PrivatesOf( uint32_t( net_id ) ) ) == false ||
		 known == false )
	{
		return Variant();
	}
	return double( value );
}

Variant CinderboxClient::evaluate_local( const String& expression ) const
{
	return evaluate( get_local_net_id(), expression );
}

String CinderboxClient::format_local_fields( const String& format ) const
{
	return format_fields( get_local_net_id(), format );
}

// "{name:deathmatch.winner}": the field holds a player's NetId; show that player's name.
String CinderboxClient::ResolveNameFields( int64_t net_id, const String& format ) const
{
	String out = format;
	int64_t at = out.find( "{name:" );
	while ( at >= 0 )
	{
		int64_t close = out.find( "}", at );
		if ( close < 0 )
		{
			break;
		}
		String field = out.substr( at + 6, close - at - 6 );
		Variant id = get_field( net_id, field );
		String name = id.get_type() == Variant::NIL || int64_t( id ) == 0 ? String() : get_player_name( int64_t( id ) );
		out = out.substr( 0, at ) + name.replace( "{", "(" ) + out.substr( close + 1 );
		at = out.find( "{name:", at + name.length() );
	}
	return out;
}

int64_t CinderboxClient::get_local_net_id() const
{
	return m_haveFrame ? int64_t( m_frame.frame.localNetId ) : 0;
}

bool CinderboxClient::is_local_player_dead() const
{
	if ( !m_mirror )
	{
		return false;
	}
	flecs::entity ve = m_mirror->VisualOf( uint32_t( get_local_net_id() ) );
	return ve.is_valid() && ve.get<present::Visual>().dead;
}

double CinderboxClient::get_tick_time() const
{
	// Drawn `alpha` of the way from the frame before (stride ticks back) to this one.
	return m_haveFrame ? double( m_frame.frame.tick ) + 1.0 + ( double( m_alpha ) - 1.0 ) * double( m_frame.stride ) : 0.0;
}

double CinderboxClient::get_tick_rate() const
{
	return m_haveFrame && m_frame.frame.tickSeconds > 0.0f ? 1.0 / double( m_frame.frame.tickSeconds ) : 60.0;
}

Vector3 CinderboxClient::get_camera_target() const
{
	if ( !m_mirror )
	{
		return Vector3( 0, 1, 0 );
	}
	if ( is_local_player_dead() )
	{
		// Watch the body fall.
		flecs::entity body = m_mirror->RagdollOf( uint32_t( get_local_net_id() ) );
		if ( body.is_valid() )
		{
			if ( const present::RagdollAnim* ra = body.try_get<present::RagdollAnim>() )
			{
				return ToGodot( ra->current[0].position ) + Vector3( 0, 0.3f, 0 );
			}
		}
	}
	present::RenderPose pose;
	if ( m_mirror->LocalPlayer( pose ) )
	{
		// The point the server takes for a third-person camera's pivot (Context::EyePosition).
		return ToGodot( pose.position ) + Vector3( 0, kViewPivotHeight, 0 );
	}
	return Vector3( 0, 1, 0 );
}

Vector3 CinderboxClient::get_view_position( int64_t view, const Basis& camera ) const
{
	Vector3 pivot = get_camera_target();
	switch ( ViewMode( view >= 0 && view < int64_t( kViewModes ) ? view : 0 ) )
	{
		case ViewMode::FirstPerson:
		{
			present::RenderPose pose;
			if ( m_mirror && m_animSet && is_local_player_dead() == false && m_mirror->LocalPlayer( pose ) )
			{
				// The character's eye height above its feet, as Context::ViewPosition places it: on
				// the mover, so no animation moves the camera.
				Vector3 feet = ToGodot( pose.position ) - Vector3( 0, ragdoll::kFeetBelowCenter, 0 );
				return feet + Vector3( 0, anim::EyeHeight( *m_animSet ), 0 ) - camera.get_column( 2 ) * kEyeAhead;
			}
			return pivot;
		}
		case ViewMode::ShoulderRight:
			return pivot + camera.get_column( 0 ) * kShoulderOffset;
		case ViewMode::ShoulderLeft:
			return pivot - camera.get_column( 0 ) * kShoulderOffset;
		case ViewMode::ThirdPerson:
		default:
			return pivot;
	}
}

double CinderboxClient::get_camera_distance( const Vector3& target, const Vector3& direction, double max_distance, double radius ) const
{
	Vector3 d = direction.normalized();
	if ( m_haveFrame == false || m_frame.hasWorld == false || d.is_zero_approx() )
	{
		return max_distance;
	}
	return present::CameraFreeDistance( m_frame.frame, { target.x, target.y, target.z }, { d.x, d.y, d.z }, float( max_distance ),
										float( radius ) );
}

Node3D* CinderboxClient::get_entity_node( int64_t net_id ) const
{
	if ( !m_mirror )
	{
		return nullptr;
	}
	flecs::entity ve = m_mirror->VisualOf( uint32_t( net_id ) );
	return ve.is_valid() ? get_visual_node( int64_t( ve.id() ) ) : nullptr;
}

Vector3 CinderboxClient::get_bone_position( int64_t net_id, const String& bone ) const
{
	Node3D* node = get_entity_node( net_id );
	if ( node == nullptr )
	{
		return Vector3();
	}
	if ( CinderboxSkeleton* skeleton = FindSkeleton( node ) )
	{
		Transform3D joint;
		if ( bone.is_empty() == false && skeleton->JointTransform( bone, joint ) )
		{
			return ( skeleton->get_global_transform() * joint ).origin;
		}
	}
	return node->get_global_position();
}

int CinderboxClient::SlotOfNetId( uint32_t netId ) const
{
	if ( !m_mirror )
	{
		return -1;
	}
	flecs::entity ve = m_mirror->VisualOf( netId );
	if ( ve.is_valid() == false )
	{
		return -1;
	}
	const present::Visual& v = ve.get<present::Visual>();
	bool person = v.kind == present::VisualKind::Player || v.kind == present::VisualKind::Ragdoll;
	return person ? int( v.slot ) : -1;
}

PackedInt64Array CinderboxClient::get_players() const
{
	PackedInt64Array out;
	if ( !m_mirror )
	{
		return out;
	}
	m_mirror->ForEach( [&]( uint64_t, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		if ( v.kind == present::VisualKind::Player )
		{
			out.push_back( int64_t( v.netId ) );
		}
	} );
	return out;
}

String CinderboxClient::get_player_name( int64_t net_id ) const
{
	int slot = SlotOfNetId( uint32_t( net_id ) );
	if ( slot < 0 )
	{
		return String();
	}
	const std::string& name = m_frame.names[size_t( slot )];
	return name.empty() ? String( "Player " ) + String::num_int64( slot + 1 ) : String::utf8( name.c_str() );
}

PackedInt64Array CinderboxClient::get_items( const String& kind, int64_t holder ) const
{
	PackedInt64Array out;
	int wanted = kind.is_empty() ? -1 : m_frame.schema.FindItemKind( ToStd( kind ) );
	if ( !m_mirror || ( kind.is_empty() == false && wanted < 0 ) )
	{
		return out;
	}
	m_mirror->ForEach( [&]( uint64_t, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		if ( v.kind == present::VisualKind::Item && ( wanted < 0 || int( v.itemKind ) == wanted ) &&
			 ( holder == 0 || int64_t( v.holder ) == holder ) )
		{
			out.push_back( int64_t( v.netId ) );
		}
	} );
	return out;
}

String CinderboxClient::get_entity_name( int64_t net_id ) const
{
	flecs::entity ve = m_mirror ? m_mirror->VisualOf( uint32_t( net_id ) ) : flecs::entity();
	if ( ve.is_valid() )
	{
		const present::Visual& v = ve.get<present::Visual>();
		if ( v.kind == present::VisualKind::Item && v.itemKind < m_frame.schema.itemKinds.size() )
		{
			const std::string& kind = m_frame.schema.itemKinds[v.itemKind];
			auto found = m_itemNames.find( kind );
			return found != m_itemNames.end() && found->second.is_empty() == false ? found->second : String::utf8( kind.c_str() );
		}
	}
	return get_player_name( net_id );
}

String CinderboxClient::ResolveKeysAndLooks( int64_t net_id, const String& format ) const
{
	String out = format;
	// {key:action}: what the player pressed to do it, as bound now (rebinding shows).
	for ( int64_t at = out.find( "{key:" ); at >= 0; at = out.find( "{key:", at + 1 ) )
	{
		int64_t close = out.find( "}", at );
		if ( close < 0 )
		{
			break;
		}
		String action = "cb_" + out.substr( at + 5, close - at - 5 );
		String key = "?";
		InputMap* map = InputMap::get_singleton();
		if ( map->has_action( action ) )
		{
			TypedArray<InputEvent> events = map->action_get_events( action );
			Ref<InputEvent> first = events.size() > 0 ? Ref<InputEvent>( events[0] ) : Ref<InputEvent>();
			if ( Ref<InputEventKey> k = first; k.is_valid() )
			{
				Key code = k->get_physical_keycode() != KEY_NONE ? k->get_physical_keycode() : k->get_keycode();
				key = OS::get_singleton()->get_keycode_string( code );
			}
			else if ( Ref<InputEventMouseButton> m = first; m.is_valid() )
			{
				MouseButton button = m->get_button_index();
				key = button == MOUSE_BUTTON_LEFT ? "LMB" : button == MOUSE_BUTTON_RIGHT ? "RMB" : button == MOUSE_BUTTON_MIDDLE ? "MMB" : "Mouse";
			}
			else if ( first.is_valid() )
			{
				key = first->as_text();
			}
		}
		out = out.substr( 0, at ) + key + out.substr( close + 1 );
	}
	// {look:field}: the field holds an entity's NetId; what is it called?
	for ( int64_t at = out.find( "{look:" ); at >= 0; at = out.find( "{look:", at + 1 ) )
	{
		int64_t close = out.find( "}", at );
		if ( close < 0 )
		{
			break;
		}
		Variant id = get_field( net_id, out.substr( at + 6, close - at - 6 ) );
		String name;
		flecs::entity ve = m_mirror && id.get_type() != Variant::NIL ? m_mirror->VisualOf( uint32_t( int64_t( id ) ) ) : flecs::entity();
		if ( ve.is_valid() )
		{
			const present::Visual& target = ve.get<present::Visual>();
			if ( target.kind == present::VisualKind::Item && target.itemKind < m_frame.schema.itemKinds.size() )
			{
				const std::string& kind = m_frame.schema.itemKinds[target.itemKind];
				auto found = m_itemNames.find( kind );
				name = found != m_itemNames.end() && found->second.is_empty() == false ? found->second : String::utf8( kind.c_str() );
			}
			else if ( target.kind == present::VisualKind::Player )
			{
				name = get_player_name( int64_t( id ) );
			}
		}
		out = out.substr( 0, at ) + name.replace( "{", "(" ) + out.substr( close + 1 );
	}
	return out;
}

String CinderboxClient::format_fields( int64_t net_id, const String& format ) const
{
	return FormatWith( net_id, format, Dictionary() );
}

String CinderboxClient::FormatWith( int64_t net_id, const String& format, const Dictionary& extra ) const
{
	// The asker's own names and the viewer's own values, first: "{slot.number}", "{ui.picked}".
	String own = format;
	Array keys = extra.keys();
	for ( int64_t i = 0; i < keys.size(); ++i )
	{
		if ( extra[keys[i]].get_type() == Variant::STRING )
		{
			// Words of the asker's own (a name in a row of events): as they are, and never read again
			// as a field's name.
			own = own.replace( "{" + String( keys[i] ) + "}", String( extra[keys[i]] ).replace( "{", "(" ) );
			continue;
		}
		double v = double( extra[keys[i]] );
		own = own.replace( "{" + String( keys[i] ) + "}", v == std::floor( v ) ? String::num_int64( int64_t( v ) ) : String::num( v, 1 ) );
	}
	for ( const auto& [name, v] : m_localValues )
	{
		own = own.replace( "{" + String::utf8( name.c_str() ) + "}", v == std::floor( v ) ? String::num_int64( int64_t( v ) ) : String::num( v, 1 ) );
	}
	String withName = ResolveNameFields(
		net_id, ResolveKeysAndLooks( net_id, own ).replace( "{name}", get_entity_name( net_id ).replace( "{", "(" ) ) );
	const BoardValues* globals = m_mirror ? &m_mirror->GlobalBoard() : nullptr;
	std::string text =
		present::FormatFields( m_frame.schema, ToStd( withName ), BoardOf( uint32_t( net_id ) ), globals, PrivatesOf( uint32_t( net_id ) ) );
	return String::utf8( text.c_str() );
}

Array CinderboxClient::get_required_items() const
{
	Array out;
	for ( const ModItem& item : m_frame.schema.items )
	{
		Dictionary d;
		d["mod"] = String( item.mod.c_str() );
		d["sha256"] = String( item.sha256.c_str() );
		out.push_back( d );
	}
	return out;
}

String CinderboxClient::get_kind( int64_t net_id ) const
{
	if ( !m_mirror )
	{
		return String();
	}
	flecs::entity ve = m_mirror->VisualOf( uint32_t( net_id ) );
	return ve.is_valid() ? String( KindName( ve.get<present::Visual>().kind ) ) : String();
}

String CinderboxClient::get_entity_template_name( int64_t net_id ) const
{
	if ( !m_mirror )
	{
		return String();
	}
	flecs::entity ve = m_mirror->VisualOf( uint32_t( net_id ) );
	return ve.is_valid() ? TemplateName( ve.get<present::Visual>().templateIndex ) : String();
}

namespace
{
// The lead is given back at this share of real time once nothing is predicted: the upper layers
// play that much slower until they are level with the server again.
constexpr float kLeadReturn = 0.15f;
// Never further ahead than this, whatever the latency.
constexpr float kMaxLead = 0.5f;
} // namespace

void CinderboxClient::LeadLocalPlayer( float delta )
{
	present::AnimLead lead;
	uint32_t local = m_frame.frame.localNetId;
	const auto& library = m_mirror->World().get<present::AnimLibrary>();
	if ( local == 0 || !library.graph )
	{
		m_lead = 0.0f;
		m_mirror->SetAnimLead( lead );
		return;
	}
	const ModSchema& schema = m_frame.schema;
	double now = double( Time::get_singleton()->get_ticks_usec() ) / 1e6;
	float newest = 0.0f;
	for ( const auto& shown : Director()->Pending() )
	{
		// Only what the character's upper layers read: its machine may not care.
		present::AnimLead::Input input;
		input.age = float( now - shown.at );
		int event = schema.FindEvent( ToStd( shown.cue ) );
		if ( event >= 0 && library.graph->UpperLayersRead( AnimExpr::VarKind::Event, event ) )
		{
			input.event = event;
		}
		int stance = shown.stance.is_empty() ? -1 : schema.FindStance( ToStd( shown.stance ) );
		int layer = shown.stanceLayer.is_empty() ? -1 : schema.FindLayer( ToStd( shown.stanceLayer ) );
		if ( stance >= 0 && layer >= 0 && layer < kLayerLimit && library.graph->UpperLayersRead( AnimExpr::VarKind::Stance, stance + 1 ) )
		{
			input.layer = layer;
			input.stance = uint8_t( stance + 1 );
		}
		if ( input.event >= 0 || input.layer >= 0 )
		{
			newest = std::max( newest, input.age );
			lead.inputs.push_back( input );
		}
	}
	// Ahead by the time since the oldest unanswered press; afterwards the lead is given back slowly.
	m_lead = lead.inputs.empty() ? std::max( 0.0f, m_lead - kLeadReturn * delta ) : std::max( m_lead, newest );
	m_lead = std::min( m_lead, kMaxLead );
	if ( m_lead <= 0.0f )
	{
		m_mirror->SetAnimLead( lead );
		return;
	}
	lead.netId = local;
	lead.seconds = m_lead;
	lead.tickSeconds = m_frame.frame.tickSeconds;
	if ( const Blackboard* board = BoardOf( local ) )
	{
		lead.board = *board;
	}
	lead.globalBoard = m_mirror->GlobalBoard();
	auto held = m_heldKinds.find( local );
	if ( held != m_heldKinds.end() )
	{
		lead.heldKinds = held->second;
	}
	m_mirror->SetAnimLead( lead );
}

void CinderboxClient::ApplyPredictedFields()
{
	m_privates = m_frame.privates; // this frame's, before what is predicted
	uint32_t local = m_frame.frame.localNetId;
	flecs::entity visual = local != 0 ? m_mirror->VisualOf( local ) : flecs::entity();
	if ( visual.is_valid() == false )
	{
		return;
	}
	const ModSchema& schema = m_frame.schema;
	for ( const auto& shown : Director()->Pending() )
	{
		for ( int64_t i = 0; i < shown.changes.size(); ++i )
		{
			cue::Change change;
			const BoardField* field = cue::ParseChange( shown.changes[i], change ) ? schema.FindField( ToStd( change.field ) ) : nullptr;
			if ( field == nullptr || field->scope == BoardScope::Global )
			{
				continue; // a field no mod on this server declared, or not the player's own
			}
			// Copies that are written again from the frame on every update, so this never adds up:
			// the mirror's board, or the frame's private fields.
			present::Visual& v = visual.get_mut<present::Visual>();
			int32_t& slot = field->scope == BoardScope::Private ? m_privates.values[field->slot] : v.board.values[field->slot];
			if ( field->type == BoardType::Float )
			{
				float value = BoardToFloat( slot );
				value = change.op == '-' ? value - float( change.value ) : change.op == '+' ? value + float( change.value ) : float( change.value );
				slot = BoardFromFloat( value );
			}
			else if ( field->type == BoardType::Bool )
			{
				slot = change.value != 0.0 ? 1 : 0;
			}
			else
			{
				int32_t amount = int32_t( change.value );
				slot = change.op == '-' ? slot - amount : change.op == '+' ? slot + amount : amount;
			}
		}
	}
}

void CinderboxClient::_process( double delta )
{
	if ( Engine::get_singleton()->is_editor_hint() || !m_mirror || !m_source )
	{
		return;
	}

	if ( m_source->Take( m_frame ) )
	{
		m_haveFrame = true;
		m_frame.frame.resetGeneration += m_sourceCount << 32;
	}
	if ( m_haveFrame == false )
	{
		return;
	}

	String state = get_source_state();
	if ( state != m_lastState )
	{
		m_lastState = state;
		emit_signal( "source_state_changed", state );
	}
	if ( m_frame.schemaGeneration != m_schemaGeneration )
	{
		m_schemaGeneration = m_frame.schemaGeneration;
		emit_signal( "schema_changed" );
	}
	if ( m_frame.namesGeneration != m_namesGeneration )
	{
		m_namesGeneration = m_frame.namesGeneration;
		emit_signal( "names_changed" );
	}
	if ( m_frame.hasWorld == false )
	{
		return;
	}

	// Interpolate from the moment the frame was published, at the pace the source moves on.
	float tickSeconds = m_frame.frame.tickSeconds;
	double since = present::ViewClock() - m_frame.publishedAt;
	// (A source that sends a frame every few ticks: the way from the frame before takes that long.)
	double frameSeconds = double( tickSeconds ) * double( std::max<uint32_t>( m_frame.stride, 1 ) );
	float alpha = std::clamp( float( m_frame.alphaAtPublish + since * double( m_frame.rate ) / frameSeconds ), 0.0f, 1.0f );

	m_alpha = alpha;
	LeadLocalPlayer( float( delta ) );
	m_mirror->Update( m_frame.frame, alpha, float( delta ) );
	// A rollback is reported once; later updates of the same frame are ordinary.
	m_frame.frame.rolledBack = false;

	UpdateMapVisual();
	RefreshHeldKinds();
	HandleEvents();
	// A source that plays someone else's input says what the local player pressed.
	AnnouncePresses( m_frame.localPressed );
	m_frame.localPressed = 0;
	ApplyPredictedFields();
	UpdateNodes();
	UpdateLinks();
	PushStates();
	Director()->update();
}

// Links: for each player that has one out, the scene its CbLinkLook names, stretched from the
// player (a socket, or its chest) to the link's end. One metre of scene along -Z per metre of rope.
void CinderboxClient::UpdateLinks()
{
	if ( m_motionNamesGeneration != m_frame.schemaGeneration )
	{
		// The motions' names, in the schema's order: a link says which motion threw it by index.
		m_motionNamesGeneration = m_frame.schemaGeneration;
		m_motionNames.clear();
		std::string ignored;
		if ( std::shared_ptr<const Motions> motions = CompileMotions( m_frame.schema, ignored ) )
		{
			for ( const Motion& m : motions->list )
			{
				m_motionNames.push_back( m.name );
			}
		}
	}
	std::map<uint64_t, bool> seen;
	m_mirror->ForEach( [&]( uint64_t id, const present::Visual& v, const present::RenderPose& pose, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		if ( v.kind != present::VisualKind::Player || v.linked == false || v.dead || m_linkLooks.empty() )
		{
			return;
		}
		auto look = m_linkLooks.find( v.linkMotion < m_motionNames.size() ? m_motionNames[v.linkMotion] : std::string() );
		if ( look == m_linkLooks.end() )
		{
			look = m_linkLooks.find( std::string() );
		}
		if ( look == m_linkLooks.end() )
		{
			return;
		}
		auto existing = m_linkNodes.find( id );
		auto* rope = existing != m_linkNodes.end() ? Object::cast_to<Node3D>( ObjectDB::get_instance( existing->second.node ) ) : nullptr;
		if ( rope != nullptr && existing->second.motion != v.linkMotion )
		{
			rope->queue_free();
			rope = nullptr;
		}
		if ( rope == nullptr )
		{
			// (Checked first, like every scene a pack brings.)
			Ref<PackedScene> packed = ResourceLoader::get_singleton()->load( look->second.scene, "PackedScene" );
			rope = Object::cast_to<Node3D>( cue::Instantiate( packed ) );
			if ( rope == nullptr )
			{
				return;
			}
			Director()->add_child( rope );
			m_linkNodes[id] = { ObjectID( rope->get_instance_id() ), v.linkMotion };
		}
		seen[id] = true;

		// From the socket, where the player's scene has it; else from its chest.
		Vector3 from = Vector3( pose.position.x, pose.position.y + kViewPivotHeight, pose.position.z );
		auto found = m_nodes.find( id );
		auto* player = found != m_nodes.end() ? Object::cast_to<Node>( ObjectDB::get_instance( found->second ) ) : nullptr;
		if ( player != nullptr && look->second.from.is_empty() == false )
		{
			if ( auto* socket = Object::cast_to<Node3D>( player->get_node_or_null( NodePath( look->second.from ) ) ) )
			{
				from = socket->get_global_position();
			}
		}
		Vector3 along = Vector3( v.linkEnd.x, v.linkEnd.y, v.linkEnd.z ) - from;
		real_t length = along.length();
		Transform3D where;
		if ( length > 0.01f )
		{
			Vector3 up = Math::abs( along.normalized().y ) < 0.99f ? Vector3( 0, 1, 0 ) : Vector3( 1, 0, 0 );
			where.basis = Basis::looking_at( along, up ).scaled_local( Vector3( 1, 1, length ) );
		}
		where.origin = from;
		rope->set_global_transform( where );
	} );
	for ( auto it = m_linkNodes.begin(); it != m_linkNodes.end(); )
	{
		if ( seen.count( it->first ) == 0 )
		{
			if ( auto* rope = Object::cast_to<Node>( ObjectDB::get_instance( it->second.node ) ) )
			{
				rope->queue_free();
			}
			it = m_linkNodes.erase( it );
		}
		else
		{
			++it;
		}
	}
}

String CinderboxClient::TemplateName( uint32_t index ) const
{
	return index < m_frame.templateNames.size() ? String( m_frame.templateNames[index].c_str() ) : String();
}

String CinderboxClient::get_map_name() const
{
	return String( m_frame.mapName.c_str() );
}

// The map's visuals are an ordinary Godot scene named after the map, which a mod can replace. If
// it is missing, the baked collision boxes are drawn instead, so a client is never left in the void.
void CinderboxClient::UpdateMapVisual()
{
	if ( m_frame.mapHash == m_visualMapHash )
	{
		return;
	}
	m_visualMapHash = m_frame.mapHash;
	m_hideStaticBoxes = false;

	if ( auto* old = Object::cast_to<Node>( ObjectDB::get_instance( m_mapVisual ) ) )
	{
		old->queue_free();
	}
	m_mapVisual = ObjectID();

	// The level itself changed, so every visual belongs to the previous map.
	for ( const auto& entry : m_nodes )
	{
		if ( auto* node = Object::cast_to<Node>( ObjectDB::get_instance( entry.second ) ) )
		{
			node->set_name( "Removed" );
			node->queue_free();
		}
	}
	m_nodes.clear();
	m_stateHashes.clear();

	if ( m_frame.mapName.empty() )
	{
		return;
	}
	String path = m_mapDir.path_join( String( m_frame.mapName.c_str() ) + ".tscn" );
	if ( ResourceLoader::get_singleton()->exists( path ) == false )
	{
		UtilityFunctions::print( "Cinderbox: no visuals for map \"", String( m_frame.mapName.c_str() ),
								 "\", drawing collision boxes" );
		return;
	}
	Ref<PackedScene> scene = ResourceLoader::get_singleton()->load( path );
	Node* instance = cue::Instantiate( scene );
	Node3D* node = Object::cast_to<Node3D>( instance );
	if ( node == nullptr )
	{
		if ( instance != nullptr )
		{
			memdelete( instance );
		}
		UtilityFunctions::push_warning( "Cinderbox: cannot use ", path, ", drawing collision boxes" );
		return;
	}
	node->set_name( "Map" );
	Director()->add_child( node );
	m_mapVisual = node->get_instance_id();
	m_hideStaticBoxes = true;
}

Dictionary CinderboxClient::get_stats() const
{
	Dictionary d;
	for ( const present::ViewStat& stat : m_frame.stats )
	{
		Variant value;
		if ( const int64_t* i = std::get_if<int64_t>( &stat.value ) )
		{
			value = *i;
		}
		else if ( const double* f = std::get_if<double>( &stat.value ) )
		{
			value = *f;
		}
		else if ( const bool* b = std::get_if<bool>( &stat.value ) )
		{
			value = *b;
		}
		else if ( const std::string* s = std::get_if<std::string>( &stat.value ) )
		{
			value = String::utf8( s->c_str() );
		}
		d[String::utf8( stat.name.c_str() )] = value;
	}
	d["state"] = get_source_state();
	d["entities"] = int64_t( m_frame.frame.entities.size() );
	d["animation"] = m_animSet ? String( m_animSet->Description().c_str() ) : String();
	return d;
}

String CinderboxClient::get_source_state() const
{
	if ( !m_source )
	{
		return "stopped";
	}
	if ( m_haveFrame == false )
	{
		return "starting";
	}
	return String( m_frame.state.c_str() );
}

bool CinderboxClient::has_local_player() const
{
	present::RenderPose pose;
	return m_mirror && m_mirror->LocalPlayer( pose );
}

Vector3 CinderboxClient::get_local_player_position() const
{
	present::RenderPose pose;
	if ( m_mirror && m_mirror->LocalPlayer( pose ) )
	{
		return ToGodot( pose.position );
	}
	return Vector3();
}

Node3D* CinderboxClient::get_visual_node( int64_t visual_id ) const
{
	auto it = m_nodes.find( uint64_t( visual_id ) );
	if ( it == m_nodes.end() )
	{
		return nullptr;
	}
	return Object::cast_to<Node3D>( ObjectDB::get_instance( it->second ) );
}

} // namespace cb::gd
