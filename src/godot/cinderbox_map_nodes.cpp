#include "cinderbox_map_nodes.h"

#include "cinderbox_entity_nodes.h"

#include "map.h"
#include "reflect.h"

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/capsule_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/cylinder_shape3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace godot;

namespace cb::gd
{

namespace
{

// Scales this close count as the same (a sphere scaled unevenly is not a sphere).
constexpr float kScaleEpsilon = 0.001f;

// A cylinder's hull: this many points around each of its two rims.
constexpr int kCylinderSides = 16;

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
		// Internal: not a child the bake, a look's copy or the scene tree dock sees.
		parent->add_child( slot, false, Node::INTERNAL_MODE_BACK );
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
	// Each look node's number (MapLookNodes).
	std::unordered_map<const Node*, uint32_t> looks;
};

uint32_t LookOf( const BakeContext& ctx, const Node* node )
{
	auto it = ctx.looks.find( node );
	return it != ctx.looks.end() ? it->second : kNoLook;
}

// Whether a body node draws something of its own: a mesh, a sprite, a light, particles under it.
bool HasLook( Node* node )
{
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		Node* child = node->get_child( i );
		if ( Object::cast_to<VisualInstance3D>( child ) != nullptr || HasLook( child ) )
		{
			return true;
		}
	}
	return false;
}

// What a body's own frame leaves of where it is: its scale (its shapes take it, the body has none).
Transform3D ScaleOf( const Transform3D& here )
{
	return Transform3D( Basis::from_scale( here.basis.get_scale().abs() ), Vector3() );
}

// A component as an author would have attached it, from values by field name.
AuthoredComponent MakeComponent( const char* name, std::initializer_list<std::pair<const char*, double>> values )
{
	AuthoredComponent component;
	const ComponentDef* def = FindComponent( name );
	if ( def == nullptr )
	{
		return component;
	}
	component.id = def->Id();
	for ( const auto& [field, value] : values )
	{
		for ( const FieldDef& known : def->fields )
		{
			if ( std::string( known.name ) == field )
			{
				AuthoredField authored;
				authored.id = known.Id();
				authored.raw[0] = MapQuantize( std::clamp( float( value ), known.min, known.max ), kMapPositionScale );
				component.fields.push_back( authored );
			}
		}
	}
	return component;
}

// A set of a body's shapes; an identical one already in the map is shared.
uint32_t AddShapeSet( BakeContext& ctx, std::vector<LevelBox>&& set )
{
	LevelLayout snapped;
	snapped.shapeSets.push_back( set );
	QuantizeLayout( snapped );
	auto same = []( const LevelBox& a, const LevelBox& b ) {
		return a.kind == b.kind && a.hull == b.hull && a.center.x == b.center.x && a.center.y == b.center.y && a.center.z == b.center.z &&
			   a.halfExtents.x == b.halfExtents.x && a.halfExtents.y == b.halfExtents.y && a.halfExtents.z == b.halfExtents.z &&
			   a.pitch == b.pitch && a.yaw == b.yaw && a.roll == b.roll;
	};
	const std::vector<LevelBox>& mine = snapped.shapeSets[0];
	for ( size_t i = 0; i < ctx.layout.shapeSets.size(); ++i )
	{
		const std::vector<LevelBox>& other = ctx.layout.shapeSets[i];
		bool alike = other.size() == mine.size();
		for ( size_t k = 0; alike && k < mine.size(); ++k )
		{
			alike = same( other[k], mine[k] );
		}
		if ( alike )
		{
			return uint32_t( i );
		}
	}
	ctx.layout.shapeSets.push_back( mine );
	return uint32_t( ctx.layout.shapeSets.size() - 1 );
}

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
		if ( existing.visual == candidate.visual && existing.shapes == candidate.shapes && existing.look == candidate.look &&
			 SameComponents( existing.components, candidate.components ) )
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

void BodyOfNode( Node3D* node, const Transform3D& here, EntityTemplate& t, BakeContext& ctx );

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
		// (Where a template stands does not matter, only its own scale.)
		BodyOfNode( source, source->get_transform(), t, ctx );

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

// A piece of level geometry where `at` puts it: turned (yaw, pitch, roll as Godot's YXZ order),
// the scale left to the caller.
LevelBox PieceAt( const Transform3D& at, ShapeKind kind, const Vector3& halfExtents )
{
	Vector3 euler = at.basis.orthonormalized().get_euler( EULER_ORDER_YXZ );
	LevelBox out;
	out.kind = kind;
	out.center = ToSim( at.origin );
	out.halfExtents = ToSim( halfExtents );
	out.pitch = float( euler.x );
	out.yaw = float( euler.y );
	out.roll = float( euler.z );
	return out;
}

// A hull's points as the map stores them: scaled, on the grid; shared with an identical hull
// already in the map. Returns false (and says why) when Box3D would make no hull of them.
bool AddHull( std::vector<b3Vec3> points, const String& name, BakeContext& ctx, LevelBox& piece )
{
	if ( points.size() < 4 || points.size() > kMapHullPointLimit )
	{
		ctx.warnings.push_back( String( "skipped " ) + name + ": a hull needs 4 to " + String::num_int64( kMapHullPointLimit ) +
								" points, it has " + String::num_int64( int64_t( points.size() ) ) );
		return false;
	}
	b3Vec3 bounds = { 0.0f, 0.0f, 0.0f };
	for ( b3Vec3& point : points )
	{
		point.x = MapDequantize( MapQuantize( point.x, kMapPositionScale ), kMapPositionScale );
		point.y = MapDequantize( MapQuantize( point.y, kMapPositionScale ), kMapPositionScale );
		point.z = MapDequantize( MapQuantize( point.z, kMapPositionScale ), kMapPositionScale );
		bounds = { std::max( bounds.x, std::fabs( point.x ) ), std::max( bounds.y, std::fabs( point.y ) ),
				   std::max( bounds.z, std::fabs( point.z ) ) };
	}
	if ( MapHullBuilds( points ) == false )
	{
		ctx.warnings.push_back( String( "skipped " ) + name + ": its points make no solid (they lie in a plane)" );
		return false;
	}
	piece.kind = ShapeKind::Hull;
	piece.halfExtents = bounds;
	auto same = []( const std::vector<b3Vec3>& a, const std::vector<b3Vec3>& b ) {
		if ( a.size() != b.size() )
		{
			return false;
		}
		for ( size_t i = 0; i < a.size(); ++i )
		{
			if ( a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z )
			{
				return false;
			}
		}
		return true;
	};
	for ( size_t i = 0; i < ctx.layout.hulls.size(); ++i )
	{
		if ( same( ctx.layout.hulls[i], points ) )
		{
			piece.hull = uint32_t( i );
			return true;
		}
	}
	piece.hull = uint32_t( ctx.layout.hulls.size() );
	ctx.layout.hulls.push_back( std::move( points ) );
	return true;
}

bool Uneven( float a, float b )
{
	return std::fabs( a - b ) > kScaleEpsilon * std::max( std::fabs( a ), std::fabs( b ) );
}

// A body node's CollisionShape3D children, each a piece where `frame` puts it (a CbStatic's: in
// the map; a body's: in its own frame, the scale above it in), into `out`. Returns how many there
// were (used or not): with none, a node is its own box or ball.
int BakeShapes( Node* body, const Transform3D& frame, std::vector<LevelBox>& out, BakeContext& ctx )
{
	int found = 0;
	for ( int i = 0; i < body->get_child_count(); ++i )
	{
		auto* node = Object::cast_to<CollisionShape3D>( body->get_child( i ) );
		if ( node == nullptr )
		{
			continue;
		}
		found += 1;
		String name = body->get_name() + String( "/" ) + node->get_name();
		BakedShape baked;
		String why = BakeShape( node, frame * node->get_transform(), name, baked, ctx.warnings );
		if ( why.is_empty() == false )
		{
			ctx.warnings.push_back( String( "skipped " ) + name + ": " + why );
			continue;
		}
		LevelBox piece = PieceAt( baked.at, baked.kind, baked.half );
		if ( baked.kind == ShapeKind::Hull )
		{
			std::vector<b3Vec3> points;
			for ( const Vector3& point : baked.points )
			{
				points.push_back( ToSim( point ) );
			}
			if ( AddHull( std::move( points ), name, ctx, piece ) == false )
			{
				continue;
			}
		}
		out.push_back( piece );
	}
	return found;
}

// A template's shapes from its node's CollisionShape3D children, and its look: the node itself
// when it has no visual and draws something of its own.
void BodyOfNode( Node3D* node, const Transform3D& here, EntityTemplate& t, BakeContext& ctx )
{
	std::vector<LevelBox> set;
	if ( BakeShapes( node, ScaleOf( here ), set, ctx ) > 0 )
	{
		if ( set.empty() )
		{
			ctx.warnings.push_back( node->get_name() + String( ": none of its shapes could be used; its Shape component is" ) );
		}
		else
		{
			t.shapes = AddShapeSet( ctx, std::move( set ) );
		}
	}
	if ( t.visual.empty() && HasLook( node ) )
	{
		t.look = LookOf( ctx, node );
	}
}

LevelInstance InstanceAt( uint32_t index, const Transform3D& here )
{
	Vector3 euler = here.basis.orthonormalized().get_euler( EULER_ORDER_YXZ );
	LevelInstance instance;
	instance.templateIndex = index;
	instance.position = ToSim( here.origin );
	instance.pitch = float( euler.x );
	instance.yaw = float( euler.y );
	instance.roll = float( euler.z );
	return instance;
}

// A CbProp with CollisionShape3D children, or with a look of its own (a mesh under it), is a body
// like a template's: its shapes (or its box or ball) on one body, drawn by its own node, turned as
// it stands. Returns false for a plain one (the map's prop list, as always).
bool BakeBodyProp( CbProp* prop, const Transform3D& here, BakeContext& ctx )
{
	std::vector<LevelBox> set;
	const int found = BakeShapes( prop, ScaleOf( here ), set, ctx );
	const bool look = HasLook( prop );
	if ( found == 0 && look == false )
	{
		return false;
	}
	if ( found == 0 )
	{
		Vector3 scale = here.basis.get_scale().abs();
		set.push_back( prop->get_shape() == CbProp::SHAPE_SPHERE
						   ? PieceAt( Transform3D(), ShapeKind::Sphere, Vector3( prop->get_radius() * float( scale.x ), 0.0f, 0.0f ) )
						   : PieceAt( Transform3D(), ShapeKind::Box, prop->get_size() * scale * 0.5f ) );
	}
	if ( set.empty() )
	{
		ctx.warnings.push_back( String( "skipped " ) + prop->get_name() + ": none of its shapes could be used" );
		return true;
	}
	EntityTemplate t;
	t.name = SafeText( prop->get_name() );
	t.components.push_back( MakeComponent( "Material", { { "density", prop->get_density() },
														  { "friction", prop->get_friction() },
														  { "restitution", prop->get_bounce() },
														  { "mass", prop->get_mass() } } ) );
	t.components.push_back( MakeComponent( "Prop", { { "lifetime_seconds", 0.0 } } ) );
	t.shapes = AddShapeSet( ctx, std::move( set ) );
	t.look = look ? LookOf( ctx, prop ) : kNoLook;
	ctx.layout.instances.push_back( InstanceAt( AddTemplate( ctx, std::move( t ) ), here ) );
	return true;
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
		// Its CollisionShape3D children, or else its own box.
		if ( BakeShapes( box, here, ctx.layout.statics, ctx ) == 0 )
		{
			Vector3 size = box->get_size() * here.basis.get_scale().abs();
			if ( size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f )
			{
				ctx.warnings.push_back( String( "skipped " ) + box->get_name() + ": size must be positive" );
			}
			else
			{
				ctx.layout.statics.push_back( PieceAt( here, ShapeKind::Box, size * 0.5f ) );
			}
		}
	}
	else if ( auto* prop = Object::cast_to<CbProp>( node ); prop != nullptr && BakeBodyProp( prop, here, ctx ) )
	{
		// (A body of its own shapes, or drawn by its own node: a template and its placement.)
	}
	else if ( prop != nullptr )
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
		out.mass = float( prop->get_mass() );
		out.density = float( prop->get_density() );
		out.friction = float( prop->get_friction() );
		out.restitution = float( prop->get_bounce() );
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
			BodyOfNode( entity, here, inlineTemplate, ctx );
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
			ctx.layout.instances.push_back( InstanceAt( index, here ) );
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

void MapLookNodes( Node* node, std::vector<Node3D*>& out )
{
	if ( Object::cast_to<CbProp>( node ) != nullptr || Object::cast_to<CbTemplate>( node ) != nullptr ||
		 Object::cast_to<CbEntity>( node ) != nullptr )
	{
		out.push_back( Object::cast_to<Node3D>( node ) );
	}
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		MapLookNodes( node->get_child( i ), out );
	}
}

String BakeShape( const CollisionShape3D* node, const Transform3D& at, const String& name, BakedShape& out, Array& warnings )
{
	Ref<Shape3D> shape = node->get_shape();
	if ( shape.is_null() )
	{
		return "it has no shape";
	}
	if ( node->is_disabled() )
	{
		return "it is disabled";
	}
	const Vector3 scale = at.basis.get_scale().abs();
	out = BakedShape();
	out.at = Transform3D( at.basis.orthonormalized(), at.origin );
	if ( auto* box = Object::cast_to<BoxShape3D>( shape.ptr() ) )
	{
		out.kind = ShapeKind::Box;
		out.half = box->get_size() * scale * 0.5f;
	}
	else if ( auto* sphere = Object::cast_to<SphereShape3D>( shape.ptr() ) )
	{
		if ( Uneven( float( scale.x ), float( scale.y ) ) || Uneven( float( scale.x ), float( scale.z ) ) )
		{
			warnings.push_back( name + String( ": a sphere scaled unevenly stays round (its largest scale)" ) );
		}
		out.kind = ShapeKind::Sphere;
		out.half = Vector3( float( sphere->get_radius() ) * float( std::max( { scale.x, scale.y, scale.z } ) ), 0.0f, 0.0f );
	}
	else if ( auto* capsule = Object::cast_to<CapsuleShape3D>( shape.ptr() ) )
	{
		if ( Uneven( float( scale.x ), float( scale.z ) ) )
		{
			warnings.push_back( name + String( ": a capsule scaled unevenly across stays round (its larger scale)" ) );
		}
		float radius = float( capsule->get_radius() ) * float( std::max( scale.x, scale.z ) );
		float height = float( capsule->get_height() ) * float( scale.y );
		// Box3D's capsule is the distance between its two sphere centres, which cannot go negative.
		out.kind = ShapeKind::Capsule;
		out.half = Vector3( radius, std::max( 0.5f * height - radius, 0.0f ), 0.0f );
	}
	else if ( auto* cylinder = Object::cast_to<CylinderShape3D>( shape.ptr() ) )
	{
		// Two rims of points: a hull, so any scale is kept (an oval drum stays oval).
		out.kind = ShapeKind::Hull;
		float radius = float( cylinder->get_radius() );
		float half = 0.5f * float( cylinder->get_height() );
		for ( int side = 0; side < kCylinderSides; ++side )
		{
			float angle = 6.2831853f * float( side ) / float( kCylinderSides );
			for ( float y : { -half, half } )
			{
				out.points.push_back( Vector3( radius * std::cos( angle ), y, radius * std::sin( angle ) ) * scale );
			}
		}
	}
	else if ( auto* convex = Object::cast_to<ConvexPolygonShape3D>( shape.ptr() ) )
	{
		out.kind = ShapeKind::Hull;
		for ( const Vector3& point : convex->get_points() )
		{
			out.points.push_back( point * scale );
		}
	}
	else
	{
		return String( "a " ) + shape->get_class() + " is not a body's shape yet (box, sphere, capsule, cylinder and convex polygon are)";
	}
	bool boxy = out.kind == ShapeKind::Box;
	if ( out.kind != ShapeKind::Hull && ( ( boxy && ( out.half.x <= 0.0f || out.half.y <= 0.0f || out.half.z <= 0.0f ) ) || out.half.x <= 0.0f ) )
	{
		return "size must be positive";
	}
	return String();
}

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
	if ( what == NOTIFICATION_ENTER_TREE || what == NOTIFICATION_READY || what == NOTIFICATION_CHILD_ORDER_CHANGED )
	{
		UpdatePreview();
	}
}

PackedStringArray CbStatic::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	for ( int i = 0; i < get_child_count(); ++i )
	{
		if ( Object::cast_to<CollisionShape3D>( get_child( i ) ) != nullptr )
		{
			warnings.push_back( "The CollisionShape3D children are its shape; `size` is not used. (Their own warning about a "
								"missing physics body can be ignored: Cinderbox's physics reads them.)" );
			break;
		}
	}
	return warnings;
}

void CbStatic::UpdatePreview()
{
	if ( InEditor() == false || is_inside_tree() == false )
	{
		return;
	}
	MeshInstance3D* preview = EnsurePreview( this, m_preview );
	// With CollisionShape3D children those are its shape, and Godot draws them itself.
	bool shaped = false;
	for ( int i = 0; i < get_child_count(); ++i )
	{
		shaped |= Object::cast_to<CollisionShape3D>( get_child( i ) ) != nullptr;
	}
	preview->set_visible( shaped == false );
	update_configuration_warnings();
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
	std::vector<Node3D*> looks;
	MapLookNodes( root, looks );
	for ( size_t i = 0; i < looks.size(); ++i )
	{
		ctx.looks[looks[i]] = uint32_t( i );
	}
	CollectTemplates( root, ctx );
	BakeNode( root, Transform3D(), ctx );

	result["statics"] = int( ctx.layout.statics.size() );
	result["hulls"] = int( ctx.layout.hulls.size() );
	// Each template's body as stored: its shapes (a count; 0: its Shape component) and its look.
	Array bodies;
	for ( const EntityTemplate& t : ctx.layout.templates )
	{
		Dictionary body;
		body["name"] = String( t.name.c_str() );
		body["shapes"] = t.shapes < ctx.layout.shapeSets.size() ? int( ctx.layout.shapeSets[t.shapes].size() ) : 0;
		body["look"] = t.look == kNoLook ? -1 : int( t.look );
		body["visual"] = String( TemplateLookName( t ).c_str() );
		bodies.push_back( body );
	}
	result["bodies"] = bodies;
	// Each static piece as it will be stored (after rounding onto the map's grid, below).
	QuantizeLayout( ctx.layout );
	Array pieces;
	for ( const LevelBox& box : ctx.layout.statics )
	{
		Dictionary piece;
		static const char* const kKinds[] = { "box", "sphere", "capsule", "hull" };
		piece["kind"] = kKinds[int( box.kind )];
		if ( box.kind == ShapeKind::Hull )
		{
			PackedVector3Array points;
			for ( const b3Vec3& point : ctx.layout.hulls[box.hull] )
			{
				points.push_back( Vector3( point.x, point.y, point.z ) );
			}
			piece["points"] = points;
			piece["hull"] = int( box.hull );
		}
		piece["center"] = Vector3( box.center.x, box.center.y, box.center.z );
		piece["half_extents"] = Vector3( box.halfExtents.x, box.halfExtents.y, box.halfExtents.z );
		piece["rotation"] = Vector3( box.pitch, box.yaw, box.roll ); // Godot's YXZ euler
		pieces.push_back( piece );
	}
	result["pieces"] = pieces;
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
