#include "cinderbox_client_parts.h"

namespace cb::gd
{

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
	// Players by slot: stable while they are connected, so paths can name them. A body nobody
	// drives by its NetId.
	if ( v.kind == present::VisualKind::Player )
	{
		return v.slot < kMaxPlayers ? "player_" + String::num_int64( int64_t( v.slot ) ) : "body_" + String::num_int64( int64_t( v.netId ) );
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
		else if ( line.begins_with( "viewturn " ) )
		{
			PackedFloat64Array v = line.substr( 9 ).split_floats( " ", false );
			if ( v.size() == 3 )
			{
				m_itemViewTurns[key] = Vector3( float( v[0] ), float( v[1] ), float( v[2] ) );
			}
		}
		else if ( line.begins_with( "viewhands " ) )
		{
			PackedFloat64Array v = line.substr( 10 ).split_floats( " ", false );
			if ( v.size() == 2 )
			{
				m_itemViewHands[key] = { v[0] != 0.0, v[1] != 0.0 };
			}
		}
		else if ( line.begins_with( "viewfov " ) )
		{
			m_itemViewFovs[key] = float( line.substr( 8 ).to_float() );
		}
		else if ( line.begins_with( "viewfrom " ) )
		{
			PackedFloat64Array v = line.substr( 9 ).split_floats( " ", false );
			if ( v.size() == 7 )
			{
				Quaternion turn{ float( v[3] ), float( v[4] ), float( v[5] ), float( v[6] ) };
				m_itemViewFrames[key] = Transform3D( Basis( turn.normalized() ), Vector3( float( v[0] ), float( v[1] ), float( v[2] ) ) );
			}
		}
	}
}

void CinderboxClient::clear_world_scenes()
{
	m_itemLooks.clear();
	m_itemNames.clear();
	m_itemViewOffsets.clear();
	m_itemViewFrames.clear();
	m_itemViewFovs.clear();
	m_itemViewHands.clear();
	m_itemViewTurns.clear();
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
		hash = Mix( hash, 0x30000u + ( v.linked ? 1u : 0u ) + ( v.linkHolds ? 2u : 0u ) + ( v.linkMissed ? 4u : 0u ) );
		hash = Mix( hash, 0x40000u + ( v.kind == present::VisualKind::Item && v.holder != 0 && v.stowed == false ? 1u : 0u ) );
		bool firstPerson = m_firstPerson && v.kind == present::VisualKind::Player && v.netId == m_frame.frame.localNetId;
		hash = Mix( hash, 0x50000u + ( firstPerson ? 1u : 0u ) );
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
			// "linked": a link of its is out; "link_holds": and it has taken hold; "link_missed": it
			// found nothing, and flies out its range all the same.
			// "first_person": the viewer looks out of this player's eyes (its own). For what only the
			// first-person view shows: a held item's swing in front of the camera.
			state["first_person"] = firstPerson;
			state["linked"] = v.linked;
			// "container_open": a container (a chest) is open to it.
			state["container_open"] = v.container != 0;
			state["link_holds"] = v.linked && v.linkHolds;
			state["link_missed"] = v.linked && v.linkMissed;
			// "motion.<name>": that motion of its is on.
			for ( const auto& [key, bit] : MotionKeys() )
			{
				state[key] = ( ( v.motionsOn >> bit ) & 1 ) != 0;
			}
			for ( size_t kind = 0; kind < schema.itemKinds.size(); ++kind )
			{
				state[String::utf8( schema.itemKinds[kind].c_str() )] = Holds( v.netId, uint16_t( kind ) );
				// "carried.<kind>": it has one, in the hand or put away.
				auto carried = m_carriedKinds.find( v.netId );
				state[String::utf8( ( "carried." + schema.itemKinds[kind] ).c_str() )] =
					carried != m_carriedKinds.end() && std::find( carried->second.begin(), carried->second.end(), uint16_t( kind ) ) != carried->second.end();
				// "using.<kind>": it uses one this tick (the button down with it in the hand, or its own key).
				auto using_ = m_usingKinds.find( v.netId );
				state[String::utf8( ( "using." + schema.itemKinds[kind] ).c_str() )] =
					using_ != m_usingKinds.end() && std::find( using_->second.begin(), using_->second.end(), uint16_t( kind ) ) != using_->second.end();
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

void CinderboxClient::RetargetTracks( ViewCharacter& character, Node* entity, Node* root )
{
	if ( character.tracks.is_null() || m_socketMoves.empty() || character.retargeted )
	{
		return;
	}
	character.retargeted = true;
	// Track paths are from `root`; the moves are from the entity.
	String rootFromEntity = String( entity->get_path_to( root ) );
	String entityFromRoot = String( root->get_path_to( entity ) );
	TypedArray<StringName> names = character.tracks->get_animation_list();
	for ( int64_t i = 0; i < names.size(); ++i )
	{
		Ref<Animation> animation = character.tracks->get_animation( names[i] );
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
	HideMapLooks();
	// Drawn by its own node in the map's scene (a prop's meshes), copied; else a prefab.
	Node3D* node = MapLookCopy( v );
	Ref<PackedScene> prefab = node == nullptr ? Prefab( v ) : Ref<PackedScene>();
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
	// Its hitboxes are where they were authored; the server and the simulation read the baked
	// ones (hitboxes.cfg). Gone from what is drawn, with the bone attachments they leave empty
	// (on the mannequin, 11 of 15: the sockets' stay), each a node Godot would follow every frame.
	if ( v.kind == present::VisualKind::Player )
	{
		TypedArray<Node> hitboxes = node->find_children( "*", "CbHitbox", true, false );
		for ( int64_t i = 0; i < hitboxes.size(); ++i )
		{
			if ( Node* hitbox = Object::cast_to<Node>( hitboxes[i] ) )
			{
				Node* attachment = hitbox->get_parent();
				attachment->remove_child( hitbox );
				memdelete( hitbox );
				if ( Object::cast_to<BoneAttachment3D>( attachment ) != nullptr && attachment->get_child_count() == 0 )
				{
					attachment->get_parent()->remove_child( attachment );
					memdelete( attachment );
				}
			}
		}
	}
	if ( v.kind == present::VisualKind::Item && v.holder == 0 )
	{
		LeanItem( node ); // (before it is in the tree: what is about a holder never registers)
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
	if ( v.kind == present::VisualKind::Player || v.kind == present::VisualKind::Ragdoll )
	{
		m_nodeCharacters[visual] = v.character;
	}
	ViewCharacter* view = v.kind == present::VisualKind::Player ? ViewOf( v.character ) : nullptr;
	if ( CbCharacter* character = view != nullptr ? FindInPrefab<CbCharacter>( node ) : nullptr )
	{
		if ( view->tracksBuilt == false )
		{
			view->tracksBuilt = true;
			Ref<AnimationLibrary> library = character->build_track_library();
			if ( library->get_animation_list().is_empty() == false )
			{
				// Its method tracks call methods on the character's nodes: only listed ones.
				String problem = cue::CheckResource( library );
				if ( problem.is_empty() )
				{
					view->tracks = library;
				}
				else
				{
					UtilityFunctions::push_warning( "Cinderbox: the animations of ", view->folder,
													" play their bones only: a track has ", problem );
				}
			}
		}
		// The character's own AnimationPlayer names the root its tracks' paths start from.
		if ( view->tracks.is_valid() )
		{
			auto* source = Object::cast_to<AnimationPlayer>( character->get_node_or_null( character->get_animation_player_path() ) );
			Node* root = source != nullptr ? source->get_node_or_null( source->get_root_node() ) : nullptr;
			if ( root != nullptr )
			{
				auto* tracks = memnew( CbTrackPlayer );
				tracks->set_name( "TrackPlayer" );
				source->get_parent()->add_child( tracks );
				RetargetTracks( *view, node, root );
				tracks->setup( view->tracks, root );
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

void CinderboxClient::rebuild_nodes()
{
	RebuildCharacterNodes( true );
}

void CinderboxClient::RebuildCharacterNodes( bool itemsToo, const std::vector<uint32_t>& only )
{
	if ( !m_mirror )
	{
		return;
	}
	auto picked = [&]( uint32_t netId ) { return std::find( only.begin(), only.end(), netId ) != only.end(); };
	// (The characters first: an item goes into its holder's new socket.)
	std::vector<std::pair<uint64_t, present::Visual>> rebuild, items;
	m_mirror->ForEach( [&]( uint64_t id, const present::Visual& v, const present::RenderPose&, const present::PlayerAnim*,
							const present::RagdollAnim* ) {
		if ( ( v.kind == present::VisualKind::Player || v.kind == present::VisualKind::Ragdoll ) && m_nodes.count( id ) &&
			 ( only.empty() || ( v.kind == present::VisualKind::Player && picked( v.netId ) ) ) )
		{
			rebuild.emplace_back( id, v );
		}
		else if ( ( itemsToo || ( only.empty() == false && picked( v.holder ) ) ) && v.kind == present::VisualKind::Item &&
				  m_nodes.count( id ) )
		{
			items.emplace_back( id, v );
		}
	} );
	rebuild.insert( rebuild.end(), items.begin(), items.end() );
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

std::shared_ptr<const AnimGraph> CinderboxClient::ServerGraph( const anim::AnimSet& set, size_t character )
{
	const CharacterInfo& info = m_frame.schema.characters[character];
	const std::string& text = info.graph;
	String name = info.name.empty() ? String( "built-in" ) : String::utf8( info.name.c_str() );
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

namespace
{
// A folder's baked files (from a mounted item), read for anim::AnimSet::Load.
anim::FileReader ReadFrom( const String& folder )
{
	return [folder]( const std::string& file, std::string& bytes ) {
		String path = folder + String::utf8( file.c_str() );
		if ( FileAccess::file_exists( path ) == false )
		{
			return false;
		}
		PackedByteArray data = FileAccess::get_file_as_bytes( path );
		bytes.assign( reinterpret_cast<const char*>( data.ptr() ), size_t( data.size() ) );
		return true;
	};
}
} // namespace

void CinderboxClient::ServerPacks( AnimGraphPacks& packs, std::vector<std::shared_ptr<const anim::AnimSet>>& sets )
{
	std::string warnings;
	packs = CompileAnimPacks( m_frame.schema, warnings );
	sets.assign( packs.size(), nullptr );
	for ( size_t i = 0; i < packs.size(); ++i )
	{
		if ( !packs[i] )
		{
			continue;
		}
		// The pack's baked files, from its mod's item (mounted like every other item).
		const AnimPackInfo& info = m_frame.schema.animPacks[i];
		String folder = "res://anim/" + String::utf8( info.name.c_str() ) + "/";
		std::string error;
		sets[i] = anim::AnimSet::Load( ReadFrom( folder ), ToStd( folder ), error, warnings );
		if ( sets[i] == nullptr )
		{
			warnings += "animation pack " + info.name + ": " + error + "; ";
		}
	}
	if ( warnings.empty() == false )
	{
		UtilityFunctions::push_warning( "Cinderbox animation packs: ", String::utf8( warnings.c_str() ) );
	}
}

String CinderboxClient::get_character() const
{
	return m_frame.schema.characters.empty() ? String() : String::utf8( m_frame.schema.characters.front().name.c_str() );
}

CinderboxClient::ViewCharacter* CinderboxClient::ViewOf( uint8_t character )
{
	if ( m_characters.empty() )
	{
		return nullptr;
	}
	ViewCharacter& view = m_characters[character < m_characters.size() ? character : 0];
	return view.loaded ? &view : &m_characters.front();
}

float CinderboxClient::FeetOf( uint64_t visual ) const
{
	if ( !m_mirror )
	{
		return present::kFeetOffset;
	}
	flecs::entity ve( m_mirror->World(), visual );
	uint8_t character = ve.is_alive() && ve.has<present::Visual>() ? ve.get<present::Visual>().character : 0;
	return m_mirror->Library().Of( character ).feet;
}

String CinderboxClient::use_characters()
{
	const std::vector<CharacterInfo>& infos = m_frame.schema.characters;
	std::vector<ViewCharacter> views( std::max<size_t>( infos.size(), 1 ) );
	bool changed = views.size() != m_characters.size();
	for ( size_t i = 0; i < views.size(); ++i )
	{
		String name = i < infos.size() ? String::utf8( infos[i].name.c_str() ) : String();
		ViewCharacter& view = views[i];
		if ( i < m_characters.size() && m_characters[i].name == name && m_characters[i].set )
		{
			view = m_characters[i]; // the same one: what was read of it stays
			continue;
		}
		changed = true;
		view.name = name;
		if ( name.is_empty() )
		{
			if ( i == 0 )
			{
				m_animSet.reset();
				EnsureAnimations(); // the built-in rig, or --animations
				view.set = m_animSet;
				view.loaded = true;
			}
			continue;
		}
		String folder = "res://characters/" + name + "/";
		std::string error, warnings;
		std::shared_ptr<const anim::AnimSet> loaded = anim::AnimSet::Load( ReadFrom( folder ), ToStd( folder ), error, warnings );
		String problem;
		if ( loaded == nullptr )
		{
			problem = String::utf8( ( "character " + ToStd( name ) + ": " + error ).c_str() );
		}
		else if ( ResourceLoader::get_singleton()->exists( folder + "character.tscn" ) == false )
		{
			problem = "character " + name + " has no " + folder + "character.tscn";
		}
		if ( problem.is_empty() == false )
		{
			if ( i == 0 )
			{
				return problem;
			}
			// (Its body still moves as the simulation has it: its capsule, its state machine.)
			UtilityFunctions::push_warning( "Cinderbox: ", problem, "; drawn with the server's own character's look" );
			continue;
		}
		if ( warnings.empty() == false )
		{
			UtilityFunctions::push_warning( "Cinderbox character ", name, ": ", String::utf8( warnings.c_str() ) );
		}
		UtilityFunctions::print( "Cinderbox character: ", name, " (", String::utf8( loaded->Description().c_str() ), ")" );
		view.folder = folder;
		view.set = std::move( loaded );
		view.loaded = true;
	}
	if ( changed && views.front().name.is_empty() )
	{
		UtilityFunctions::print( "Cinderbox character: built-in (", String::utf8( views.front().set->Description().c_str() ), ")" );
	}
	m_animSet = views.front().set;

	// Each as the mirror poses it: a character that could not be read is posed on the server's own
	// skeleton, by its own state machine, its feet where its own capsule has them.
	AnimGraphPacks packs;
	std::vector<std::shared_ptr<const anim::AnimSet>> packSets;
	ServerPacks( packs, packSets );
	std::string fitWarnings;
	std::vector<present::CharacterAnim> anims( views.size() );
	for ( size_t i = 0; i < views.size(); ++i )
	{
		const auto& set = views[i].loaded ? views[i].set : views.front().set;
		present::CharacterAnim& anim = anims[i];
		anim.set = set;
		anim.graph = i < infos.size() ? ServerGraph( *set, i ) : nullptr;
		anim.packClips.assign( packs.size(), nullptr );
		for ( size_t p = 0; p < packs.size(); ++p )
		{
			if ( packs[p] && packSets[p] )
			{
				anim.packClips[p] = anim::FitPack( packSets[p], *packs[p], *set, fitWarnings );
			}
		}
		if ( i < infos.size() )
		{
			anim.feet = mover::FeetBelowCenter( infos[i].radius, infos[i].halfHeight );
		}
	}
	if ( fitWarnings.empty() == false )
	{
		UtilityFunctions::push_warning( "Cinderbox animation packs: ", String::utf8( fitWarnings.c_str() ) );
	}
	m_characters = std::move( views );
	if ( m_mirror )
	{
		m_mirror->SetCharacters( std::move( anims ), std::move( packs ) );
		m_mirror->SetItemShapes( m_frame.schema.itemShapes );
	}
	if ( changed )
	{
		RebuildCharacterNodes();
	}
	return String();
}

} // namespace cb::gd
