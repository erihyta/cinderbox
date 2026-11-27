#include "sheet_motions.h"

#include "cue_rules.h"
#include "sheet_bake.h"
#include "sheet_parts.h"
#include "sheet_rules.h"

#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/capsule_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/cylinder_shape3d.hpp>
#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/path3d.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/core/math.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

// A rules sheet's motion blocks, baked for the simulation (docs/sheets.md). Moved out of
// cinderbox_motion.cpp in M268, when the motion nodes it baked through were deleted; since M283 the
// bake is the sheet compiler's (src/sheet, sheet_bake.h), and this is the scene it asks about the
// nodes a sheet names: a find's shape, a marker, a path.

using namespace godot;

namespace cb::gd
{

bool MotionTextPlain( const String& text )
{
	return sheet::MotionTextPlain( std::string( text.utf8().get_data() ) );
}

namespace
{

std::string Std( const String& s )
{
	return std::string( s.utf8().get_data() );
}

} // namespace

// --- The scene a sheet's motions name nodes of ------------------------------------------------------

namespace
{

// Where a node is in `root`'s frame (its transforms up to it); false when it is not under it.
bool FrameIn( const Node* node, const Node* root, Transform3D& out )
{
	out = Transform3D();
	for ( const Node* at = node; at != nullptr; at = at->get_parent() )
	{
		if ( at == root )
		{
			return true;
		}
		if ( const auto* spatial = Object::cast_to<Node3D>( at ) )
		{
			out = spatial->get_transform() * out;
		}
	}
	return false;
}


// A node beside the sheet, and where it is in the frame of what is baked; false with why.
bool PlaceOf( const String& path, const Node* from, const Node* root, const char* what, Transform3D& out, String& why )
{
	const Node* node = from != nullptr ? from->get_node_or_null( NodePath( path ) ) : nullptr;
	if ( Object::cast_to<Node3D>( node ) == nullptr )
	{
		why = String( what ) + " " + path + ": no Node3D (a Marker3D) of that name beside the sheet";
		return false;
	}
	if ( FrameIn( node, root, out ) == false )
	{
		why = String( what ) + " " + path + " is not in the scene being baked";
		return false;
	}
	return true;
}

} // namespace

// The scene a sheet's motions name nodes of (sheet_bake.h): the node its CbRules is under (paths
// start there) and the item or the motion set being baked (shapes and markers are placed in its frame).
class GodotScene final : public sheet::Scene
{
public:
	GodotScene( const Node* from, const Node* root )
		: m_from( from )
		, m_root( root )
	{
	}

	bool Place( const std::string& path, const char* what, sheet::Float3& zAxis, sheet::Float3& origin, std::string& why ) const override
	{
		Transform3D place;
		String w;
		if ( PlaceOf( String::utf8( path.c_str() ), m_from, m_root, what, place, w ) == false )
		{
			why = Std( w );
			return false;
		}
		const Vector3 z = place.basis.get_column( 2 );
		zAxis = { z.x, z.y, z.z };
		origin = { place.origin.x, place.origin.y, place.origin.z };
		return true;
	}

	bool PathSamples( const std::string& name, std::vector<sheet::Float3>& out, std::string& ) const override
	{
		const String path = String::utf8( name.c_str() );
		const Path3D* node = Object::cast_to<Path3D>( m_from != nullptr ? m_from->get_node_or_null( NodePath( path ) ) : nullptr );
		Ref<Curve3D> curve = node != nullptr ? node->get_curve() : Ref<Curve3D>();
		Transform3D place;
		if ( curve.is_null() || curve->get_point_count() < 2 || FrameIn( node, m_root, place ) == false )
		{
			return false; // (said by the bake)
		}
		const float length = curve->get_baked_length();
		for ( int k = 0; k < sheet::kPathSamples; ++k )
		{
			const Vector3 at = place.xform( curve->sample_baked( length * float( k ) / float( sheet::kPathSamples - 1 ) ) );
			out.push_back( { at.x, at.y, at.z } );
		}
		return true;
	}

	bool Shapes( const std::string& name, bool own, sheet::Float3 shift, std::vector<sheet::ShapePiece>& out, std::string& why ) const override
	{
		const String path = String::utf8( name.c_str() );
		const Vector3 moved( shift.x, shift.y, shift.z );
		const Node* node = m_from != nullptr ? m_from->get_node_or_null( NodePath( path ) ) : nullptr;
		if ( node == nullptr )
		{
			why = Std( "find: no node " + path + ( m_from != nullptr ? " (a path from " + String( m_from->get_name() ) + ")" : String() ) );
			return false;
		}
		std::vector<const CollisionShape3D*> shapes;
		if ( const auto* single = Object::cast_to<CollisionShape3D>( node ) )
		{
			shapes.push_back( single );
		}
		else if ( Object::cast_to<Area3D>( node ) != nullptr )
		{
			for ( int i = 0; i < node->get_child_count(); ++i )
			{
				if ( const auto* child = Object::cast_to<CollisionShape3D>( node->get_child( i ) ) )
				{
					shapes.push_back( child );
				}
			}
		}
		if ( shapes.empty() )
		{
			why = Std( "find: " + path + " is not an Area3D with CollisionShape3D children, or a CollisionShape3D" );
			return false;
		}
		auto point = []( const Vector3& p ) { return sheet::Float3{ p.x, p.y, p.z }; };
		for ( const CollisionShape3D* piece : shapes )
		{
			if ( piece->is_disabled() )
			{
				continue;
			}
			Transform3D xf;
			if ( FrameIn( piece, own ? node : m_root, xf ) == false )
			{
				why = Std( "find: " + path + " is not in the scene being baked" );
				return false;
			}
			xf.origin += moved;
			const float scale = xf.basis.get_scale().abs().x;
			Ref<Shape3D> s = piece->get_shape();
			sheet::ShapePiece made;
			if ( auto* ball = Object::cast_to<SphereShape3D>( s.ptr() ) )
			{
				made.radius = ball->get_radius() * scale;
				made.points.push_back( point( xf.origin ) );
			}
			else if ( auto* capsule = Object::cast_to<CapsuleShape3D>( s.ptr() ) )
			{
				float r = capsule->get_radius(), h = std::max( capsule->get_height(), 2.0f * r );
				made.radius = r * scale;
				made.points.push_back( point( xf.xform( Vector3( 0, h / 2 - r, 0 ) ) ) );
				made.points.push_back( point( xf.xform( Vector3( 0, r - h / 2, 0 ) ) ) );
			}
			else if ( auto* box = Object::cast_to<BoxShape3D>( s.ptr() ) )
			{
				Vector3 half = box->get_size() / 2.0f;
				for ( int c = 0; c < 8; ++c )
				{
					made.points.push_back( point( xf.xform( Vector3( c & 1 ? half.x : -half.x, c & 2 ? half.y : -half.y, c & 4 ? half.z : -half.z ) ) ) );
				}
			}
			else if ( auto* cylinder = Object::cast_to<CylinderShape3D>( s.ptr() ) )
			{
				// Two rings of 16.
				for ( int ring = 0; ring < 2; ++ring )
				{
					for ( int k = 0; k < 16; ++k )
					{
						float angle = float( k ) * float( Math_TAU ) / 16.0f;
						made.points.push_back( point( xf.xform( Vector3( cylinder->get_radius() * std::cos( angle ), ( ring == 0 ? 0.5f : -0.5f ) * cylinder->get_height(),
																		 cylinder->get_radius() * std::sin( angle ) ) ) ) );
					}
				}
			}
			else if ( auto* hull = Object::cast_to<ConvexPolygonShape3D>( s.ptr() ) )
			{
				PackedVector3Array points = hull->get_points();
				if ( points.is_empty() || points.size() > 128 )
				{
					why = Std( "find: " + path + "'s convex shape has " + String::num_int64( points.size() ) + " points (1 to 128)" );
					return false;
				}
				for ( int64_t k = 0; k < points.size(); ++k )
				{
					made.points.push_back( point( xf.xform( points[k] ) ) );
				}
			}
			else
			{
				why = Std( "find: " + path + ": a find's shape is a sphere, a capsule, a box, a cylinder or a convex polygon" );
				return false;
			}
			out.push_back( made );
		}
		return true;
	}

private:
	const Node* m_from;
	const Node* m_root;
};

bool BakeSheetMotions( const Node* node, const String& kind, int use, std::string& lines, std::vector<std::string>& names, sheet::OwnerParts* parts,
					   String& error )
{
	// Every sheet under it, each with the scene its paths start in (the node its CbRules is under);
	// the sheet compiler bakes them as the server will (sheet_parts.h, M294), recording their parts.
	TypedArray<Node> holders = node->find_children( "*", "CbRules", true, false );
	std::vector<GodotScene> scenes;
	scenes.reserve( size_t( holders.size() ) );
	std::vector<sheet::SheetSource> sources;
	for ( int64_t s = 0; s < holders.size(); ++s )
	{
		const CbRules* holder = Object::cast_to<CbRules>( holders[s] );
		if ( holder == nullptr || holder->get_sheet().is_empty() )
		{
			continue;
		}
		const String path = holder->get_sheet();
		// (A path that names no sheet is the bake's mistake, said: a file moved or renamed.)
		if ( const String problem = holder->SheetProblem(); problem.is_empty() == false )
		{
			error = problem;
			return false;
		}
		scenes.emplace_back( Object::cast_to<Node>( holders[s] )->get_parent(), node );
		sheet::SheetSource source;
		source.name = Std( holder->SheetName() );
		source.path = path.begins_with( "res://" ) && path.ends_with( ".cbsheet" ) ? Std( path.substr( 6 ) ) : std::string();
		source.text = Std( holder->SheetText() );
		source.scene = &scenes.back();
		sources.push_back( source );
	}
	std::string why;
	if ( sheet::BakeOwnerMotions( sources, Std( kind ), use, lines, names, parts, why ) == false )
	{
		error = String::utf8( why.c_str() );
		return false;
	}
	return true;
}

} // namespace cb::gd
