#include "cinderbox_item_look.h"

#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

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
	ClassDB::bind_method( D_METHOD( "set_mass", "value" ), &CbItem::set_mass );
	ClassDB::bind_method( D_METHOD( "get_mass" ), &CbItem::get_mass );
	ClassDB::bind_method( D_METHOD( "set_view_offset", "value" ), &CbItem::set_view_offset );
	ClassDB::bind_method( D_METHOD( "get_view_offset" ), &CbItem::get_view_offset );
	ClassDB::bind_method( D_METHOD( "set_properties", "value" ), &CbItem::set_properties );
	ClassDB::bind_method( D_METHOD( "get_properties" ), &CbItem::get_properties );
	ClassDB::bind_method( D_METHOD( "bake" ), &CbItem::bake );
	ClassDB::bind_method( D_METHOD( "bake_to_project" ), &CbItem::bake_to_project );
	ClassDB::bind_method( D_METHOD( "get_bake_button" ), &CbItem::get_bake_button );
	ADD_GROUP( "Item", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "kind", PROPERTY_HINT_PLACEHOLDER_TEXT, "melee.bat" ), "set_kind", "get_kind" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "display_name", PROPERTY_HINT_PLACEHOLDER_TEXT, "Bat" ), "set_display_name",
				  "get_display_name" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "mass", PROPERTY_HINT_RANGE, "0.01,1000,0.01,suffix:kg" ), "set_mass", "get_mass" );
	ADD_PROPERTY( PropertyInfo( Variant::DICTIONARY, "properties", PROPERTY_HINT_DICTIONARY_TYPE, "String;float" ), "set_properties",
				  "get_properties" );
	ADD_GROUP( "First person", "" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR3, "view_offset", PROPERTY_HINT_NONE, "suffix:m" ), "set_view_offset", "get_view_offset" );
	ADD_GROUP( "", "" );
	ADD_PROPERTY( PropertyInfo( Variant::CALLABLE, "bake_button", PROPERTY_HINT_TOOL_BUTTON, "Bake item,Save", PROPERTY_USAGE_EDITOR ), "",
				  "get_bake_button" );
}

void CbGrip::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_align_rotation", "value" ), &CbGrip::set_align_rotation );
	ClassDB::bind_method( D_METHOD( "get_align_rotation" ), &CbGrip::get_align_rotation );
	ClassDB::bind_static_method( "CbGrip", D_METHOD( "carry_frame_under", "root" ), &CbGrip::carry_frame_under );
	ClassDB::bind_method( D_METHOD( "set_hand", "value" ), &CbGrip::set_hand );
	ClassDB::bind_method( D_METHOD( "get_hand" ), &CbGrip::get_hand );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "hand", PROPERTY_HINT_ENUM, "The other hand,The carrying hand" ), "set_hand", "get_hand" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "align_rotation" ), "set_align_rotation", "get_align_rotation" );
	ClassDB::bind_method( D_METHOD( "set_as_animated", "value" ), &CbGrip::set_as_animated );
	ClassDB::bind_method( D_METHOD( "get_as_animated" ), &CbGrip::get_as_animated );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "as_animated" ), "set_as_animated", "get_as_animated" );
	BIND_ENUM_CONSTANT( HAND_OTHER );
	BIND_ENUM_CONSTANT( HAND_CARRYING );
}

void CbLinkLook::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_motion", "value" ), &CbLinkLook::set_motion );
	ClassDB::bind_method( D_METHOD( "get_motion" ), &CbLinkLook::get_motion );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "motion", PROPERTY_HINT_PLACEHOLDER_TEXT, "grapple.moves/Hook (empty: any link)" ), "set_motion",
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

// A scene's grips for one hand.
std::vector<CbGrip*> GripsUnder( const Node* root, int hand )
{
	std::vector<CbGrip*> out;
	TypedArray<Node> found = const_cast<Node*>( root )->find_children( "*", "CbGrip", true, false );
	for ( int i = 0; i < found.size(); ++i )
	{
		auto* grip = Object::cast_to<CbGrip>( found[i] );
		if ( grip != nullptr && grip->get_hand() == hand )
		{
			out.push_back( grip );
		}
	}
	return out;
}

// The item's body: the first CollisionShape3D in its scene.
const CollisionShape3D* BodyOf( const Node* item )
{
	TypedArray<Node> found = const_cast<Node*>( item )->find_children( "*", "CollisionShape3D", true, false );
	return found.size() > 0 ? Object::cast_to<CollisionShape3D>( found[0] ) : nullptr;
}

String ShapeProblem( const CollisionShape3D* body )
{
	if ( body == nullptr )
	{
		return "Give it a CollisionShape3D child with a BoxShape3D or a SphereShape3D: its body when it lies in the world.";
	}
	Ref<Shape3D> shape = body->get_shape();
	if ( shape.is_null() )
	{
		return "Give its CollisionShape3D a BoxShape3D or a SphereShape3D.";
	}
	if ( Object::cast_to<BoxShape3D>( shape.ptr() ) == nullptr && Object::cast_to<SphereShape3D>( shape.ptr() ) == nullptr )
	{
		return "An item's body is a box or a sphere (" + shape->get_class() + " cannot be baked).";
	}
	// It may be turned any way (the bake writes its turn in the frame the item is carried in), but
	// not scaled: the shape's size is the body's.
	Transform3D t = InSceneFrame( body, SceneRoot( body ) );
	if ( t.basis.get_scale().is_equal_approx( Vector3( 1, 1, 1 ) ) == false || t.basis.determinant() < 0.0f )
	{
		return "Keep its CollisionShape3D unscaled (set the shape's size instead).";
	}
	return String();
}

} // namespace

Transform3D CbGrip::CarryFrame( const Node* in )
{
	return CarryFrameUnder( SceneRoot( in ) );
}

Transform3D CbGrip::CarryFrameUnder( const Node* root )
{
	std::vector<CbGrip*> carrying = GripsUnder( root, HAND_CARRYING );
	return carrying.empty() ? Transform3D() : InSceneFrame( carrying[0], root ).orthonormalized();
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
		warnings.push_back( "The item's own transform is not used by the game. To turn the item in the hand, turn its carrying CbGrip." );
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
	const CollisionShape3D* body = BodyOf( this );
	String problem = ShapeProblem( body );
	if ( problem.is_empty() == false )
	{
		return fail( problem );
	}
	Vector3 half;
	String shapeName = "box";
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
	if ( GripsUnder( this, CbGrip::HAND_CARRYING ).size() > 1 || GripsUnder( this, CbGrip::HAND_OTHER ).size() > 1 )
	{
		return fail( "an item has one CbGrip for each hand at most (the carrying hand's, the other hand's)" );
	}
	// Everything is written in the frame the item is carried in.
	const Transform3D toCarried = CbGrip::CarryFrameUnder( this ).affine_inverse();
	const Transform3D inCarried = ( toCarried * InSceneFrame( body, this ) ).orthonormalized();
	Vector3 center = inCarried.origin;
	Quaternion turn = inCarried.basis.get_rotation_quaternion();
	String text = "# Baked from the item's CbItem (its scene's root). Edit the scene and bake again.\n";
	text += "shape " + shapeName + "\n";
	text += vformat( "half %.4f %.4f %.4f\n", half.x, half.y, half.z );
	text += vformat( "center %.4f %.4f %.4f\n", center.x, center.y, center.z );
	text += vformat( "mass %.3f\n", m_mass );
	if ( turn.is_equal_approx( Quaternion() ) == false )
	{
		text += vformat( "turn %.5f %.5f %.5f %.5f\n", turn.x, turn.y, turn.z, turn.w );
	}
	// Sorted, so the same scene always bakes the same file (the item's hash depends on it).
	Array names = m_properties.keys();
	names.sort();
	for ( int i = 0; i < names.size(); ++i )
	{
		String name = String( names[i] ).strip_edges();
		Variant value = m_properties[names[i]];
		bool number = value.get_type() == Variant::FLOAT || value.get_type() == Variant::INT;
		if ( name.is_empty() || name.contains( " " ) || name.contains( "\t" ) || number == false || std::isfinite( double( value ) ) == false )
		{
			return fail( "property \"" + name + "\" needs a name without spaces and a number" );
		}
		text += "property " + name + " " + String::num( double( value ), 4 ) + "\n";
	}
	// Where the other hand holds it.
	std::vector<CbGrip*> others = GripsUnder( this, CbGrip::HAND_OTHER );
	if ( others.empty() == false )
	{
		Transform3D t = ( toCarried * InSceneFrame( others[0], this ) ).orthonormalized();
		Quaternion q = t.basis.get_rotation_quaternion();
		text += vformat( "grip %.4f %.4f %.4f %.5f %.5f %.5f %.5f %d\n", t.origin.x, t.origin.y, t.origin.z, q.x, q.y, q.z, q.w,
						 others[0]->get_as_animated() ? 2 : ( others[0]->get_align_rotation() ? 1 : 0 ) );
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
	out["text"] = text;
	out["error"] = "";
	return out;
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

} // namespace cb::gd
