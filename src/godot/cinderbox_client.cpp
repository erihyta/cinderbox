#include "cinderbox_client_parts.h"

namespace cb::gd
{


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

