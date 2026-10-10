#include "cinderbox_client_parts.h"

namespace cb::gd
{

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

// (Shared with the world's part: cinderbox_client_parts.h.)

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

void CinderboxClient::UpdateItem( uint64_t visual, const present::Visual& v, const present::RenderPose& pose, Node3D* node )
{
	// Floating in the viewer's own first-person view (CbItem.view_camera): out of its socket's
	// frame while it does, and back in it the moment it does not (dropped, put away, another view).
	static const StringName kFloating( "cb_floating" );
	Transform3D eye;
	bool floats = m_firstPerson && v.holder != 0 && v.holder == m_frame.frame.localNetId && v.stowed == false && v.socket <= kSocketLeftHand &&
				  ViewFrameOf( v.itemKind, eye );
	bool floated = node->has_meta( kFloating );
	if ( floated && floats == false )
	{
		node->remove_meta( kFloating );
		node->set_as_top_level( false );
	}
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
	else if ( floated && floats == false )
	{
		node->set_transform( SceneInSocket( node, v.socket ) );
	}
	if ( floats )
	{
		if ( floated == false )
		{
			node->set_as_top_level( true );
		}
		node->set_meta( kFloating, eye );
		node->set_global_transform( ViewItemTransform( eye ) );
		m_viewItems.push_back( ObjectID( node->get_instance_id() ) );
	}
	node->set_visible( true );
}

} // namespace cb::gd
