#include "cinderbox_client.h"

#include "cinderbox_animator.h"
#include "cinderbox_skeleton.h"
#include "detmath.h"
#include "pose_tools.h"
#include "types.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
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

CinderboxClient::~CinderboxClient()
{
	m_thread.Stop();
}

void CinderboxClient::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "connect_to_server" ), &CinderboxClient::connect_to_server );
	ClassDB::bind_method( D_METHOD( "disconnect_from_server" ), &CinderboxClient::disconnect_from_server );
	ClassDB::bind_method( D_METHOD( "is_running" ), &CinderboxClient::is_running );
	ClassDB::bind_method( D_METHOD( "set_input", "move", "camera_yaw", "camera_pitch", "jump", "sprint", "actions" ),
						  &CinderboxClient::set_input );
	ClassDB::bind_method( D_METHOD( "get_actions" ), &CinderboxClient::get_actions );
	ClassDB::bind_method( D_METHOD( "get_mod_names" ), &CinderboxClient::get_mod_names );
	ClassDB::bind_method( D_METHOD( "get_field", "net_id", "name" ), &CinderboxClient::get_field );
	ClassDB::bind_method( D_METHOD( "get_local_field", "name" ), &CinderboxClient::get_local_field );
	ClassDB::bind_method( D_METHOD( "check_conditions", "net_id", "conditions" ), &CinderboxClient::check_conditions );
	ClassDB::bind_method( D_METHOD( "check_local_conditions", "conditions" ), &CinderboxClient::check_local_conditions );
	ClassDB::bind_method( D_METHOD( "format_local_fields", "format" ), &CinderboxClient::format_local_fields );
	ClassDB::bind_method( D_METHOD( "get_local_net_id" ), &CinderboxClient::get_local_net_id );
	ClassDB::bind_method( D_METHOD( "is_local_player_dead" ), &CinderboxClient::is_local_player_dead );
	ClassDB::bind_method( D_METHOD( "get_camera_target" ), &CinderboxClient::get_camera_target );
	ClassDB::bind_method( D_METHOD( "get_bone_position", "net_id", "bone" ), &CinderboxClient::get_bone_position );
	ClassDB::bind_method( D_METHOD( "get_kind", "net_id" ), &CinderboxClient::get_kind );
	ClassDB::bind_method( D_METHOD( "get_entity_template_name", "net_id" ), &CinderboxClient::get_entity_template_name );
	ClassDB::bind_method( D_METHOD( "get_entity_node", "net_id" ), &CinderboxClient::get_entity_node );
	ClassDB::bind_method( D_METHOD( "add_state_binding", "binding" ), &CinderboxClient::add_state_binding );
	ClassDB::bind_method( D_METHOD( "clear_state_bindings" ), &CinderboxClient::clear_state_bindings );
	ClassDB::bind_method( D_METHOD( "get_stats" ), &CinderboxClient::get_stats );
	ClassDB::bind_method( D_METHOD( "get_connection_state" ), &CinderboxClient::get_connection_state );
	ClassDB::bind_method( D_METHOD( "has_local_player" ), &CinderboxClient::has_local_player );
	ClassDB::bind_method( D_METHOD( "get_local_player_position" ), &CinderboxClient::get_local_player_position );
	ClassDB::bind_method( D_METHOD( "get_visual_node", "visual_id" ), &CinderboxClient::get_visual_node );

	ClassDB::bind_method( D_METHOD( "set_host", "host" ), &CinderboxClient::set_host );
	ClassDB::bind_method( D_METHOD( "get_host" ), &CinderboxClient::get_host );
	ClassDB::bind_method( D_METHOD( "set_port", "port" ), &CinderboxClient::set_port );
	ClassDB::bind_method( D_METHOD( "get_port" ), &CinderboxClient::get_port );
	ClassDB::bind_method( D_METHOD( "set_rollback_min", "ticks" ), &CinderboxClient::set_rollback_min );
	ClassDB::bind_method( D_METHOD( "get_rollback_min" ), &CinderboxClient::get_rollback_min );
	ClassDB::bind_method( D_METHOD( "set_rollback_max", "ticks" ), &CinderboxClient::set_rollback_max );
	ClassDB::bind_method( D_METHOD( "get_rollback_max" ), &CinderboxClient::get_rollback_max );
	ClassDB::bind_method( D_METHOD( "set_map_dir", "dir" ), &CinderboxClient::set_map_dir );
	ClassDB::bind_method( D_METHOD( "get_map_dir" ), &CinderboxClient::get_map_dir );
	ClassDB::bind_method( D_METHOD( "get_map_name" ), &CinderboxClient::get_map_name );
	ClassDB::bind_method( D_METHOD( "set_prefab_dir", "dir" ), &CinderboxClient::set_prefab_dir );
	ClassDB::bind_method( D_METHOD( "get_prefab_dir" ), &CinderboxClient::get_prefab_dir );
	ClassDB::bind_method( D_METHOD( "set_animation_dir", "dir" ), &CinderboxClient::set_animation_dir );
	ClassDB::bind_method( D_METHOD( "get_animation_dir" ), &CinderboxClient::get_animation_dir );

	ADD_PROPERTY( PropertyInfo( Variant::STRING, "host" ), "set_host", "get_host" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "port", PROPERTY_HINT_RANGE, "1,65535" ), "set_port", "get_port" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "rollback_min", PROPERTY_HINT_RANGE, "1,64" ), "set_rollback_min", "get_rollback_min" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "rollback_max", PROPERTY_HINT_RANGE, "1,64" ), "set_rollback_max", "get_rollback_max" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "prefab_dir", PROPERTY_HINT_DIR ), "set_prefab_dir", "get_prefab_dir" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "map_dir", PROPERTY_HINT_DIR ), "set_map_dir", "get_map_dir" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "animation_dir", PROPERTY_HINT_GLOBAL_DIR ), "set_animation_dir",
				  "get_animation_dir" );

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
	ADD_SIGNAL( MethodInfo( "connection_state_changed", PropertyInfo( Variant::STRING, "state" ) ) );
	// The server's mods changed (a first join, or a different server): actions and fields to rebind.
	ADD_SIGNAL( MethodInfo( "schema_changed" ) );
	// A server mod announced something. a is who it is about, b the other entity (0 if none).
	ADD_SIGNAL( MethodInfo( "mod_event", PropertyInfo( Variant::STRING, "name" ), PropertyInfo( Variant::INT, "net_id_a" ),
							PropertyInfo( Variant::INT, "net_id_b" ), PropertyInfo( Variant::INT, "value" ),
							PropertyInfo( Variant::VECTOR3, "position" ), PropertyInfo( Variant::VECTOR3, "vector" ) ) );
	// The local player just pressed a mod action; the server has not answered yet.
	ADD_SIGNAL( MethodInfo( "action_pressed", PropertyInfo( Variant::STRING, "name" ) ) );
}

void CinderboxClient::EnsureAnimations()
{
	if ( m_animSet )
	{
		return;
	}
	if ( m_animationDir.is_empty() == false )
	{
		// res:// and user:// work when the folder exists on disk (editor runs, user folders).
		String dir = ProjectSettings::get_singleton()->globalize_path( m_animationDir );
		std::string error, warnings;
		auto set = anim::AnimSet::Load( ToStd( dir ), error, warnings );
		if ( set )
		{
			UtilityFunctions::print( "Cinderbox animations: ", String( set->Description().c_str() ) );
			if ( warnings.empty() == false )
			{
				UtilityFunctions::push_warning( String( warnings.c_str() ) );
			}
			m_animSet = std::move( set );
			return;
		}
		UtilityFunctions::push_warning( "Cinderbox animations: ", String( error.c_str() ), ", using the placeholder rig" );
	}
	m_animSet = anim::AnimSet::CreateProcedural();
}

void CinderboxClient::connect_to_server()
{
	if ( Engine::get_singleton()->is_editor_hint() )
	{
		return;
	}
	EnsureAnimations();
	if ( !m_mirror )
	{
		m_mirror = std::make_unique<present::Mirror>( m_animSet );
	}

	ClientOptions options;
	options.host = ToStd( m_host );
	options.port = uint16_t( std::clamp( m_port, 1, 65535 ) );
	options.minRollbackTicks = uint32_t( std::max( 1, m_rollbackMin ) );
	options.maxRollbackTicks = uint32_t( std::max( m_rollbackMin, m_rollbackMax ) );
	options.logName = "godot";
	m_thread.Start( options );
}

void CinderboxClient::disconnect_from_server()
{
	m_thread.Stop();
}

bool CinderboxClient::is_running() const
{
	return m_thread.Running();
}

void CinderboxClient::_exit_tree()
{
	m_thread.Stop();
}

void CinderboxClient::_enter_tree()
{
	// HUD nodes (CbFieldLabel) find the client through this group.
	add_to_group( "cinderbox_client" );
}

void CinderboxClient::set_input( const Vector2& move, double camera_yaw, double camera_pitch, bool jump, bool sprint,
								 int64_t actions )
{
	// Godot's camera yaw: 0 looks down -Z. The simulation's: 0 looks down +Z, positive turns left
	// (toward +X). A Godot camera with rotation.y = r looks along (-sin r, 0, -cos r), which is
	// the simulation's yaw r + pi.
	PlayerInput in;
	in.moveRight = int8_t( std::clamp( int( std::lround( move.x * 127.0 ) ), -127, 127 ) );
	in.moveForward = int8_t( std::clamp( int( std::lround( move.y * 127.0 ) ), -127, 127 ) );
	in.cameraYaw = detmath::RadiansToYaw( float( camera_yaw ) + detmath::kPi );
	in.buttons = uint8_t( ( jump ? BtnJump : 0 ) | ( sprint ? BtnSprint : 0 ) );
	// Pitch: a positive Godot rotation.x looks up, which is the simulation's convention too.
	double pitchTurns = std::clamp( camera_pitch / ( 2.0 * detmath::kPi ), -0.24, 0.24 );
	in.cameraPitch = int16_t( std::clamp( int( std::lround( pitchTurns * 65536.0 ) ), -int( kMaxCameraPitch ), int( kMaxCameraPitch ) ) );
	in.actions = uint16_t( actions );
	m_thread.SetInput( in );

	// Presses are announced here, before the server has seen them, so feedback does not wait.
	uint16_t pressed = uint16_t( in.actions & ~m_lastActions );
	m_lastActions = in.actions;
	for ( const ModAction& a : m_frame.schema.actions )
	{
		if ( pressed & ( 1u << a.bit ) )
		{
			emit_signal( "action_pressed", String( a.name.c_str() ) );
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
			return LoadPrefab( "player" );
		case present::VisualKind::Ragdoll:
		{
			String path = m_prefabDir.path_join( "ragdoll.tscn" );
			return ResourceLoader::get_singleton()->exists( path ) ? LoadPrefab( "ragdoll" ) : LoadPrefab( "player" );
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
				Ref<PackedScene> prefab = Prefab( v );
				Node3D* node = nullptr;
				if ( prefab.is_valid() )
				{
					node = Object::cast_to<Node3D>( prefab->instantiate() );
				}
				if ( node == nullptr )
				{
					node = memnew( Node3D );
				}
				node->set_name( String( KindName( v.kind ) ) + "_" + String::num_int64( int64_t( v.netId ) ) );
				add_child( node );
				m_nodes[e.visual] = node->get_instance_id();

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
				emit_signal( "visual_spawned", int64_t( e.visual ), int64_t( e.netId ), String( KindName( e.kind ) ), node, position,
							 e.withEffect, TemplateName( v.templateIndex ) );
				break;
			}
			case present::EventType::Destroying:
			{
				flecs::entity ve( visuals, e.visual );
				uint32_t templateIndex = ve.is_alive() ? ve.get<present::Visual>().templateIndex : kNoTemplate;
				emit_signal( "visual_destroying", int64_t( e.visual ), int64_t( e.netId ), String( KindName( e.kind ) ), position,
							 TemplateName( templateIndex ) );
				break;
			}
			case present::EventType::Removed:
			{
				m_attachments.erase( e.visual );
				auto it = m_nodes.find( e.visual );
				if ( it != m_nodes.end() )
				{
					if ( auto* node = Object::cast_to<Node>( ObjectDB::get_instance( it->second ) ) )
					{
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
				// A character's own animation can react too (a recoil clip on "pistol.fired").
				if ( Node3D* node = get_entity_node( int64_t( e.netId ) ) )
				{
					if ( CinderboxAnimator* animator = FindInPrefab<CinderboxAnimator>( node ) )
					{
						animator->on_mod_event( name );
					}
				}
				emit_signal( "mod_event", name, int64_t( e.netId ), int64_t( e.otherNetId ), int64_t( e.value ), position,
							 ToGodot( e.vector ) );
				break;
			}
		}
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

			// The pose: evaluated for players, hung off the parts for ragdolls. State bindings
			// may aim it before it is applied.
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
			ApplyStates( id, v, pose, node, models );

			if ( models != nullptr )
			{
				if ( CinderboxSkeleton* skeleton = FindSkeleton( node ) )
				{
					skeleton->ApplyPose( *m_animSet, *models );
				}
			}
			// A prefab poses its character with ozz, with Godot's own animation system, or with
			// both; whichever it contains is what gets driven.
			if ( anim != nullptr )
			{
				if ( CinderboxAnimator* animator = FindInPrefab<CinderboxAnimator>( node ) )
				{
					animator->ApplyState( anim->current );
				}
			}
			PlaceAttachments( id, v, node );
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

// --- Mod data: fields, conditions and state bindings ------------------------------------------------

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

bool CinderboxClient::StateHolds( const CbStateBinding& state, const present::Visual& v ) const
{
	String kind = state.get_kind();
	if ( kind.is_empty() == false && kind != "any" && kind != String( KindName( v.kind ) ) )
	{
		return false;
	}
	if ( state.get_who() == CbEffect::WHO_LOCAL && v.isLocalPlayer == false )
	{
		return false;
	}
	if ( state.get_who() == CbEffect::WHO_REMOTE && v.isLocalPlayer )
	{
		return false;
	}
	const int32_t* globals = m_mirror ? m_mirror->GlobalBoard() : nullptr;
	return present::CheckConditions( m_frame.schema, Conditions( state.get_conditions() ), v.hasBoard ? &v.board : nullptr, globals );
}

void CinderboxClient::ApplyStates( uint64_t visual, const present::Visual& v, const present::RenderPose& pose, Node3D* node,
								   present::Models* models )
{
	m_active.assign( m_states.size(), false );
	CinderboxAnimator* animator = FindInPrefab<CinderboxAnimator>( node );
	for ( size_t i = 0; i < m_states.size(); ++i )
	{
		const CbStateBinding& state = **m_states[i];
		bool holds = StateHolds( state, v );
		m_active[i] = holds;

		if ( animator != nullptr && state.get_tree_parameter().is_empty() == false )
		{
			animator->set_tree_parameter( state.get_tree_parameter(), holds );
		}
		if ( holds == false || models == nullptr || v.hasAim == false || state.get_aim_bone().is_empty() )
		{
			continue;
		}
		int joint = present::FindJoint( *m_animSet, state.get_aim_bone().utf8().get_data() );
		int tip = present::FindJoint( *m_animSet, state.get_aim_tip().utf8().get_data() );
		// Where the camera looks, in the character's own frame (feet at the origin, facing +Z).
		b3CosSin p = b3ComputeCosSin( v.aimPitch );
		b3CosSin y = b3ComputeCosSin( v.aimYaw );
		b3Vec3 world = { y.sine * p.cosine, p.sine, y.cosine * p.cosine };
		b3Vec3 local = b3RotateVector( b3Conjugate( pose.rotation ), world );
		present::AimChain( *m_animSet, *models, joint, tip, local, state.get_aim_weight() );
	}
	(void)visual;
}

// Attached scenes follow their joint. They live under the visual's node, so they vanish with it.
void CinderboxClient::PlaceAttachments( uint64_t visual, const present::Visual& v, Node3D* node )
{
	std::vector<ObjectID>& attached = m_attachments[visual];
	attached.resize( m_states.size() );
	CinderboxSkeleton* skeleton = FindSkeleton( node );
	for ( size_t i = 0; i < m_states.size(); ++i )
	{
		const CbStateBinding& state = **m_states[i];
		Node3D* item = Object::cast_to<Node3D>( ObjectDB::get_instance( attached[i] ) );
		bool wanted = i < m_active.size() && m_active[i] && state.get_attach_scene().is_empty() == false && v.dead == false;
		if ( wanted == false )
		{
			if ( item != nullptr )
			{
				item->queue_free();
			}
			attached[i] = ObjectID();
			continue;
		}
		if ( item == nullptr )
		{
			Ref<PackedScene> scene = LoadScene( state.get_attach_scene() );
			item = scene.is_valid() ? Object::cast_to<Node3D>( scene->instantiate() ) : nullptr;
			if ( item == nullptr )
			{
				continue;
			}
			node->add_child( item );
			attached[i] = item->get_instance_id();
		}

		Transform3D joint;
		if ( skeleton == nullptr || skeleton->JointTransform( state.get_attach_bone(), joint ) == false )
		{
			item->set_visible( false );
			continue;
		}
		Vector3 degrees = state.get_attach_rotation();
		Basis turn = Basis::from_euler( Vector3( Math::deg_to_rad( degrees.x ), Math::deg_to_rad( degrees.y ), Math::deg_to_rad( degrees.z ) ) );
		Transform3D offset( turn, state.get_attach_offset() );
		item->set_visible( true );
		item->set_global_transform( skeleton->get_global_transform() * joint * offset );
	}
}

void CinderboxClient::add_state_binding( const Ref<CbStateBinding>& binding )
{
	if ( binding.is_valid() )
	{
		m_states.push_back( binding );
	}
}

void CinderboxClient::clear_state_bindings()
{
	m_states.clear();
	for ( auto& entry : m_attachments )
	{
		for ( ObjectID id : entry.second )
		{
			if ( auto* node = Object::cast_to<Node>( ObjectDB::get_instance( id ) ) )
			{
				node->queue_free();
			}
		}
	}
	m_attachments.clear();
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
	const int32_t* globals = m_mirror ? m_mirror->GlobalBoard() : nullptr;
	present::FieldValue value = present::ReadField( m_frame.schema, ToStd( name ), BoardOf( uint32_t( net_id ) ), globals );
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

bool CinderboxClient::check_conditions( int64_t net_id, const PackedStringArray& conditions ) const
{
	const int32_t* globals = m_mirror ? m_mirror->GlobalBoard() : nullptr;
	return present::CheckConditions( m_frame.schema, Conditions( conditions ), BoardOf( uint32_t( net_id ) ), globals );
}

bool CinderboxClient::check_local_conditions( const PackedStringArray& conditions ) const
{
	return check_conditions( get_local_net_id(), conditions );
}

String CinderboxClient::format_local_fields( const String& format ) const
{
	const int32_t* globals = m_mirror ? m_mirror->GlobalBoard() : nullptr;
	std::string text = present::FormatFields( m_frame.schema, ToStd( format ), BoardOf( uint32_t( get_local_net_id() ) ), globals );
	return String::utf8( text.c_str() );
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
		// The same point the server casts the crosshair ray from (Context::EyePosition).
		return ToGodot( pose.position ) + Vector3( 0, 0.4f, 0 );
	}
	return Vector3( 0, 1, 0 );
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

void CinderboxClient::_process( double delta )
{
	if ( Engine::get_singleton()->is_editor_hint() || !m_mirror || m_thread.Running() == false )
	{
		return;
	}

	if ( m_thread.TakeFrame( m_frame ) )
	{
		m_haveFrame = true;
	}
	if ( m_haveFrame == false )
	{
		return;
	}

	String state = get_connection_state();
	if ( state != m_lastState )
	{
		m_lastState = state;
		emit_signal( "connection_state_changed", state );
	}
	if ( m_frame.schemaGeneration != m_schemaGeneration )
	{
		m_schemaGeneration = m_frame.schemaGeneration;
		emit_signal( "schema_changed" );
	}
	if ( m_frame.hasSimulation == false )
	{
		return;
	}

	// Interpolate from the moment the frame was published.
	float tickSeconds = m_frame.frame.tickSeconds;
	double since = m_thread.Now() - m_frame.publishedAt;
	float alpha = m_frame.state == ClientState::Playing
					  ? std::clamp( float( m_frame.alphaAtPublish + since / tickSeconds ), 0.0f, 1.0f )
					  : m_frame.alphaAtPublish;

	m_mirror->Update( m_frame.frame, alpha, float( delta ) );
	// A rollback is reported once; later updates of the same frame are ordinary.
	m_frame.frame.rolledBack = false;

	UpdateMapVisual();
	HandleEvents();
	UpdateNodes();
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
			node->queue_free();
		}
	}
	m_nodes.clear();

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
	Node3D* node = scene.is_valid() ? Object::cast_to<Node3D>( scene->instantiate() ) : nullptr;
	if ( node == nullptr )
	{
		UtilityFunctions::push_warning( "Cinderbox: cannot instantiate ", path );
		return;
	}
	node->set_name( "MapVisual" );
	add_child( node );
	m_mapVisual = node->get_instance_id();
	m_hideStaticBoxes = true;
}

Dictionary CinderboxClient::get_stats() const
{
	Dictionary d;
	const auto& s = m_frame.stats;
	d["state"] = get_connection_state();
	d["reject_reason"] = String( m_frame.rejectReason.c_str() );
	d["tick"] = int64_t( m_frame.currentTick );
	d["confirmed_tick"] = int64_t( m_frame.confirmedTick );
	d["rollback_window"] = int64_t( m_frame.rollbackWindow );
	d["rtt_ms"] = int64_t( s.rttMs );
	d["clock_error"] = s.tickError;
	d["rate_scale"] = s.rateScale;
	d["rollbacks"] = int64_t( m_frame.rollback.rollbacks );
	d["last_rollback_depth"] = int64_t( m_frame.rollback.lastRollbackDepth );
	d["resimulated_ticks"] = int64_t( m_frame.rollback.resimulatedTicks );
	d["stalled_seconds"] = s.stalledSeconds;
	d["checksums_verified"] = int64_t( s.checksumsVerified );
	d["desyncs"] = int64_t( s.desyncs );
	d["welcomes"] = int64_t( s.welcomes );
	d["client_work_ms"] = s.simMsLastFrame;
	d["kbit_down_total"] = double( s.bytesReceived ) * 8.0 / 1000.0;
	d["entities"] = int64_t( m_frame.frame.entities.size() );
	d["fingerprint"] = String::num_uint64( m_frame.fingerprint, 16 );
	d["fp_environment_ok"] = m_frame.fpEnvironmentOk;
	d["animation"] = m_animSet ? String( m_animSet->Description().c_str() ) : String();
	return d;
}

String CinderboxClient::get_connection_state() const
{
	if ( m_thread.Running() == false )
	{
		return "stopped";
	}
	if ( m_haveFrame == false )
	{
		return "starting";
	}
	return String( ToString( m_frame.state ) );
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
