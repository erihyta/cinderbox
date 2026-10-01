#include "cinderbox_item_look.h"

#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::gd
{

void CbItemLook::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_kind", "value" ), &CbItemLook::set_kind );
	ClassDB::bind_method( D_METHOD( "get_kind" ), &CbItemLook::get_kind );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "kind", PROPERTY_HINT_PLACEHOLDER_TEXT, "melee.bat" ), "set_kind", "get_kind" );
	ClassDB::bind_method( D_METHOD( "set_display_name", "value" ), &CbItemLook::set_display_name );
	ClassDB::bind_method( D_METHOD( "get_display_name" ), &CbItemLook::get_display_name );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "display_name", PROPERTY_HINT_PLACEHOLDER_TEXT, "Bat" ), "set_display_name",
				  "get_display_name" );
	ClassDB::bind_method( D_METHOD( "set_scene", "value" ), &CbItemLook::set_scene );
	ClassDB::bind_method( D_METHOD( "get_scene" ), &CbItemLook::get_scene );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "scene", PROPERTY_HINT_FILE, "*.tscn,*.scn" ), "set_scene", "get_scene" );
}

void CbItemBody::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_mass", "value" ), &CbItemBody::set_mass );
	ClassDB::bind_method( D_METHOD( "get_mass" ), &CbItemBody::get_mass );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "mass", PROPERTY_HINT_RANGE, "0.01,1000,0.01,suffix:kg" ), "set_mass", "get_mass" );
	ClassDB::bind_method( D_METHOD( "bake" ), &CbItemBody::bake );
}

namespace
{

// Where the body is in the item's frame (the scene's root): through every Node3D above it.
Transform3D InItemFrame( const Node3D* node )
{
	Transform3D t = node->get_transform();
	for ( Node* parent = node->get_parent(); parent != nullptr && parent->get_parent() != nullptr; parent = parent->get_parent() )
	{
		if ( auto* spatial = Object::cast_to<Node3D>( parent ) )
		{
			t = spatial->get_transform() * t;
		}
	}
	return t;
}

String ShapeProblem( const CbItemBody* body )
{
	Ref<Shape3D> shape = body->get_shape();
	if ( shape.is_null() )
	{
		return "Give it a BoxShape3D or a SphereShape3D.";
	}
	if ( Object::cast_to<BoxShape3D>( shape.ptr() ) == nullptr && Object::cast_to<SphereShape3D>( shape.ptr() ) == nullptr )
	{
		return "An item's body is a box or a sphere (" + shape->get_class() + " cannot be baked).";
	}
	Transform3D t = InItemFrame( body );
	if ( t.basis.is_equal_approx( Basis() ) == false )
	{
		return "Keep it unrotated and unscaled: the box is aligned with the item (set the shape's size instead).";
	}
	return String();
}

} // namespace

PackedStringArray CbItemBody::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	String problem = ShapeProblem( this );
	if ( problem.is_empty() == false )
	{
		warnings.push_back( problem );
	}
	return warnings;
}

Dictionary CbItemBody::bake() const
{
	Dictionary out;
	String problem = ShapeProblem( this );
	if ( problem.is_empty() == false )
	{
		out["text"] = "";
		out["error"] = problem;
		return out;
	}
	Vector3 half;
	String kind = "box";
	if ( auto* box = Object::cast_to<BoxShape3D>( get_shape().ptr() ) )
	{
		half = box->get_size() * 0.5f;
	}
	else
	{
		float radius = Object::cast_to<SphereShape3D>( get_shape().ptr() )->get_radius();
		half = Vector3( radius, radius, radius );
		kind = "sphere";
	}
	Vector3 center = InItemFrame( this ).origin;
	String text = "# Baked from the item's CbItemBody (bake_items.gd): its body when it lies in the world.\n";
	text += "shape " + kind + "\n";
	text += vformat( "half %.4f %.4f %.4f\n", half.x, half.y, half.z );
	text += vformat( "center %.4f %.4f %.4f\n", center.x, center.y, center.z );
	text += vformat( "mass %.3f\n", m_mass );
	out["text"] = text;
	out["error"] = "";
	return out;
}

} // namespace cb::gd
