#include "cinderbox_client.h"

#include "cinderbox_animator.h"
#include "cinderbox_skeleton.h"
#include "detmath.h"
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
	ClassDB::bind_method( D_METHOD( "set_input", "move", "camera_yaw", "jump", "sprint", "spawn_prop" ), &CinderboxClient::set_input );
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

void CinderboxClient::set_input( const Vector2& move, double camera_yaw, bool jump, bool sprint, bool spawn_prop )
{
	// Godot's camera yaw: 0 looks down -Z. The simulation's: 0 looks down +Z, positive turns left
	// (toward +X). A Godot camera with rotation.y = r looks along (-sin r, 0, -cos r), which is
	// the simulation's yaw r + pi.
	PlayerInput in;
	in.moveRight = int8_t( std::clamp( int( std::lround( move.x * 127.0 ) ), -127, 127 ) );
	in.moveForward = int8_t( std::clamp( int( std::lround( move.y * 127.0 ) ), -127, 127 ) );
	in.cameraYaw = detmath::RadiansToYaw( float( camera_yaw ) + detmath::kPi );
	in.buttons = uint8_t( ( jump ? BtnJump : 0 ) | ( sprint ? BtnSprint : 0 ) );
	(void)spawn_prop;
	m_thread.SetInput( in );
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

				if ( v.kind == present::VisualKind::Player )
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
		}
	}
}

void CinderboxClient::UpdateNodes()
{
	m_mirror->ForEach( [&]( uint64_t id, const present::Visual& v, const present::RenderPose& pose, const present::PlayerAnim* anim ) {
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
		if ( v.kind == present::VisualKind::Player )
		{
			Vector3 feet = ToGodot( pose.position ) - Vector3( 0, present::kFeetOffset, 0 );
			node->set_transform( Transform3D( rotation.scaled( Vector3( s, s, s ) ), feet ) );
			if ( anim != nullptr )
			{
				// A prefab poses its character with ozz, with Godot's own animation system, or
				// with both; whichever it contains is what gets driven.
				if ( anim->evaluator )
				{
					if ( CinderboxSkeleton* skeleton = FindSkeleton( node ) )
					{
						skeleton->ApplyPose( *anim->evaluator );
					}
				}
				if ( CinderboxAnimator* animator = FindInPrefab<CinderboxAnimator>( node ) )
				{
					animator->ApplyState( anim->current );
				}
			}
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
