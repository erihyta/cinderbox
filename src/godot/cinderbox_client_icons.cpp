#include "cinderbox_client_parts.h"

#include <godot_cpp/classes/directional_light3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>

namespace cb::gd
{

// --- Icons: what an item kind looks like in an inventory's slot -----------------------------------
//
// Its CbItem's icon; without one, a picture of its scene taken once: the scene, with what reacts to
// the game taken out, in a world of its own with a camera and a light, framed on its meshes. A few
// frames later the picture is kept and the viewport is gone.

namespace
{

constexpr int kIconSize = 128;
constexpr int kIconFrames = 3; // frames the viewport renders before its picture is kept

// What reacts to the game (reactions, predictions, the motion nodes): not in a picture.
void Strip( Node* node )
{
	for ( int i = node->get_child_count() - 1; i >= 0; --i )
	{
		Node* child = node->get_child( i );
		if ( child->is_class( "CbReaction" ) || child->is_class( "CbPrediction" ) || child->is_class( "CbMotionPart" ) ||
			 child->is_class( "CbMotion" ) || child->is_class( "CollisionShape3D" ) )
		{
			node->remove_child( child );
			memdelete( child );
			continue;
		}
		Strip( child );
	}
}

// Its meshes' box, in the root's frame (the scene is at the origin).
bool Bounds( Node* node, AABB& out, bool& any )
{
	if ( auto* visual = Object::cast_to<VisualInstance3D>( node ) )
	{
		AABB box = visual->get_global_transform().xform( visual->get_aabb() );
		out = any ? out.merge( box ) : box;
		any = true;
	}
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		Bounds( node->get_child( i ), out, any );
	}
	return any;
}

} // namespace

Ref<Texture2D> CinderboxClient::IconOfKind( const std::string& kind )
{
	Icon& icon = m_icons[kind];
	if ( icon.texture.is_valid() || icon.failed || icon.viewport.is_valid() )
	{
		return icon.texture;
	}
	auto look = m_itemLooks.find( kind );
	Ref<PackedScene> scene = look != m_itemLooks.end() ? LoadScene( look->second ) : Ref<PackedScene>();
	auto* root = scene.is_valid() ? Object::cast_to<Node3D>( cue::Instantiate( scene ) ) : nullptr;
	if ( root == nullptr )
	{
		icon.failed = true;
		return icon.texture;
	}
	// One its author drew.
	Ref<Texture2D> drawn = root->get( "icon" );
	if ( drawn.is_valid() )
	{
		icon.texture = drawn;
		memdelete( root );
		return icon.texture;
	}
	if ( DisplayServer::get_singleton() == nullptr || DisplayServer::get_singleton()->get_name() == String( "headless" ) )
	{
		icon.failed = true; // (nothing renders)
		memdelete( root );
		return icon.texture;
	}
	Strip( root );
	auto* viewport = memnew( SubViewport );
	viewport->set_name( "Icon" );
	viewport->set_size( Vector2i( kIconSize, kIconSize ) );
	viewport->set_transparent_background( true );
	viewport->set_use_own_world_3d( true );
	viewport->set_update_mode( SubViewport::UPDATE_ALWAYS );
	viewport->add_child( root );
	add_child( viewport );
	AABB box;
	bool any = false;
	Bounds( root, box, any );
	if ( any == false )
	{
		box = AABB( Vector3( -0.1f, -0.1f, -0.1f ), Vector3( 0.2f, 0.2f, 0.2f ) );
	}
	// From the front, a little above and to the side; orthographic, the whole of it in view.
	Vector3 center = box.get_center();
	real_t radius = std::max( box.size.length() * 0.5f, real_t( 0.02f ) );
	Vector3 from = Vector3( 0.6f, 0.45f, 1.0f ).normalized();
	auto* camera = memnew( Camera3D );
	camera->set_projection( Camera3D::PROJECTION_ORTHOGONAL );
	camera->set_size( radius * 2.1f );
	camera->set_near( 0.01f );
	camera->set_far( radius * 8.0f + 1.0f );
	viewport->add_child( camera );
	camera->look_at_from_position( center + from * ( radius * 3.0f ), center, Vector3( 0, 1, 0 ) );
	camera->set_current( true );
	auto* light = memnew( DirectionalLight3D );
	viewport->add_child( light );
	light->look_at_from_position( Vector3( 1, 2, 2 ), Vector3(), Vector3( 0, 1, 0 ) );
	icon.viewport = ObjectID( viewport->get_instance_id() );
	icon.frames = 0;
	return icon.texture;
}

void CinderboxClient::UpdateIcons()
{
	for ( auto& [kind, icon] : m_icons )
	{
		auto* viewport = Object::cast_to<SubViewport>( ObjectDB::get_instance( icon.viewport ) );
		if ( icon.viewport.is_valid() == false || viewport == nullptr )
		{
			continue;
		}
		if ( ++icon.frames < kIconFrames )
		{
			continue;
		}
		// The picture is kept; the viewport, its world and the copy of the scene go.
		Ref<ViewportTexture> rendered = viewport->get_texture();
		Ref<Image> image = rendered.is_valid() ? rendered->get_image() : Ref<Image>();
		if ( image.is_valid() && image->is_empty() == false )
		{
			icon.texture = ImageTexture::create_from_image( image );
		}
		else
		{
			icon.failed = true;
		}
		viewport->queue_free();
		icon.viewport = ObjectID();
	}
}

Ref<Texture2D> CinderboxClient::get_item_icon( int64_t item )
{
	flecs::entity ve = m_mirror && item != 0 ? m_mirror->VisualOf( uint32_t( item ) ) : flecs::entity();
	if ( ve.is_valid() == false )
	{
		return Ref<Texture2D>();
	}
	const present::Visual& v = ve.get<present::Visual>();
	if ( v.kind != present::VisualKind::Item || v.itemKind >= m_frame.schema.itemKinds.size() )
	{
		return Ref<Texture2D>();
	}
	return IconOfKind( m_frame.schema.itemKinds[v.itemKind] );
}

} // namespace cb::gd
