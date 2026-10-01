#include "cinderbox_item_look.h"

#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

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
	ClassDB::bind_method( D_METHOD( "set_properties", "value" ), &CbItemBody::set_properties );
	ClassDB::bind_method( D_METHOD( "get_properties" ), &CbItemBody::get_properties );
	ADD_PROPERTY( PropertyInfo( Variant::DICTIONARY, "properties", PROPERTY_HINT_DICTIONARY_TYPE, "String;float" ), "set_properties",
				  "get_properties" );
	ClassDB::bind_method( D_METHOD( "bake" ), &CbItemBody::bake );
	ClassDB::bind_method( D_METHOD( "bake_to_project" ), &CbItemBody::bake_to_project );
	ClassDB::bind_method( D_METHOD( "get_bake_button" ), &CbItemBody::get_bake_button );
	ADD_PROPERTY( PropertyInfo( Variant::CALLABLE, "bake_button", PROPERTY_HINT_TOOL_BUTTON, "Bake item body,Save", PROPERTY_USAGE_EDITOR ), "",
				  "get_bake_button" );
}

Callable CbItemBody::get_bake_button()
{
	return Callable( this, "bake_to_project" );
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
			out["text"] = "";
			out["error"] = "property \"" + name + "\" needs a name without spaces and a number";
			return out;
		}
		text += "property " + name + " " + String::num( double( value ), 4 ) + "\n";
	}
	out["text"] = text;
	out["error"] = "";
	return out;
}

namespace
{

void CollectLooks( Node* node, std::vector<CbItemLook*>& out )
{
	if ( auto* look = Object::cast_to<CbItemLook>( node ) )
	{
		out.push_back( look );
	}
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		CollectLooks( node->get_child( i ), out );
	}
}

} // namespace

void CbItemBody::bake_to_project()
{
	Node* owner = get_owner() != nullptr ? get_owner() : this;
	String scene = owner->get_scene_file_path();
	if ( scene.is_empty() )
	{
		UtilityFunctions::push_error( "Item bake: save the item's scene first." );
		return;
	}
	Dictionary baked = bake();
	if ( String( baked["text"] ).is_empty() )
	{
		UtilityFunctions::push_error( "Item bake failed: ", baked["error"] );
		return;
	}
	// Which kinds are drawn with this scene: the CbItemLook nodes in the project's world reactions.
	PackedStringArray kinds;
	Ref<DirAccess> dir = DirAccess::open( "res://vfx" );
	PackedStringArray files = dir.is_valid() ? dir->get_files() : PackedStringArray();
	for ( const String& file : files )
	{
		if ( file.begins_with( "reactions" ) == false || file.ends_with( ".tscn" ) == false )
		{
			continue;
		}
		Ref<PackedScene> packed = ResourceLoader::get_singleton()->load( "res://vfx/" + file );
		Node* root = packed.is_valid() ? packed->instantiate() : nullptr;
		if ( root == nullptr )
		{
			continue;
		}
		std::vector<CbItemLook*> looks;
		CollectLooks( root, looks );
		for ( CbItemLook* look : looks )
		{
			if ( look->get_scene() == scene && look->get_kind().is_empty() == false && kinds.has( look->get_kind() ) == false )
			{
				kinds.push_back( look->get_kind() );
			}
		}
		memdelete( root );
	}
	if ( kinds.is_empty() )
	{
		UtilityFunctions::push_error( "Item bake: no CbItemLook in res://vfx/reactions*.tscn draws ", scene,
									  ". Add one (its kind names the file) and bake again." );
		return;
	}
	DirAccess::make_dir_recursive_absolute( "res://items" );
	for ( const String& kind : kinds )
	{
		String path = "res://items/" + kind + ".cfg";
		Ref<FileAccess> out = FileAccess::open( path, FileAccess::WRITE );
		if ( out.is_null() )
		{
			UtilityFunctions::push_error( "Item bake: cannot write ", path );
			continue;
		}
		out->store_string( baked["text"] );
		out->close();
		UtilityFunctions::print( "Baked item body ", kind, " into ", path, ". Publish the mod for servers to get it." );
	}
}

} // namespace cb::gd
