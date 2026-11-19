#include "sheet_motions.h"

#include "cue_names.h"

#include "cue/cue_info.h"
#include "cue/cue_prediction.h" // ParseChange: a motion's changes are written as a prediction's are
#include "cue/cue_rules.h"

#include "expr.h"
#include "mod_schema.h"
#include "motions.h"
#include "move_params.h"

#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/capsule_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/cylinder_shape3d.hpp>
#include <godot_cpp/classes/curve3d.hpp>
#include <godot_cpp/classes/path3d.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <functional>
#include <charconv>
#include <vector>

// A rules sheet's motion blocks, baked for the simulation (docs/rules-language.md). Moved out of
// cinderbox_motion.cpp in M268, when the motion nodes it baked through were deleted.

using namespace godot;

namespace cb::gd
{

bool MotionTextPlain( const String& text )
{
	return text.contains( "\t" ) == false && text.contains( "\n" ) == false && text.contains( "#" ) == false;
}

namespace
{

std::string Std( const String& s )
{
	return std::string( s.utf8().get_data() );
}

// The shortest text that reads back as the same float.
std::string Num( double value )
{
	char buffer[32];
	auto end = std::to_chars( buffer, buffer + sizeof( buffer ), float( value ) ).ptr;
	return std::string( buffer, end );
}

bool Plain( const String& text )
{
	return MotionTextPlain( text );
}

// "param" lines of a parameters dictionary, in the simulation's order; "" with `error` for a name
// that is not a movement parameter.
std::string ParamLines( const Dictionary& parameters, String& error )
{
	Array names = parameters.keys();
	for ( int64_t i = 0; i < names.size(); ++i )
	{
		if ( MoveParamByName( Std( String( names[i] ).strip_edges() ).c_str() ) < 0 )
		{
			error = "parameters: \"" + String( names[i] ) + "\" is not a movement parameter";
			return std::string();
		}
	}
	std::string text;
	for ( int p = 0; p < kMoveParams; ++p )
	{
		for ( int64_t i = 0; i < names.size(); ++i )
		{
			if ( MoveParamByName( Std( String( names[i] ).strip_edges() ).c_str() ) == p )
			{
				// A number, or an expression (a sheet's: read when its step starts).
				const Variant value = parameters[names[i]];
				if ( value.get_type() == Variant::STRING && Plain( String( value ) ) == false )
				{
					error = "parameters: \"" + String( value ) + "\" cannot have a tab or a #";
					return std::string();
				}
				text += std::string( "param\t" ) + MoveParamInfoOf( p ).name + "\t" +
						( value.get_type() == Variant::STRING ? Std( String( value ).strip_edges() ) : Num( double( value ) ) ) + "\n";
			}
		}
	}
	return text;
}

// Several conditions as one expression: all of them (" and "), or any of them (" or ").
bool Joined( const PackedStringArray& conditions, const char* with, String& out, String& bad )
{
	out = String();
	for ( int64_t i = 0; i < conditions.size(); ++i )
	{
		String condition = conditions[i].strip_edges();
		if ( condition.is_empty() )
		{
			continue;
		}
		if ( Plain( condition ) == false )
		{
			bad = condition;
			return false;
		}
		out += String( out.is_empty() ? "" : with ) + "( " + condition + " )";
	}
	return true;
}

const char* const kFrames[] = { "look", "move", "facing", "up", "world", "to", "away" };
const char* const kReplaces[] = { "none", "vertical", "horizontal", "all" };
const char* const kPushes[] = { "accel", "force", "velocity" };

// A statement's impulse, force, link or launch (cue_rules.cpp: its "class" and properties) as its
// line. Until M268 each was the motion node of that class, made for the bake; these are the nodes'
// defaults for what the statement leaves out, and their lines word for word.
std::string PartLine( const String& cls, const Dictionary& p, String& why )
{
	auto number = [&]( const char* key, double fallback ) { return double( p.get( key, fallback ) ); };
	auto whole = [&]( const char* key, int fallback ) { return int( p.get( key, fallback ) ); };
	auto direction = [&]() {
		const Vector3 d = Vector3( p.get( "direction", Vector3( 0, 1, 0 ) ) ).normalized();
		return "\t" + Num( d.x ) + "\t" + Num( d.y ) + "\t" + Num( d.z );
	};
	if ( cls == "CbLaunch" )
	{
		const String kind = String( p.get( "item_kind", "" ) ).strip_edges();
		if ( kind.is_empty() || Plain( kind ) == false || kind.contains( " " ) )
		{
			why = "launch: name the item kind it throws (one a server mod declares, and a CbItem bakes: grenade.shell)";
			return std::string();
		}
		const int frame = whole( "frame", 0 ); // (look, move, facing, up, world)
		std::string line = std::string( whole( "what", 0 ) == 1 ? "launch_carried\t" : "launch\t" ) + Std( kind ) + "\t" +
						   Num( number( "speed", 15.0 ) ) + "\t" + kFrames[frame >= 0 && frame <= 4 ? frame : 0] + "\t" + Num( number( "lift", 0.0 ) ) +
						   "\t" + Num( number( "ahead", 0.7 ) ) + "\t" + Num( number( "seconds", 5.0 ) );
		return line + ( frame == 4 ? direction() : std::string() ) + "\n";
	}
	const bool impulse = cls == "CbImpulse", force = cls == "CbForce", link = cls == "CbLink";
	if ( impulse == false && force == false && link == false )
	{
		why = "this game has no " + cls;
		return std::string();
	}
	// Who it acts on: the player (0), what the cast holds (1), the entity a field names (2), what a
	// find found (3). A rope goes to what the cast holds unless it says.
	const int target = whole( "target", link ? 1 : 0 );
	std::string on = target == 0 ? "self" : target == 1 ? "hit" : target == 3 ? "found" : std::string();
	if ( on.empty() )
	{
		const String field = String( p.get( "target_field", "" ) ).strip_edges();
		if ( target != 2 || field.is_empty() || Plain( field ) == false || field.contains( " " ) )
		{
			why = "name the field whose entity it acts on (one the server mod writes an entity's id into)";
			return std::string();
		}
		on = "@" + Std( field );
	}
	if ( link )
	{
		if ( target == 0 )
		{
			why = "a rope goes to something else: what the cast holds, or the entity a field names";
			return std::string();
		}
		// (A rope that reacts, the default, bakes as it always did.)
		return "link\t" + on + "\t" + Num( number( "length", 0.0 ) ) + "\t" + Num( number( "reel", 0.0 ) ) +
			   ( bool( p.get( "react", true ) ) ? "" : "\t0" ) + "\n";
	}
	const int frame = whole( "frame", 1 ); // (look, move, facing, up, world, to, away)
	const std::string along = kFrames[frame >= 0 && frame <= 6 ? frame : 1];
	const std::string world = frame == 4 ? direction() : std::string();
	if ( impulse )
	{
		const int replace = whole( "replace", 0 );
		return "impulse\t" + on + "\t" + Num( number( "speed", 0.0 ) ) + "\t" + along + "\t" + kReplaces[replace >= 0 && replace <= 3 ? replace : 0] +
			   world + "\n";
	}
	const int push = whole( "kind", 0 );
	return "force\t" + on + "\t" + along + "\t" + kPushes[push >= 0 && push <= 2 ? push : 0] + "\t" + Num( number( "strength", 0.0 ) ) + "\t" +
		   Num( number( "speed", 0.0 ) ) + "\t" + Num( number( "ramp_in", 0.0 ) ) + "\t" + ( bool( p.get( "react", false ) ) ? "1" : "0" ) + world +
		   "\n";
}

} // namespace

// --- A rules sheet's motion blocks ------------------------------------------------------------------

namespace
{

// Where a node is in `root`'s frame (below).
bool FrameIn( const Node* node, const Node* root, Transform3D& out );

// A sheet's "change FIELD OP VALUE" as a "change" line: its value a number, or an expression (the
// rest of the line). "" with why when it cannot be read.
std::string ChangeLine( const String& text, String& why )
{
	cue::Change change;
	if ( cue::ParseChange( text, change, &why ) && Plain( change.field ) )
	{
		return "change\t" + Std( change.field ) + "\t" + ( change.op == '=' ? std::string( "=" ) : std::string( 1, change.op ) + "=" ) + "\t" +
			   Num( change.value ) + "\n";
	}
	const String trimmed = text.strip_edges();
	PackedStringArray words = trimmed.split( " ", false );
	if ( words.size() < 3 || ( words[1] != "=" && words[1] != "+=" && words[1] != "-=" ) || Plain( trimmed ) == false )
	{
		why = "change FIELD = VALUE, += or -= (a number or an expression)";
		return std::string();
	}
	const String value = trimmed.substr( trimmed.find( " " + words[1] + " " ) + words[1].length() + 2 ).strip_edges();
	why = String();
	return "change\t" + Std( words[0] ) + "\t" + Std( words[1] ) + "\t" + Std( value ) + "\n";
}

// A point of the scene, in the simulation's frame (forward +Z): Godot's forward is -Z.
Vector3 SimPoint( const Vector3& p )
{
	return Vector3( 0.0f - p.x, p.y, 0.0f - p.z ); // (0 - 0 is 0, where -0 would print "-0")
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

std::string BakeSheetPart( const Dictionary& part, const String& kind, const Node* from, const Node* root, String& why )
{
	Dictionary props = part.get( "properties", Dictionary() ).duplicate();
	// Along a marker: its forward in the scene, written as a world direction and named "marker" after.
	const String marker = props.get( "marker", "" );
	if ( marker.is_empty() == false )
	{
		Transform3D place;
		if ( PlaceOf( marker, from, root, "along", place, why ) == false )
		{
			return std::string();
		}
		Vector3 forward = SimPoint( -place.basis.get_column( 2 ) ).normalized();
		if ( forward.length() < 0.5f )
		{
			why = "along " + marker + ": it has no forward (its scale is 0)";
			return std::string();
		}
		props["frame"] = 4;
		props["direction"] = forward;
	}
	if ( String( part["class"] ) == "CbMove" )
	{
		// Parameters from when the step starts: for its seconds, or while it is the step.
		std::string seconds = double( props.get( "duration", 0.0 ) ) > 0.0 ? "\t" + Num( double( props["duration"] ) ) : std::string();
		std::string params = ParamLines( props.get( "parameters", Dictionary() ), why );
		if ( params.empty() )
		{
			why = why.is_empty() ? String( "move wants a parameter and a value" ) : why;
			return std::string();
		}
		for ( size_t at = 0; ( at = params.find( '\n', at ) ) != std::string::npos; at += seconds.size() + 1 )
		{
			params.insert( at, seconds );
		}
		return params;
	}
	if ( String( part["class"] ) == "CbLaunch" && int( props.get( "what", 0 ) ) == 1 && props.has( "item_kind" ) == false )
	{
		props["item_kind"] = kind; // (the one carried: the item's own)
	}
	// (Its direction is the simulation's already: written as it is, normalized.)
	std::string lines = PartLine( String( part["class"] ), props, why );
	// The amounts that are expressions, each in its number's place in the first line (by its field:
	// an impulse's speed 2; a force's strength 4 and top 5; a link's length 2 and reel 3; a launch's
	// speed 2, lift 4, ahead 5, seconds 6).
	Dictionary amounts = props.get( "amounts", Dictionary() );
	Array fields = amounts.keys();
	for ( int64_t f = 0; lines.empty() == false && f < fields.size(); ++f )
	{
		const String expression = String( amounts[fields[f]] ).strip_edges();
		if ( Plain( expression ) == false )
		{
			why = "\"" + expression + "\" cannot have a tab or a #";
			return std::string();
		}
		const int field = int( fields[f] );
		size_t at = 0;
		for ( int k = 0; k < field && at != std::string::npos; ++k )
		{
			at = lines.find( '\t', at );
			at = at == std::string::npos ? at : at + 1;
		}
		const size_t first = lines.find( '\n' );
		if ( at == std::string::npos || at > first )
		{
			why = "\"" + expression + "\": no place for it in what the line bakes to";
			return std::string();
		}
		size_t end = lines.find_first_of( "\t\n", at );
		lines.replace( at, end - at, Std( expression ) );
	}
	if ( lines.empty() )
	{
		return lines;
	}
	if ( bool( props.get( "momentum", false ) ) && lines.rfind( "impulse\t", 0 ) == 0 )
	{
		lines.replace( 0, 7, "impulse_momentum" ); // (its number N s, not m/s)
	}
	if ( marker.is_empty() == false )
	{
		// The frame's word: "world" becomes "marker" (a force has it a field earlier).
		const int field = String( part["class"] ) == "CbForce" ? 2 : 3;
		size_t at = 0;
		for ( int k = 0; k < field; ++k )
		{
			at = lines.find( '\t', at ) + 1;
		}
		lines.replace( at, 5, "marker" );
	}
	if ( props.has( "turn" ) )
	{
		Vector2 turn = props["turn"];
		if ( turn.y < -90.0f || turn.y > 90.0f || turn.x < -360.0f || turn.x > 360.0f )
		{
			why = "turn (yaw, pitch): a heading from -360 to 360 degrees, a pitch from -90 to 90";
			return std::string();
		}
		lines += "turn\t" + Num( turn.x ) + "\t" + Num( turn.y ) + "\n";
	}
	const String start = props.get( "from", "" );
	if ( start.is_empty() == false )
	{
		Transform3D place;
		if ( PlaceOf( start, from, root, "from", place, why ) == false )
		{
			return std::string();
		}
		Vector3 at = SimPoint( place.origin );
		lines += "from\t" + Num( at.x ) + "\t" + Num( at.y ) + "\t" + Num( at.z ) + "\n";
	}
	return lines;
}

// The paths a motion's follows carry the player along (Path3D nodes beside the sheet): "path" lines,
// points at even steps along each from where it starts, in the simulation's frame.
bool PathLines( const std::vector<std::string>& names, const Node* from, const Node* root, std::string& out, String& why )
{
	for ( const std::string& name : names )
	{
		const String path = String::utf8( name.c_str() );
		const Path3D* node = Object::cast_to<Path3D>( from != nullptr ? from->get_node_or_null( NodePath( path ) ) : nullptr );
		Ref<Curve3D> curve = node != nullptr ? node->get_curve() : Ref<Curve3D>();
		Transform3D place;
		if ( curve.is_null() || curve->get_point_count() < 2 || FrameIn( node, root, place ) == false )
		{
			why = "follow " + path + ": no Path3D of that name, with two points at least, beside the sheet";
			return false;
		}
		const float length = curve->get_baked_length();
		const Vector3 first = place.xform( curve->sample_baked( 0.0f ) );
		out += "path\t" + name;
		for ( int k = 0; k < kPathSamples; ++k )
		{
			Vector3 at = SimPoint( place.xform( curve->sample_baked( length * float( k ) / float( kPathSamples - 1 ) ) ) - first );
			out += "\t" + Num( at.x ) + "\t" + Num( at.y ) + "\t" + Num( at.z );
		}
		out += "\n";
	}
	return true;
}

// The curves a motion's expressions read ("curve(Falloff, reach)"): none (M268). They were drawn in
// CbCurve nodes beside the sheet, deleted with the motion nodes; the simulation still reads a
// motions file's "curve" lines (ROADMAP, postponed: curves as resources).
bool NoCurves( const std::vector<std::string>& expressions, String& why )
{
	for ( const std::string& text : expressions )
	{
		expr::Program program;
		std::string error;
		if ( text.empty() || expr::Compile( text, program, error ) == false )
		{
			continue; // (what is not an expression is said by the simulation's compiler)
		}
		if ( program.curves.empty() == false )
		{
			why = "curve(" + String::utf8( program.curves[0].c_str() ) + ", ...): a sheet has no curves (yet)";
			return false;
		}
	}
	return true;
}

// A point of a shape in the player's frame, as the simulation has it: Godot's forward is -Z, the
// simulation's +Z (the same turn about Y by half a circle).
std::string PointWords( const Vector3& p )
{
	return "\t" + Num( 0.0f - p.x ) + "\t" + Num( p.y ) + "\t" + Num( 0.0f - p.z ); // (0 - 0 is 0, where -0 would print "-0")
}

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

} // namespace

bool FindShapeLines( const Dictionary& shape, const Node* from, const Node* root, std::string& out, String& why )
{
	const String kind = shape.get( "kind", "" );
	// Every shape is placed from the player's feet, as if it stood at the scene's origin facing -Z:
	// one drawn where it is drawn, one written in the line around the feet; "at (x, y, z)" moves
	// either, metres right, up and ahead.
	const Vector3 at = shape.get( "at", Vector3() );
	const Vector3 moved( at.x, at.y, 0.0f - at.z );
	if ( kind == "sphere" )
	{
		out = "shape\t" + Num( double( shape["radius"] ) ) + PointWords( moved ) + "\n";
		return true;
	}
	if ( kind == "capsule" )
	{
		double r = shape["radius"], h = shape["height"];
		out = "shape\t" + Num( r ) + PointWords( moved + Vector3( 0, float( h / 2.0 - r ), 0 ) ) + PointWords( moved + Vector3( 0, float( r - h / 2.0 ), 0 ) ) + "\n";
		return true;
	}
	if ( kind == "box" )
	{
		Vector3 half = Vector3( shape["size"] ).abs() / 2.0f;
		out = "shape\t0";
		for ( int c = 0; c < 8; ++c )
		{
			out += PointWords( moved + Vector3( c & 1 ? half.x : -half.x, c & 2 ? half.y : -half.y, c & 4 ? half.z : -half.z ) );
		}
		out += "\n";
		return true;
	}
	// A node of the scene: an Area3D (its CollisionShape3D children) or a CollisionShape3D, drawn
	// around the scene's origin as if the player stood there facing -Z.
	const String path = shape.get( "path", "" );
	const Node* node = from != nullptr ? from->get_node_or_null( NodePath( path ) ) : nullptr;
	if ( node == nullptr )
	{
		why = "find: no node " + path + ( from != nullptr ? " (a path from " + String( from->get_name() ) + ")" : String() );
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
		why = "find: " + path + " is not an Area3D with CollisionShape3D children, or a CollisionShape3D";
		return false;
	}
	out.clear();
	for ( const CollisionShape3D* piece : shapes )
	{
		if ( piece->is_disabled() )
		{
			continue;
		}
		Transform3D xf;
		if ( FrameIn( piece, root, xf ) == false )
		{
			why = "find: " + path + " is not in the scene being baked";
			return false;
		}
		xf.origin += moved;
		const float scale = xf.basis.get_scale().abs().x;
		Ref<Shape3D> s = piece->get_shape();
		if ( auto* ball = Object::cast_to<SphereShape3D>( s.ptr() ) )
		{
			out += "shape\t" + Num( ball->get_radius() * scale ) + PointWords( xf.origin ) + "\n";
		}
		else if ( auto* capsule = Object::cast_to<CapsuleShape3D>( s.ptr() ) )
		{
			float r = capsule->get_radius(), h = std::max( capsule->get_height(), 2.0f * r );
			out += "shape\t" + Num( r * scale ) + PointWords( xf.xform( Vector3( 0, h / 2 - r, 0 ) ) ) + PointWords( xf.xform( Vector3( 0, r - h / 2, 0 ) ) ) + "\n";
		}
		else if ( auto* box = Object::cast_to<BoxShape3D>( s.ptr() ) )
		{
			Vector3 half = box->get_size() / 2.0f;
			out += "shape\t0";
			for ( int c = 0; c < 8; ++c )
			{
				out += PointWords( xf.xform( Vector3( c & 1 ? half.x : -half.x, c & 2 ? half.y : -half.y, c & 4 ? half.z : -half.z ) ) );
			}
			out += "\n";
		}
		else if ( auto* cylinder = Object::cast_to<CylinderShape3D>( s.ptr() ) )
		{
			// Two rings of 16.
			out += "shape\t0";
			for ( int ring = 0; ring < 2; ++ring )
			{
				for ( int k = 0; k < 16; ++k )
				{
					float angle = float( k ) * float( Math_TAU ) / 16.0f;
					out += PointWords( xf.xform( Vector3( cylinder->get_radius() * std::cos( angle ), ( ring == 0 ? 0.5f : -0.5f ) * cylinder->get_height(),
														  cylinder->get_radius() * std::sin( angle ) ) ) );
				}
			}
			out += "\n";
		}
		else if ( auto* hull = Object::cast_to<ConvexPolygonShape3D>( s.ptr() ) )
		{
			PackedVector3Array points = hull->get_points();
			if ( points.is_empty() || points.size() > 128 )
			{
				why = "find: " + path + "'s convex shape has " + String::num_int64( points.size() ) + " points (1 to 128)";
				return false;
			}
			out += "shape\t0";
			for ( int64_t k = 0; k < points.size(); ++k )
			{
				out += PointWords( xf.xform( points[k] ) );
			}
			out += "\n";
		}
		else
		{
			why = "find: " + path + ": a find's shape is a sphere, a capsule, a box, a cylinder or a convex polygon";
			return false;
		}
	}
	if ( out.empty() )
	{
		why = "find: " + path + " has no shape that is on";
		return false;
	}
	return true;
}

std::string BakeSheetMotion( const Dictionary& p, const String& kind, const Node* from, const Node* root, String& error )
{
	auto fail = [&]( const String& why ) {
		error = why;
		return std::string();
	};
	const String name = p.get( "name", "" );
	if ( name.is_empty() || Plain( name ) == false || name.contains( " " ) )
	{
		return fail( "a motion's name is one word" );
	}
	std::string text = "motion\t" + Std( name ) + "\n";
	{
		// Every expression it has, for the curves they read (a sheet has none: NoCurves).
		std::vector<std::string> expressions;
		auto add = [&]( const Variant& v ) {
			if ( v.get_type() == Variant::PACKED_STRING_ARRAY )
			{
				PackedStringArray all = v;
				for ( int64_t k = 0; k < all.size(); ++k )
				{
					expressions.push_back( Std( all[k] ) );
				}
			}
			else if ( v.get_type() == Variant::STRING )
			{
				expressions.push_back( Std( String( v ) ) );
			}
		};
		add( p.get( "expression", Variant() ) );
		add( p.get( "conditions", Variant() ) );
		add( p.get( "until", Variant() ) );
		std::function<void( const Array& )> statementsOf = [&]( const Array& list ) {
			for ( int64_t k = 0; k < list.size(); ++k )
			{
				Dictionary statement = list[k];
				add( statement.get( "guard", Variant() ) );
				// Its amounts that are expressions: a part's, a move's values, a change's, a break's.
				Dictionary props = Dictionary( statement.get( "part", Dictionary() ) ).get( "properties", Dictionary() );
				Array amounts = Dictionary( props.get( "amounts", Dictionary() ) ).values();
				for ( int64_t a = 0; a < amounts.size(); ++a )
				{
					add( amounts[a] );
				}
				Array values = Dictionary( props.get( "parameters", Dictionary() ) ).values();
				for ( int64_t a = 0; a < values.size(); ++a )
				{
					add( values[a] );
				}
				for ( const char* key : { "radius", "speed", "seconds" } )
				{
					add( statement.get( key, Variant() ) );
				}
				if ( String( statement.get( "kind", "" ) ) == "change" )
				{
					// (Its value: after the operator.)
					PackedStringArray words = String( statement.get( "text", "" ) ).strip_edges().split( " ", false );
					String rest;
					for ( int64_t k = 2; k < words.size(); ++k )
					{
						rest += ( k > 2 ? " " : "" ) + words[k];
					}
					add( rest );
				}
				statementsOf( statement.get( "statements", Array() ) );
			}
		};
		Array all = p.get( "steps", Array() );
		for ( int64_t k = 0; k < all.size(); ++k )
		{
			add( Dictionary( all[k] ).get( "wait", Variant() ) );
			statementsOf( Dictionary( all[k] ).get( "statements", Array() ) );
		}
		String why;
		if ( NoCurves( expressions, why ) == false )
		{
			return fail( why );
		}
		// The paths its follows name, the same way.
		std::vector<std::string> paths;
		for ( int64_t k = 0; k < all.size(); ++k )
		{
			Array list = Dictionary( all[k] ).get( "statements", Array() );
			for ( int64_t i = 0; i < list.size(); ++i )
			{
				Dictionary statement = list[i];
				std::string name = Std( String( statement.get( "path", "" ) ) );
				if ( String( statement.get( "kind", "" ) ) == "follow" && std::find( paths.begin(), paths.end(), name ) == paths.end() )
				{
					paths.push_back( name );
				}
			}
		}
		std::string pathLines;
		if ( PathLines( paths, from, root, pathLines, why ) == false )
		{
			return fail( why );
		}
		text += pathLines;
	}
	const String trigger = p.get( "trigger", "" );
	PackedStringArray conditions;
	auto word = [&]( const char* key ) {
		String w = String( p.get( key, "" ) ).strip_edges();
		return Plain( w ) && w.contains( " " ) == false ? w : String();
	};
	const bool ownerItem = String( p.get( "owner", "player" ) ) == "item";
	if ( ownerItem && kind.is_empty() )
	{
		return fail( "owner item is for an item's sheet" );
	}
	if ( ownerItem && trigger != "event" )
	{
		return fail( "an item's motion (owner item) starts on an event said at it: on expired, or on EVENT" );
	}
	// The item's own "When used" fields (its CbItem's use_conditions, use_changes, use_cooldown,
	// use_emits) join its motion named Use, as they join a use made of nodes.
	const bool itemUse = trigger == "use" && name == "Use" && root != nullptr && root->get( "use_conditions" ).get_type() == Variant::PACKED_STRING_ARRAY;
	if ( trigger == "use" )
	{
		if ( kind.is_empty() )
		{
			return fail( "on use is for an item's sheet (a motion set's motions are everyone's)" );
		}
		// An item used while its key is held (use: a press, and while held) is in use while it is held:
		// its use motions are on for as long, as a use made of nodes is.
		if ( root != nullptr && int( root->get( "use" ) ) == kItemUseHold )
		{
			text += "when\twhile\n";
			conditions.push_back( "using." + kind );
		}
		else
		{
			text += "when\tevent\t" + Std( kind ) + ".used\n";
		}
		if ( itemUse )
		{
			conditions.append_array( PackedStringArray( root->get( "use_conditions" ) ) );
		}
	}
	else if ( trigger == "press" )
	{
		if ( word( "action" ).is_empty() )
		{
			return fail( "on press wants one action" );
		}
		text += "when\tpress\t" + Std( word( "action" ) ) + "\n";
	}
	else if ( trigger == "event" )
	{
		if ( word( "event" ).is_empty() )
		{
			return fail( "on wants one event" );
		}
		// "expired": the item's own time being up ("<kind>.expired", said by the simulation).
		String event = word( "event" ) == "expired" && kind.is_empty() == false ? kind + String( ".expired" ) : word( "event" );
		text += "when\tevent\t" + Std( event ) + "\n";
	}
	else if ( trigger == "while" || trigger == "when" )
	{
		String expression = String( p.get( "expression", "" ) ).strip_edges();
		if ( expression.is_empty() || Plain( expression ) == false )
		{
			return fail( trigger + String( " wants an expression (with no tab or #)" ) );
		}
		if ( trigger == "while" )
		{
			text += "when\twhile\n";
			conditions.push_back( expression );
		}
		else
		{
			text += "when\tedge\t" + Std( expression ) + "\n";
		}
	}
	else
	{
		return fail( "a motion starts on use, on press, on an event, while or when" );
	}
	if ( ownerItem )
	{
		text += "owner\titem\n";
	}
	// Who may: in an item's sheet, whoever has it out (or carries it); its use says so itself, and an
	// item's own motion is not a player's.
	if ( kind.is_empty() == false && trigger != "use" && ownerItem == false )
	{
		conditions.push_back( String( p.get( "item", "hand" ) ) == "carried" ? "carried." + kind : kind );
	}
	conditions.append_array( p.get( "conditions", PackedStringArray() ) );
	String all, any, bad;
	if ( Joined( conditions, " and ", all, bad ) == false || Joined( p.get( "until", PackedStringArray() ), " or ", any, bad ) == false )
	{
		return fail( "\"" + bad + "\" cannot have a tab or a #" );
	}
	if ( all.is_empty() == false )
	{
		text += "if\t" + Std( all ) + "\n";
	}
	if ( any.is_empty() == false )
	{
		text += "until\t" + Std( any ) + "\n";
	}
	const double cooldown = p.has( "cooldown" ) ? double( p["cooldown"] ) : itemUse ? double( root->get( "use_cooldown" ) ) : 0.0;
	if ( cooldown > 0.0 )
	{
		text += "cooldown\t" + Num( cooldown ) + "\n";
	}
	if ( int( p.get( "uses", 0 ) ) > 0 )
	{
		Variant refill = p.get( "refill", 0.0 );
		text += "uses\t" + std::to_string( int( p["uses"] ) ) + "\t" +
				( refill.get_type() == Variant::STRING ? Std( String( refill ) ) : Num( double( refill ) ) ) + "\n";
	}
	if ( double( p.get( "lasts", 0.0 ) ) > 0.0 )
	{
		text += "duration\t" + Num( double( p["lasts"] ) ) + "\n";
	}

	Array steps = p.get( "steps", Array() );
	// With a cast, what the first step does to bodies waits for it to take hold: so it goes under it.
	if ( steps.is_empty() == false )
	{
		Array first = Dictionary( steps[0] ).get( "statements", Array() );
		bool casts = false;
		int64_t loose = -1;
		for ( int64_t i = 0; i < first.size(); ++i )
		{
			Dictionary statement = first[i];
			casts = casts || String( statement.get( "kind", "" ) ) == "cast";
			loose = loose < 0 && String( statement.get( "kind", "" ) ) == "part" ? int64_t( statement.get( "line", 0 ) ) : loose;
		}
		if ( casts && loose >= 0 )
		{
			return fail( "line " + String::num_int64( loose ) +
						 ": with a cast, what the motion's first step does to bodies goes under the cast (it happens while it holds)" );
		}
	}
	for ( int64_t s = 0; s < steps.size(); ++s )
	{
		Dictionary step = steps[s];
		if ( s > 0 && bool( step.get( "timeout", false ) ) )
		{
			text += "timeout\n";
		}
		else if ( s > 0 )
		{
			String wait = String( step.get( "wait", "" ) ).strip_edges();
			if ( Plain( wait ) == false )
			{
				return fail( "then when \"" + wait + "\" cannot have a tab or a #" );
			}
			text += "step\t" + Num( double( step.get( "after", 0.0 ) ) ) + "\t" + Num( double( step.get( "within", 0.0 ) ) ) +
					( wait.is_empty() ? std::string() : "\t" + Std( wait ) ) + "\n";
		}
		String guard;
		Array statements = step.get( "statements", Array() );
		for ( int64_t i = 0; i < statements.size(); ++i )
		{
			Dictionary statement = statements[i];
			const String line = "line " + String::num_int64( int64_t( statement.get( "line", 0 ) ) ) + ": ";
			String under = String( statement.get( "guard", "" ) ).strip_edges();
			if ( Plain( under ) == false )
			{
				return fail( line + String( "if \"" ) + under + "\" cannot have a tab or a #" );
			}
			if ( under != guard )
			{
				text += under.is_empty() ? std::string( "guard\n" ) : "guard\t" + Std( under ) + "\n";
				guard = under;
			}
			const String what = statement.get( "kind", "" );
			if ( what == "change" )
			{
				String why;
				std::string changed = ChangeLine( statement["text"], why );
				if ( changed.empty() )
				{
					return fail( line + String( "change \"" ) + String( statement["text"] ) + "\": " + why );
				}
				text += changed;
			}
			else if ( what == "emit" )
			{
				String event = String( statement["text"] ).strip_edges();
				if ( Plain( event ) == false || event.contains( " " ) )
				{
					return fail( line + String( "emit names one event" ) );
				}
				text += "emit\t" + Std( event ) + "\n";
			}
			else if ( what == "end" )
			{
				text += "end\n";
			}
			else if ( what == "part" )
			{
				String why;
				std::string lines = BakeSheetPart( statement["part"], kind, from, root, why );
				if ( lines.empty() )
				{
					return fail( line + why );
				}
				text += lines;
			}
			else if ( what == "break" )
			{
				auto value = [&]( const char* key, const char* otherwise ) {
					String v = String( statement.get( key, otherwise ) ).strip_edges();
					return Plain( v ) ? Std( v ) : std::string();
				};
				std::string radius = value( "radius", "0" ), speed = value( "speed", "10" ), seconds = value( "seconds", "0" );
				if ( radius.empty() || speed.empty() || seconds.empty() )
				{
					return fail( line + String( "break: no tab or # in its numbers" ) );
				}
				text += "break\t" + radius + "\t" + speed + "\t" + seconds + "\n";
			}
			else if ( what == "remove" )
			{
				text += "remove\n";
			}
			else if ( what == "every" )
			{
				text += "every\t" + Num( double( statement["seconds"] ) ) + "\n";
			}
			else if ( what == "follow" )
			{
				text += "follow\t" + Std( String( statement["path"] ) ) + "\t" + Num( double( statement["seconds"] ) ) + "\n";
			}
			else if ( what == "cast" )
			{
				// probe <range> <speed>, what it may hold, its shape; then what it does while it holds
				// (on what it holds, unless it says), and its event when it takes hold
				String why;
				Dictionary shape = statement["shape"];
				std::string probeShape;
				if ( String( shape.get( "kind", "" ) ) == "sphere" )
				{
					probeShape = "probe_shape\t" + Num( double( shape["radius"] ) ) + "\t0\t0\t0\n";
				}
				else if ( String( shape.get( "kind", "" ) ) == "node" )
				{
					// One convex shape, around its own origin (the line's tip), forward -Z as drawn.
					const Node* drawn = from != nullptr ? from->get_node_or_null( NodePath( String( shape["path"] ) ) ) : nullptr;
					std::string pieces;
					if ( FindShapeLines( shape, from, drawn, pieces, why ) == false )
					{
						return fail( line + why.replace( "find:", "cast:" ) );
					}
					if ( pieces.find( "\nshape\t" ) != std::string::npos )
					{
						return fail( line + String( "cast: its shape is one piece (one CollisionShape3D)" ) );
					}
					probeShape = "probe_" + pieces;
				}
				text += "probe\t" + Num( double( statement.get( "range", 40.0 ) ) ) + "\t" + Num( double( statement.get( "speed", 0.0 ) ) ) + "\n";
				if ( String( statement.get( "along", "look" ) ) == "facing" )
				{
					text += "probe_frame\tfacing\n"; // level along the body's facing, not the camera's look
				}
				PackedStringArray holds = statement.get( "holds", PackedStringArray() );
				if ( holds.is_empty() == false && holds.size() < 3 )
				{
					text += "holds";
					for ( int64_t k = 0; k < holds.size(); ++k )
					{
						text += "\t" + Std( holds[k] );
					}
					text += "\n";
				}
				text += probeShape;
				Array inside = statement.get( "statements", Array() );
				for ( int64_t k = 0; k < inside.size(); ++k )
				{
					Dictionary each = inside[k];
					const String at = "line " + String::num_int64( int64_t( each.get( "line", 0 ) ) ) + ": ";
					if ( String( each.get( "kind", "" ) ) == "emit" )
					{
						String event = String( each["text"] ).strip_edges();
						if ( Plain( event ) == false || event.contains( " " ) )
						{
							return fail( at + String( "emit names one event" ) );
						}
						text += "hold_emit\t" + Std( event ) + "\n";
						continue;
					}
					std::string lines = BakeSheetPart( each["part"], kind, from, root, why );
					if ( lines.empty() )
					{
						return fail( at + why );
					}
					text += lines;
				}
			}
			else if ( what == "find" )
			{
				// find <who> <self> <at most> <frame>, its shape's pieces, what happens to each, endfind
				String why;
				std::string shape;
				if ( FindShapeLines( statement["shape"], from, root, shape, why ) == false )
				{
					return fail( line + why );
				}
				text += "find\t" + Std( String( statement.get( "who", "all" ) ) ) + "\t" + ( bool( statement.get( "self", false ) ) ? "1" : "0" ) + "\t" +
						std::to_string( int( statement.get( "most", 16 ) ) ) + "\t" + Std( String( statement.get( "frame", "facing" ) ) ) + "\n" + shape;
				Array inside = statement.get( "statements", Array() );
				for ( int64_t k = 0; k < inside.size(); ++k )
				{
					Dictionary each = inside[k];
					const String at = "line " + String::num_int64( int64_t( each.get( "line", 0 ) ) ) + ": ";
					if ( String( each.get( "kind", "" ) ) == "emit" )
					{
						String event = String( each["text"] ).strip_edges();
						if ( Plain( event ) == false || event.contains( " " ) )
						{
							return fail( at + String( "emit names one event" ) );
						}
						text += "emit\t" + Std( event ) + "\n";
						continue;
					}
					if ( String( each.get( "kind", "" ) ) == "change" )
					{
						// The owner's field, once for each thing found.
						std::string changed = ChangeLine( each["text"], why );
						if ( changed.empty() )
						{
							return fail( at + String( "change \"" ) + String( each["text"] ) + "\": " + why );
						}
						text += changed;
						continue;
					}
					std::string lines = BakeSheetPart( each["part"], kind, from, root, why );
					if ( lines.empty() )
					{
						return fail( at + why );
					}
					text += lines;
				}
				text += "endfind\n";
			}
		}
		if ( s == 0 && itemUse )
		{
			// The item's own changes and event, after the first step's (as a use of nodes has them).
			if ( guard.is_empty() == false )
			{
				text += "guard\n";
				guard = String();
			}
			PackedStringArray changes = root->get( "use_changes" );
			for ( int64_t k = 0; k < changes.size(); ++k )
			{
				if ( changes[k].strip_edges().is_empty() )
				{
					continue;
				}
				cue::Change change;
				String why;
				if ( cue::ParseChange( changes[k], change, &why ) == false || Plain( change.field ) == false )
				{
					return fail( "use_changes \"" + changes[k] + "\": " + why );
				}
				text += "change\t" + Std( change.field ) + "\t" + ( change.op == '=' ? std::string( "=" ) : std::string( 1, change.op ) + "=" ) + "\t" +
						Num( change.value ) + "\n";
			}
			const String emits = String( root->get( "use_emits" ) ).strip_edges();
			if ( emits.is_empty() == false )
			{
				if ( Plain( emits ) == false || emits.contains( " " ) )
				{
					return fail( "use_emits names one event" );
				}
				text += "emit\t" + Std( emits ) + "\n";
			}
		}
	}

	// What the simulation's own compiler says about it.
	std::vector<Motion> compiled;
	std::string compileError, ignored;
	if ( CompileMotionSet( "check", "cinderbox_motions\t3\n" + text, ModSchema{}, compiled, compileError, ignored ) == false )
	{
		size_t colon = compileError.find( ": " );
		std::string why = compileError.substr( colon == std::string::npos ? 0 : colon + 2 );
		std::string prefix = "check/" + Std( name ) + ": ";
		if ( why.rfind( prefix, 0 ) == 0 )
		{
			why = why.substr( prefix.size() );
		}
		return fail( String::utf8( why.c_str() ) );
	}
	return text;
}

bool BakeSheetMotions( const Node* node, const String& kind, std::string& lines, std::vector<std::string>& names, String& error )
{
	TypedArray<Node> sheets = node->find_children( "*", "CbRules", true, false );
	for ( int64_t s = 0; s < sheets.size(); ++s )
	{
		String file = Object::cast_to<Node>( sheets[s] )->get( "file" );
		if ( file.is_empty() || FileAccess::file_exists( file ) == false )
		{
			continue;
		}
		PackedStringArray problems;
		const String text = FileAccess::get_file_as_string( file );
		std::vector<cue::Rule> rules = cue::ParseRules( text, problems );
		// A sheet with motion blocks bakes only without a mistake: one it could not read would be a
		// motion silently left out.
		const bool hasMotions = text.begins_with( "motion " ) || text.contains( "\nmotion " ) || text.contains( "\nmotion\t" );
		if ( hasMotions && problems.is_empty() == false )
		{
			error = file + " " + problems[0];
			return false;
		}
		for ( const cue::Rule& rule : rules )
		{
			if ( rule.type != "motion" )
			{
				continue;
			}
			String why;
			std::string baked = BakeSheetMotion( rule.properties, kind, Object::cast_to<Node>( sheets[s] )->get_parent(), node, why );
			if ( baked.empty() )
			{
				// (A statement's mistake says its line; the rest are the motion's first line's.)
				error = file + ( why.begins_with( "line " ) ? " " + why : " line " + String::num_int64( rule.line ) + ": " + why );
				return false;
			}
			std::string name = Std( String( rule.properties["name"] ) );
			if ( std::find( names.begin(), names.end(), name ) != names.end() )
			{
				error = file + " line " + String::num_int64( rule.line ) + ": there is a motion named " + String( name.c_str() ) + " already";
				return false;
			}
			names.push_back( name );
			lines += baked;
		}
	}
	return true;
}


} // namespace cb::gd
