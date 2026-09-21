#include "cinderbox_map_nodes.h"

#include "cinderbox_entity_nodes.h"

#include "map.h"
#include "reflect.h"

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace godot;

namespace cb::gd
{

namespace
{

// Roll beyond this is dropped by the format and worth telling the mapper about.
constexpr float kRollEpsilon = 0.001f;

Ref<StandardMaterial3D> PreviewMaterial( const Color& color )
{
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_albedo( color );
	material->set_transparency( BaseMaterial3D::TRANSPARENCY_ALPHA );
	material->set_shading_mode( BaseMaterial3D::SHADING_MODE_UNSHADED );
	return material;
}

// Editor-only preview geometry. It is never given an owner, so it is not saved into the scene and
// never reaches a shipped game.
MeshInstance3D* EnsurePreview( Node3D* parent, MeshInstance3D*& slot )
{
	if ( slot == nullptr )
	{
		slot = memnew( MeshInstance3D );
		parent->add_child( slot );
	}
	return slot;
}

bool InEditor()
{
	return Engine::get_singleton()->is_editor_hint();
}

b3Vec3 ToSim( const Vector3& v )
{
	return b3Vec3{ float( v.x ), float( v.y ), float( v.z ) };
}

struct BakeContext
{
	LevelLayout layout;
	Array warnings;
	int spawnCount = 0;
};

int32_t QuantizeField( const FieldDef& field, const Variant& value, int component )
{
	switch ( field.type )
	{
		case FieldType::Vec3:
		{
			Vector3 v = value;
			float raw = component == 0 ? float( v.x ) : ( component == 1 ? float( v.y ) : float( v.z ) );
			return MapQuantize( std::clamp( raw, field.min, field.max ), kMapPositionScale );
		}
		case FieldType::Bool:
			return bool( value ) ? 1 : 0;
		case FieldType::Int:
		case FieldType::Enum:
			return std::clamp( int32_t( int64_t( value ) ), int32_t( field.min ), int32_t( field.max ) );
		case FieldType::Float:
		default:
			return MapQuantize( std::clamp( float( double( value ) ), field.min, field.max ), kMapPositionScale );
	}
}

// Reads the CbComponent children of a template or entity node into authored values.
std::vector<AuthoredComponent> ReadComponents( Node* node, const String& owner, BakeContext& ctx )
{
	std::vector<AuthoredComponent> components;
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		auto* source = Object::cast_to<CbComponent>( node->get_child( i ) );
		if ( source == nullptr )
		{
			continue;
		}
		const ComponentDef* def = FindComponent( std::string( source->component_name().utf8().get_data() ) );
		if ( def == nullptr )
		{
			ctx.warnings.push_back( String( "skipped a component with nothing selected on " ) + owner );
			continue;
		}

		AuthoredComponent component;
		component.id = def->Id();
		Dictionary values = source->get_values();
		for ( const FieldDef& field : def->fields )
		{
			Variant value = values[String( field.name )];
			AuthoredField authored;
			authored.id = field.Id();
			int count = field.type == FieldType::Vec3 ? 3 : 1;
			for ( int c = 0; c < count; ++c )
			{
				authored.raw[c] = QuantizeField( field, value, c );
			}
			component.fields.push_back( authored );
		}
		components.push_back( std::move( component ) );
	}
	return components;
}

bool SameComponents( const std::vector<AuthoredComponent>& a, const std::vector<AuthoredComponent>& b )
{
	if ( a.size() != b.size() )
	{
		return false;
	}
	for ( size_t i = 0; i < a.size(); ++i )
	{
		if ( a[i].id != b[i].id || a[i].fields.size() != b[i].fields.size() )
		{
			return false;
		}
		for ( size_t f = 0; f < a[i].fields.size(); ++f )
		{
			const AuthoredField& x = a[i].fields[f];
			const AuthoredField& y = b[i].fields[f];
			if ( x.id != y.id || x.raw[0] != y.raw[0] || x.raw[1] != y.raw[1] || x.raw[2] != y.raw[2] )
			{
				return false;
			}
		}
	}
	return true;
}

// Names become resource paths on clients, so they are kept to what the map format accepts.
std::string SafeText( const String& text )
{
	std::string out = std::string( text.utf8().get_data() ).substr( 0, kMapNameLimit );
	for ( char& c : out )
	{
		bool allowed = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
		if ( allowed == false )
		{
			c = '_';
		}
	}
	return out;
}

// Adds a template, reusing an identical one so that a hundred crates do not write a hundred copies.
uint32_t AddTemplate( BakeContext& ctx, EntityTemplate&& candidate )
{
	for ( size_t i = 0; i < ctx.layout.templates.size(); ++i )
	{
		const EntityTemplate& existing = ctx.layout.templates[i];
		if ( existing.visual == candidate.visual && SameComponents( existing.components, candidate.components ) )
		{
			return uint32_t( i );
		}
	}
	ctx.layout.templates.push_back( std::move( candidate ) );
	return uint32_t( ctx.layout.templates.size() - 1 );
}

uint32_t FindTemplateByName( const BakeContext& ctx, const std::string& name )
{
	for ( size_t i = 0; i < ctx.layout.templates.size(); ++i )
	{
		if ( ctx.layout.templates[i].name == name )
		{
			return uint32_t( i );
		}
	}
	return kNoTemplate;
}

// First pass: named templates, so an entity can reference one that appears later in the scene.
void CollectTemplates( Node* node, BakeContext& ctx )
{
	if ( auto* source = Object::cast_to<CbTemplate>( node ) )
	{
		EntityTemplate t;
		// An unnamed template is known by its node name, which is what an author sees in the tree.
		String templateName = source->get_template_name();
		t.name = SafeText( templateName.is_empty() ? String( source->get_name() ) : templateName );
		t.visual = SafeText( source->get_visual() );
		t.components = ReadComponents( source, source->get_name(), ctx );

		if ( FindTemplateByName( ctx, t.name ) != kNoTemplate )
		{
			ctx.warnings.push_back( String( "duplicate template name \"" ) + String( t.name.c_str() ) + "\", the later one is ignored" );
		}
		else
		{
			bool spawnable = source->get_spawnable();
			// Templates are added by name here, never deduplicated: an author named them on purpose.
			ctx.layout.templates.push_back( std::move( t ) );
			uint32_t index = uint32_t( ctx.layout.templates.size() - 1 );
			if ( spawnable )
			{
				if ( ctx.layout.spawnTemplate != kNoTemplate )
				{
					ctx.warnings.push_back( String( "more than one spawnable template, using the first" ) );
				}
				else
				{
					ctx.layout.spawnTemplate = index;
				}
			}
		}
	}

	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		CollectTemplates( node->get_child( i ), ctx );
	}
}

// Transforms are accumulated down the tree instead of read with get_global_transform(), so baking
// works on a scene that is not inside a tree (headless) and is always relative to the map root.
void BakeNode( Node* node, const Transform3D& parent, BakeContext& ctx )
{
	Transform3D here = parent;
	if ( auto* spatial = Object::cast_to<Node3D>( node ) )
	{
		here = parent * spatial->get_transform();
	}

	if ( auto* box = Object::cast_to<CbStatic>( node ) )
	{
		const Transform3D& gt = here;
		Vector3 euler = gt.basis.get_euler( EULER_ORDER_YXZ );
		Vector3 scale = gt.basis.get_scale();
		Vector3 size = box->get_size() * scale;

		if ( std::fabs( float( euler.z ) ) > kRollEpsilon )
		{
			ctx.warnings.push_back( String( "roll ignored on " ) + box->get_name() );
		}
		if ( size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f )
		{
			ctx.warnings.push_back( String( "skipped " ) + box->get_name() + ": size must be positive" );
		}
		else
		{
			LevelBox out;
			out.center = ToSim( gt.origin );
			out.halfExtents = ToSim( size * 0.5f );
			out.pitch = float( euler.x );
			out.yaw = float( euler.y );
			ctx.layout.statics.push_back( out );
		}
	}
	else if ( auto* prop = Object::cast_to<CbProp>( node ) )
	{
		const Transform3D& gt = here;
		Vector3 scale = gt.basis.get_scale();
		LevelProp out;
		out.position = ToSim( gt.origin );
		if ( prop->get_shape() == CbProp::SHAPE_SPHERE )
		{
			out.kind = ShapeKind::Sphere;
			out.halfExtents = b3Vec3{ prop->get_radius() * float( scale.x ), 0.0f, 0.0f };
		}
		else
		{
			out.kind = ShapeKind::Box;
			out.halfExtents = ToSim( prop->get_size() * scale * 0.5f );
		}
		ctx.layout.props.push_back( out );
	}
	else if ( auto* entity = Object::cast_to<CbEntity>( node ) )
	{
		std::string named = SafeText( entity->get_template_name() );
		uint32_t index = kNoTemplate;
		if ( named.empty() == false )
		{
			index = FindTemplateByName( ctx, named );
			if ( index == kNoTemplate )
			{
				ctx.warnings.push_back( String( "skipped " ) + entity->get_name() + ": no CbTemplate named \"" +
										entity->get_template_name() + "\"" );
			}
		}
		else
		{
			// No name: the node's own components define a template just for it.
			EntityTemplate inlineTemplate;
			inlineTemplate.name = SafeText( entity->get_name() );
			inlineTemplate.visual = SafeText( entity->get_visual() );
			inlineTemplate.components = ReadComponents( entity, entity->get_name(), ctx );
			if ( inlineTemplate.components.empty() )
			{
				ctx.warnings.push_back( String( "skipped " ) + entity->get_name() +
										": it has no CbComponent children and names no template" );
			}
			else
			{
				index = AddTemplate( ctx, std::move( inlineTemplate ) );
			}
		}

		if ( index != kNoTemplate )
		{
			Vector3 euler = here.basis.get_euler( EULER_ORDER_YXZ );
			LevelInstance instance;
			instance.templateIndex = index;
			instance.position = ToSim( here.origin );
			instance.pitch = float( euler.x );
			instance.yaw = float( euler.y );
			ctx.layout.instances.push_back( instance );
		}
	}
	else if ( auto* spawn = Object::cast_to<CbSpawn>( node ) )
	{
		ctx.spawnCount += 1;
		if ( ctx.spawnCount == 1 )
		{
			ctx.layout.spawnCenter = ToSim( here.origin );
			ctx.layout.spawnRadius = spawn->get_radius();
		}
		else
		{
			ctx.warnings.push_back( String( "extra spawn point ignored: " ) + spawn->get_name() );
		}
	}

	// Scene-tree order decides entity creation order, and therefore the state hashes.
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		BakeNode( node->get_child( i ), here, ctx );
	}
}

} // namespace

// --- CbStatic -----------------------------------------------------------------------------------

void CbStatic::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_size", "size" ), &CbStatic::set_size );
	ClassDB::bind_method( D_METHOD( "get_size" ), &CbStatic::get_size );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR3, "size", PROPERTY_HINT_NONE ), "set_size", "get_size" );
}

void CbStatic::set_size( const Vector3& size )
{
	m_size = size;
	UpdatePreview();
	update_gizmos();
}

void CbStatic::_notification( int what )
{
	if ( what == NOTIFICATION_ENTER_TREE || what == NOTIFICATION_READY )
	{
		UpdatePreview();
	}
}

void CbStatic::UpdatePreview()
{
	if ( InEditor() == false || is_inside_tree() == false )
	{
		return;
	}
	MeshInstance3D* preview = EnsurePreview( this, m_preview );
	Ref<BoxMesh> mesh;
	mesh.instantiate();
	mesh->set_size( m_size );
	mesh->set_material( PreviewMaterial( Color( 0.45f, 0.75f, 1.0f, 0.35f ) ) );
	preview->set_mesh( mesh );
}

// --- CbProp -------------------------------------------------------------------------------------

void CbProp::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_shape", "shape" ), &CbProp::set_shape );
	ClassDB::bind_method( D_METHOD( "get_shape" ), &CbProp::get_shape );
	ClassDB::bind_method( D_METHOD( "set_size", "size" ), &CbProp::set_size );
	ClassDB::bind_method( D_METHOD( "get_size" ), &CbProp::get_size );
	ClassDB::bind_method( D_METHOD( "set_radius", "radius" ), &CbProp::set_radius );
	ClassDB::bind_method( D_METHOD( "get_radius" ), &CbProp::get_radius );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shape", PROPERTY_HINT_ENUM, "Box,Sphere" ), "set_shape", "get_shape" );
	ADD_PROPERTY( PropertyInfo( Variant::VECTOR3, "size" ), "set_size", "get_size" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "radius" ), "set_radius", "get_radius" );
}

void CbProp::set_shape( int shape )
{
	m_shape = shape == SHAPE_SPHERE ? SHAPE_SPHERE : SHAPE_BOX;
	UpdatePreview();
}

void CbProp::set_size( const Vector3& size )
{
	m_size = size;
	UpdatePreview();
}

void CbProp::set_radius( float radius )
{
	m_radius = radius;
	UpdatePreview();
}

void CbProp::_notification( int what )
{
	if ( what == NOTIFICATION_ENTER_TREE || what == NOTIFICATION_READY )
	{
		UpdatePreview();
	}
}

void CbProp::UpdatePreview()
{
	if ( InEditor() == false || is_inside_tree() == false )
	{
		return;
	}
	MeshInstance3D* preview = EnsurePreview( this, m_preview );
	Ref<StandardMaterial3D> material = PreviewMaterial( Color( 1.0f, 0.8f, 0.35f, 0.4f ) );
	if ( m_shape == SHAPE_SPHERE )
	{
		Ref<SphereMesh> mesh;
		mesh.instantiate();
		mesh->set_radius( m_radius );
		mesh->set_height( m_radius * 2.0f );
		mesh->set_material( material );
		preview->set_mesh( mesh );
	}
	else
	{
		Ref<BoxMesh> mesh;
		mesh.instantiate();
		mesh->set_size( m_size );
		mesh->set_material( material );
		preview->set_mesh( mesh );
	}
}

// --- CbSpawn ------------------------------------------------------------------------------------

void CbSpawn::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_radius", "radius" ), &CbSpawn::set_radius );
	ClassDB::bind_method( D_METHOD( "get_radius" ), &CbSpawn::get_radius );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "radius" ), "set_radius", "get_radius" );
}

void CbSpawn::set_radius( float radius )
{
	m_radius = radius;
	UpdatePreview();
}

void CbSpawn::_notification( int what )
{
	if ( what == NOTIFICATION_ENTER_TREE || what == NOTIFICATION_READY )
	{
		UpdatePreview();
	}
}

void CbSpawn::UpdatePreview()
{
	if ( InEditor() == false || is_inside_tree() == false )
	{
		return;
	}
	MeshInstance3D* preview = EnsurePreview( this, m_preview );
	Ref<SphereMesh> mesh;
	mesh.instantiate();
	mesh->set_radius( m_radius );
	mesh->set_height( m_radius * 2.0f );
	mesh->set_material( PreviewMaterial( Color( 0.4f, 1.0f, 0.5f, 0.25f ) ) );
	preview->set_mesh( mesh );
}

// --- CinderboxMapBaker --------------------------------------------------------------------------

void CinderboxMapBaker::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "bake", "root", "path" ), &CinderboxMapBaker::bake );
}

Dictionary CinderboxMapBaker::bake( Node* root, const String& path )
{
	Dictionary result;
	result["ok"] = false;
	result["path"] = path;

	if ( root == nullptr )
	{
		result["error"] = "no scene to bake";
		return result;
	}

	BakeContext ctx;
	// The file is named after the map, and so is the scene a client draws for it. Only characters
	// the format accepts survive, because the name ends up in a resource path.
	std::string name = std::string( path.get_file().get_basename().utf8().get_data() ).substr( 0, kMapNameLimit );
	for ( char& c : name )
	{
		bool allowed = ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
		if ( allowed == false )
		{
			c = '_';
		}
	}
	ctx.layout.name = name;
	ctx.layout.spawnCenter = b3Vec3{ 0.0f, 1.5f, 0.0f };
	ctx.layout.spawnRadius = 4.0f;
	CollectTemplates( root, ctx );
	BakeNode( root, Transform3D(), ctx );

	result["statics"] = int( ctx.layout.statics.size() );
	result["props"] = int( ctx.layout.props.size() );
	result["templates"] = int( ctx.layout.templates.size() );
	result["instances"] = int( ctx.layout.instances.size() );
	result["spawn_found"] = ctx.spawnCount > 0;
	result["warnings"] = ctx.warnings;

	if ( ctx.layout.statics.empty() && ctx.layout.instances.empty() )
	{
		result["error"] = "the scene has no CbStatic or CbEntity nodes";
		return result;
	}
	if ( ctx.spawnCount == 0 )
	{
		ctx.warnings.push_back( "no CbSpawn node, players will appear at the map origin" );
		result["warnings"] = ctx.warnings;
	}

	// Rounding onto the storage grid here means the editor's values and the baked file agree.
	QuantizeLayout( ctx.layout );
	std::vector<uint8_t> bytes;
	SerializeMap( ctx.layout, bytes );

	String globalPath = ProjectSettings::get_singleton()->globalize_path( path );
	std::string error;
	if ( WriteMapFile( std::string( globalPath.utf8().get_data() ), bytes, error ) == false )
	{
		result["error"] = String( error.c_str() );
		return result;
	}

	result["ok"] = true;
	result["error"] = "";
	result["name"] = String( ctx.layout.name.c_str() );
	result["bytes"] = int( bytes.size() );
	result["hash"] = String::num_uint64( MapHash( bytes.data(), bytes.size() ), 16 );
	return result;
}

} // namespace cb::gd
