#include "sheet_bake.h"

#include "expr.h"
#include "mod_schema.h"
#include "motions.h"
#include "move_params.h"
#include "sheet_rules.h"
#include "sheet_text.h"

#include <algorithm>

#include <cmath>
#include <functional>

// Moved from src/godot/sheet_motions.cpp in M283, line for line, onto plain types: the vector
// arithmetic is Godot's Vector3's (float, in the same order), so every baked number is the same.

namespace cb::sheet
{

namespace
{

Float3 Add( Float3 a, Float3 b )
{
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

Float3 Sub( Float3 a, Float3 b )
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

Float3 Neg( Float3 a )
{
	return { -a.x, -a.y, -a.z };
}

float Length( Float3 v )
{
	return std::sqrt( v.x * v.x + v.y * v.y + v.z * v.z );
}

// Godot's Vector3::normalized.
Float3 Normalized( Float3 v )
{
	const float lengthsq = v.x * v.x + v.y * v.y + v.z * v.z;
	if ( lengthsq == 0.0f )
	{
		return {};
	}
	const float length = std::sqrt( lengthsq );
	return { v.x / length, v.y / length, v.z / length };
}

// A point of the scene, in the simulation's frame (forward +Z): Godot's forward is -Z.
Float3 SimPoint( Float3 p )
{
	return { 0.0f - p.x, p.y, 0.0f - p.z }; // (0 - 0 is 0, where -0 would print "-0")
}

// A point of a shape in the player's frame, as the simulation has it: Godot's forward is -Z, the
// simulation's +Z (the same turn about Y by half a circle).
std::string PointWords( Float3 p )
{
	return "\t" + Num( 0.0f - p.x ) + "\t" + Num( p.y ) + "\t" + Num( 0.0f - p.z ); // (0 - 0 is 0, where -0 would print "-0")
}

bool Plain( const std::string& text )
{
	return MotionTextPlain( text );
}

// "param" lines of a parameters dictionary, in the simulation's order; "" with `error` for a name
// that is not a movement parameter.
std::string ParamLines( const Dict& parameters, std::string& error )
{
	const Strings names = parameters.Keys();
	for ( const std::string& name : names )
	{
		if ( MoveParamByName( Strip( name ).c_str() ) < 0 )
		{
			error = "parameters: \"" + name + "\" is not a movement parameter";
			return std::string();
		}
	}
	std::string text;
	for ( int p = 0; p < kMoveParams; ++p )
	{
		for ( const std::string& name : names )
		{
			if ( MoveParamByName( Strip( name ).c_str() ) == p )
			{
				// A number, or an expression (a sheet's: read when its step starts).
				const Value value = parameters.Get( name, Value() );
				if ( value.type() == Value::Type::String && Plain( value.AsString() ) == false )
				{
					error = "parameters: \"" + value.AsString() + "\" cannot have a tab or a #";
					return std::string();
				}
				text += std::string( "param\t" ) + MoveParamInfoOf( p ).name + "\t" +
						( value.type() == Value::Type::String ? Strip( value.AsString() ) : Num( value.AsFloat() ) ) + "\n";
			}
		}
	}
	return text;
}

// Several conditions as one expression: all of them (" and "), or any of them (" or ").
bool Joined( const Strings& conditions, const char* with, std::string& out, std::string& bad )
{
	out.clear();
	for ( const std::string& raw : conditions )
	{
		const std::string condition = Strip( raw );
		if ( condition.empty() )
		{
			continue;
		}
		if ( Plain( condition ) == false )
		{
			bad = condition;
			return false;
		}
		out += std::string( out.empty() ? "" : with ) + "( " + condition + " )";
	}
	return true;
}

const char* const kFrames[] = { "look", "move", "facing", "up", "world", "to", "away" };
const char* const kReplaces[] = { "none", "vertical", "horizontal", "all" };
const char* const kPushes[] = { "accel", "force", "velocity" };

// A statement's impulse, force, link or launch (its "class" and properties) as its line. Until
// M268 each was the motion node of that class, made for the bake; these are the nodes' defaults for
// what the statement leaves out, and their lines word for word.
std::string PartLine( const std::string& cls, const Dict& p, std::string& why )
{
	auto number = [&]( const char* key, double fallback ) { return p.Get( key, fallback ).AsFloat(); };
	auto whole = [&]( const char* key, int fallback ) { return int( p.Get( key, fallback ).AsInt() ); };
	auto direction = [&]() {
		const Value given = p.Get( "direction", Float3{ 0.0f, 1.0f, 0.0f } );
		const Float3 d = Normalized( given.type() == Value::Type::Vec3 ? given.AsVec3() : Float3{} );
		return "\t" + Num( d.x ) + "\t" + Num( d.y ) + "\t" + Num( d.z );
	};
	if ( cls == "CbLaunch" )
	{
		const std::string kind = Strip( p.Get( "item_kind", "" ).AsString() );
		if ( kind.empty() || Plain( kind ) == false || Contains( kind, " " ) )
		{
			why = "launch: name the item kind it throws (one a server mod declares, and a CbItem bakes: grenade.shell)";
			return std::string();
		}
		const int frame = whole( "frame", 0 ); // (look, move, facing, up, world)
		std::string line = std::string( whole( "what", 0 ) == 1 ? "launch_carried\t" : "launch\t" ) + kind + "\t" + Num( number( "speed", 15.0 ) ) + "\t" +
						   kFrames[frame >= 0 && frame <= 4 ? frame : 0] + "\t" + Num( number( "lift", 0.0 ) ) + "\t" + Num( number( "ahead", 0.7 ) ) + "\t" +
						   Num( number( "seconds", 5.0 ) );
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
		const std::string field = Strip( p.Get( "target_field", "" ).AsString() );
		if ( target != 2 || field.empty() || Plain( field ) == false || Contains( field, " " ) )
		{
			why = "name the field whose entity it acts on (one the server mod writes an entity's id into)";
			return std::string();
		}
		on = "@" + field;
	}
	if ( link )
	{
		if ( target == 0 )
		{
			why = "a rope goes to something else: what the cast holds, or the entity a field names";
			return std::string();
		}
		// (A rope that reacts, the default, bakes as it always did.)
		return "link\t" + on + "\t" + Num( number( "length", 0.0 ) ) + "\t" + Num( number( "reel", 0.0 ) ) + ( p.Get( "react", true ).AsBool() ? "" : "\t0" ) + "\n";
	}
	const int frame = whole( "frame", 1 ); // (look, move, facing, up, world, to, away)
	const std::string along = kFrames[frame >= 0 && frame <= 6 ? frame : 1];
	const std::string world = frame == 4 ? direction() : std::string();
	if ( impulse )
	{
		const int replace = whole( "replace", 0 );
		return "impulse\t" + on + "\t" + Num( number( "speed", 0.0 ) ) + "\t" + along + "\t" + kReplaces[replace >= 0 && replace <= 3 ? replace : 0] + world + "\n";
	}
	const int push = whole( "kind", 0 );
	return "force\t" + on + "\t" + along + "\t" + kPushes[push >= 0 && push <= 2 ? push : 0] + "\t" + Num( number( "strength", 0.0 ) ) + "\t" + Num( number( "speed", 0.0 ) ) +
		   "\t" + Num( number( "ramp_in", 0.0 ) ) + "\t" + ( p.Get( "react", false ).AsBool() ? "1" : "0" ) + world + "\n";
}

// A sheet's "change FIELD OP VALUE" as a "change" line: its value a number, or an expression (the
// rest of the line). "" with why when it cannot be read.
std::string ChangeLine( const std::string& text, std::string& why )
{
	Change change;
	if ( ParseChange( text, change, &why ) && Plain( change.field ) )
	{
		return "change\t" + change.field + "\t" + ( change.op == '=' ? std::string( "=" ) : std::string( 1, change.op ) + "=" ) + "\t" + Num( change.value ) + "\n";
	}
	const std::string trimmed = Strip( text );
	const std::vector<std::string> words = Split( trimmed, " ", false );
	if ( words.size() < 3 || ( words[1] != "=" && words[1] != "+=" && words[1] != "-=" ) || Plain( trimmed ) == false )
	{
		why = "change FIELD = VALUE, += or -= (a number or an expression)";
		return std::string();
	}
	const size_t at = trimmed.find( " " + words[1] + " " );
	const std::string value = Strip( trimmed.substr( at == std::string::npos ? 0 : at + words[1].size() + 2 ) );
	why.clear();
	return "change\t" + words[0] + "\t" + words[1] + "\t" + value + "\n";
}

std::string BakeSheetPart( const Dict& part, const std::string& kind, const Scene& scene, std::string& why )
{
	Dict props = part.Get( "properties", Dict() ).AsDict().Duplicate();
	// Along a marker: its forward in the scene, written as a world direction and named "marker" after.
	const std::string marker = props.Get( "marker", "" ).AsString();
	if ( marker.empty() == false )
	{
		Float3 zAxis, origin;
		if ( scene.Place( marker, "along", zAxis, origin, why ) == false )
		{
			return std::string();
		}
		const Float3 forward = Normalized( SimPoint( Neg( zAxis ) ) );
		if ( Length( forward ) < 0.5f )
		{
			why = "along " + marker + ": it has no forward (its scale is 0)";
			return std::string();
		}
		props["frame"] = 4;
		props["direction"] = forward;
	}
	const std::string cls = part.Get( "class", "" ).AsString();
	if ( cls == "CbMove" )
	{
		// Parameters from when the step starts: for its seconds, or while it is the step.
		const std::string seconds = props.Get( "duration", 0.0 ).AsFloat() > 0.0 ? "\t" + Num( props.Get( "duration", 0.0 ).AsFloat() ) : std::string();
		std::string params = ParamLines( props.Get( "parameters", Dict() ).AsDict(), why );
		if ( params.empty() )
		{
			why = why.empty() ? std::string( "move wants a parameter and a value" ) : why;
			return std::string();
		}
		for ( size_t at = 0; ( at = params.find( '\n', at ) ) != std::string::npos; at += seconds.size() + 1 )
		{
			params.insert( at, seconds );
		}
		return params;
	}
	if ( cls == "CbLaunch" && props.Get( "what", 0 ).AsInt() == 1 && props.Has( "item_kind" ) == false )
	{
		props["item_kind"] = kind; // (the one carried: the item's own)
	}
	// (Its direction is the simulation's already: written as it is, normalized.)
	std::string lines = PartLine( cls, props, why );
	// The amounts that are expressions, each in its number's place in the first line (by its field:
	// an impulse's speed 2; a force's strength 4 and top 5; a link's length 2 and reel 3; a launch's
	// speed 2, lift 4, ahead 5, seconds 6).
	const Dict amounts = props.Get( "amounts", Dict() ).AsDict();
	for ( const auto& [key, value] : amounts.Entries() )
	{
		if ( lines.empty() )
		{
			break;
		}
		const std::string expression = Strip( value.AsString() );
		if ( Plain( expression ) == false )
		{
			why = "\"" + expression + "\" cannot have a tab or a #";
			return std::string();
		}
		const int field = int( ToInt( key ) );
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
		const size_t end = lines.find_first_of( "\t\n", at );
		lines.replace( at, end - at, expression );
	}
	if ( lines.empty() )
	{
		return lines;
	}
	if ( props.Get( "momentum", false ).AsBool() && lines.rfind( "impulse\t", 0 ) == 0 )
	{
		lines.replace( 0, 7, "impulse_momentum" ); // (its number N s, not m/s)
	}
	if ( marker.empty() == false )
	{
		// The frame's word: "world" becomes "marker" (a force has it a field earlier).
		const int field = cls == "CbForce" ? 2 : 3;
		size_t at = 0;
		for ( int k = 0; k < field; ++k )
		{
			at = lines.find( '\t', at ) + 1;
		}
		lines.replace( at, 5, "marker" );
	}
	if ( props.Has( "turn" ) )
	{
		const Float2 turn = props.Get( "turn", Value() ).AsVec2();
		if ( turn.y < -90.0f || turn.y > 90.0f || turn.x < -360.0f || turn.x > 360.0f )
		{
			why = "turn (yaw, pitch): a heading from -360 to 360 degrees, a pitch from -90 to 90";
			return std::string();
		}
		lines += "turn\t" + Num( turn.x ) + "\t" + Num( turn.y ) + "\n";
	}
	const std::string start = props.Get( "from", "" ).AsString();
	if ( start.empty() == false )
	{
		Float3 zAxis, origin;
		if ( scene.Place( start, "from", zAxis, origin, why ) == false )
		{
			return std::string();
		}
		const Float3 at = SimPoint( origin );
		lines += "from\t" + Num( at.x ) + "\t" + Num( at.y ) + "\t" + Num( at.z ) + "\n";
	}
	return lines;
}

// The paths a motion's follows carry the player along (Path3D nodes beside the sheet): "path" lines,
// points at even steps along each from where it starts, in the simulation's frame.
bool PathLines( const std::vector<std::string>& names, const Scene& scene, std::string& out, std::string& why )
{
	for ( const std::string& name : names )
	{
		std::vector<Float3> samples;
		if ( scene.PathSamples( name, samples, why ) == false || samples.size() != size_t( kPathSamples ) )
		{
			why = why.empty() ? "follow " + name + ": no Path3D of that name, with two points at least, beside the sheet" : why;
			return false;
		}
		const Float3 first = samples[0];
		out += "path\t" + name;
		for ( const Float3& sample : samples )
		{
			const Float3 at = SimPoint( Sub( sample, first ) );
			out += "\t" + Num( at.x ) + "\t" + Num( at.y ) + "\t" + Num( at.z );
		}
		out += "\n";
	}
	return true;
}

// The curves a motion's expressions read ("curve(Falloff, reach)"): none (M268). They were drawn in
// CbCurve nodes beside the sheet, deleted with the motion nodes; the simulation still reads a
// motions file's "curve" lines (ROADMAP, postponed: curves as resources).
bool NoCurves( const std::vector<std::string>& expressions, std::string& why )
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
			why = "curve(" + program.curves[0] + ", ...): a sheet has no curves (yet)";
			return false;
		}
	}
	return true;
}

// A find's (or a cast's) shape as "shape" lines. With `own`, a node's shape in its own frame.
bool FindShapeLines( const Dict& shape, const Scene& scene, bool own, std::string& out, std::string& why )
{
	const std::string kind = shape.Get( "kind", "" ).AsString();
	// Every shape is placed from the player's feet, as if it stood at the scene's origin facing -Z:
	// one drawn where it is drawn, one written in the line around the feet; "at (x, y, z)" moves
	// either, metres right, up and ahead.
	const Float3 at = shape.Get( "at", Float3{} ).AsVec3();
	const Float3 moved{ at.x, at.y, 0.0f - at.z };
	if ( kind == "sphere" )
	{
		out = "shape\t" + Num( shape.Get( "radius", 0.0 ).AsFloat() ) + PointWords( moved ) + "\n";
		return true;
	}
	if ( kind == "capsule" )
	{
		const double r = shape.Get( "radius", 0.0 ).AsFloat(), h = shape.Get( "height", 0.0 ).AsFloat();
		out = "shape\t" + Num( r ) + PointWords( Add( moved, Float3{ 0.0f, float( h / 2.0 - r ), 0.0f } ) ) +
			  PointWords( Add( moved, Float3{ 0.0f, float( r - h / 2.0 ), 0.0f } ) ) + "\n";
		return true;
	}
	if ( kind == "box" )
	{
		const Float3 size = shape.Get( "size", Float3{} ).AsVec3();
		const Float3 half{ std::fabs( size.x ) / 2.0f, std::fabs( size.y ) / 2.0f, std::fabs( size.z ) / 2.0f };
		out = "shape\t0";
		for ( int c = 0; c < 8; ++c )
		{
			out += PointWords( Add( moved, Float3{ c & 1 ? half.x : -half.x, c & 2 ? half.y : -half.y, c & 4 ? half.z : -half.z } ) );
		}
		out += "\n";
		return true;
	}
	// A node of the scene: an Area3D (its CollisionShape3D children) or a CollisionShape3D, drawn
	// around the scene's origin as if the player stood there facing -Z.
	const std::string path = shape.Get( "path", "" ).AsString();
	std::vector<ShapePiece> pieces;
	if ( scene.Shapes( path, own, moved, pieces, why ) == false )
	{
		return false;
	}
	out.clear();
	for ( const ShapePiece& piece : pieces )
	{
		out += "shape\t" + Num( piece.radius );
		for ( const Float3& p : piece.points )
		{
			out += PointWords( p );
		}
		out += "\n";
	}
	if ( out.empty() )
	{
		why = "find: " + path + " has no shape that is on";
		return false;
	}
	return true;
}

} // namespace

std::string BakeSheetMotion( const Dict& p, const std::string& kind, const Scene& scene, std::string& error )
{
	auto fail = [&]( const std::string& why ) {
		error = why;
		return std::string();
	};
	const std::string name = p.Get( "name", "" ).AsString();
	if ( name.empty() || Plain( name ) == false || Contains( name, " " ) )
	{
		return fail( "a motion's name is one word" );
	}
	std::string text = "motion\t" + name + "\n";
	{
		// Every expression it has, for the curves they read (a sheet has none: NoCurves).
		std::vector<std::string> expressions;
		auto add = [&]( const Value& v ) {
			if ( v.type() == Value::Type::Strings )
			{
				for ( const std::string& one : v.AsStrings() )
				{
					expressions.push_back( one );
				}
			}
			else if ( v.type() == Value::Type::String )
			{
				expressions.push_back( v.AsString() );
			}
		};
		add( p.Get( "expression", Value() ) );
		add( p.Get( "conditions", Value() ) );
		add( p.Get( "until", Value() ) );
		std::function<void( const List& )> statementsOf = [&]( const List& list ) {
			for ( size_t k = 0; k < list.size(); ++k )
			{
				const Dict statement = list[k].AsDict();
				add( statement.Get( "guard", Value() ) );
				// Its amounts that are expressions: a part's, a move's values, a change's, a break's.
				const Dict props = statement.Get( "part", Dict() ).AsDict().Get( "properties", Dict() ).AsDict();
				const Dict amounts = props.Get( "amounts", Dict() ).AsDict();
				for ( const auto& entry : amounts.Entries() )
				{
					add( entry.second );
				}
				const Dict parameters = props.Get( "parameters", Dict() ).AsDict();
				for ( const auto& entry : parameters.Entries() )
				{
					add( entry.second );
				}
				for ( const char* key : { "radius", "speed", "seconds" } )
				{
					add( statement.Get( key, Value() ) );
				}
				if ( statement.Get( "kind", "" ).AsString() == "change" )
				{
					// (Its value: after the operator.)
					const std::vector<std::string> words = Split( Strip( statement.Get( "text", "" ).AsString() ), " ", false );
					std::string rest;
					for ( size_t w = 2; w < words.size(); ++w )
					{
						rest += ( w > 2 ? " " : "" ) + words[w];
					}
					add( rest );
				}
				statementsOf( statement.Get( "statements", List() ).AsList() );
			}
		};
		const List all = p.Get( "steps", List() ).AsList();
		for ( size_t k = 0; k < all.size(); ++k )
		{
			add( all[k].AsDict().Get( "wait", Value() ) );
			statementsOf( all[k].AsDict().Get( "statements", List() ).AsList() );
		}
		std::string why;
		if ( NoCurves( expressions, why ) == false )
		{
			return fail( why );
		}
		// The paths its follows name, the same way.
		std::vector<std::string> paths;
		for ( size_t k = 0; k < all.size(); ++k )
		{
			const List list = all[k].AsDict().Get( "statements", List() ).AsList();
			for ( size_t i = 0; i < list.size(); ++i )
			{
				const Dict statement = list[i].AsDict();
				const std::string path = statement.Get( "path", "" ).AsString();
				if ( statement.Get( "kind", "" ).AsString() == "follow" && std::find( paths.begin(), paths.end(), path ) == paths.end() )
				{
					paths.push_back( path );
				}
			}
		}
		std::string pathLines;
		if ( PathLines( paths, scene, pathLines, why ) == false )
		{
			return fail( why );
		}
		text += pathLines;
	}
	const std::string trigger = p.Get( "trigger", "" ).AsString();
	Strings conditions;
	auto word = [&]( const char* key ) {
		const std::string w = Strip( p.Get( key, "" ).AsString() );
		return Plain( w ) && Contains( w, " " ) == false ? w : std::string();
	};
	const bool ownerItem = p.Get( "owner", "player" ).AsString() == "item";
	if ( ownerItem && kind.empty() )
	{
		return fail( "owner item is for an item's sheet" );
	}
	if ( ownerItem && trigger != "event" )
	{
		return fail( "an item's motion (owner item) starts on an event said at it: on expired, or on EVENT" );
	}
	// The item's own "When used" fields (its CbItem's use_conditions, use_changes, use_cooldown,
	// use_emits) join its motion named Use, as they join a use made of nodes.
	const bool itemUse = trigger == "use" && name == "Use" && scene.IsItem();
	if ( trigger == "use" )
	{
		if ( kind.empty() )
		{
			return fail( "on use is for an item's sheet (a motion set's motions are everyone's)" );
		}
		// An item used while its key is held (use: a press, and while held) is in use while it is held:
		// its use motions are on for as long, as a use made of nodes is.
		if ( scene.IsItem() && scene.ItemUse() == kItemUseHold )
		{
			text += "when\twhile\n";
			conditions.push_back( "using." + kind );
		}
		else
		{
			text += "when\tevent\t" + kind + ".used\n";
		}
		if ( itemUse )
		{
			const Strings own = scene.UseConditions();
			conditions.insert( conditions.end(), own.begin(), own.end() );
		}
	}
	else if ( trigger == "press" )
	{
		if ( word( "action" ).empty() )
		{
			return fail( "on press wants one action" );
		}
		text += "when\tpress\t" + word( "action" ) + "\n";
	}
	else if ( trigger == "event" )
	{
		if ( word( "event" ).empty() )
		{
			return fail( "on wants one event" );
		}
		// "expired": the item's own time being up ("<kind>.expired", said by the simulation).
		const std::string event = word( "event" ) == "expired" && kind.empty() == false ? kind + ".expired" : word( "event" );
		text += "when\tevent\t" + event + "\n";
	}
	else if ( trigger == "while" || trigger == "when" )
	{
		const std::string expression = Strip( p.Get( "expression", "" ).AsString() );
		if ( expression.empty() || Plain( expression ) == false )
		{
			return fail( trigger + " wants an expression (with no tab or #)" );
		}
		if ( trigger == "while" )
		{
			text += "when\twhile\n";
			conditions.push_back( expression );
		}
		else
		{
			text += "when\tedge\t" + expression + "\n";
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
	if ( kind.empty() == false && trigger != "use" && ownerItem == false )
	{
		conditions.push_back( p.Get( "item", "hand" ).AsString() == "carried" ? "carried." + kind : kind );
	}
	{
		const Strings own = p.Get( "conditions", Strings() ).AsStrings();
		conditions.insert( conditions.end(), own.begin(), own.end() );
	}
	std::string all, any, bad;
	if ( Joined( conditions, " and ", all, bad ) == false || Joined( p.Get( "until", Strings() ).AsStrings(), " or ", any, bad ) == false )
	{
		return fail( "\"" + bad + "\" cannot have a tab or a #" );
	}
	if ( all.empty() == false )
	{
		text += "if\t" + all + "\n";
	}
	if ( any.empty() == false )
	{
		text += "until\t" + any + "\n";
	}
	const double cooldown = p.Has( "cooldown" ) ? p.Get( "cooldown", 0.0 ).AsFloat() : itemUse ? scene.UseCooldown() : 0.0;
	if ( cooldown > 0.0 )
	{
		text += "cooldown\t" + Num( cooldown ) + "\n";
	}
	if ( p.Get( "uses", 0 ).AsInt() > 0 )
	{
		const Value refill = p.Get( "refill", 0.0 );
		text += "uses\t" + std::to_string( int( p.Get( "uses", 0 ).AsInt() ) ) + "\t" +
				( refill.type() == Value::Type::String ? refill.AsString() : Num( refill.AsFloat() ) ) + "\n";
	}
	if ( p.Get( "lasts", 0.0 ).AsFloat() > 0.0 )
	{
		text += "duration\t" + Num( p.Get( "lasts", 0.0 ).AsFloat() ) + "\n";
	}

	const List steps = p.Get( "steps", List() ).AsList();
	// With a cast, what the first step does to bodies waits for it to take hold: so it goes under it.
	if ( steps.empty() == false )
	{
		const List first = steps[0].AsDict().Get( "statements", List() ).AsList();
		bool casts = false;
		int64_t loose = -1;
		for ( size_t i = 0; i < first.size(); ++i )
		{
			const Dict statement = first[i].AsDict();
			casts = casts || statement.Get( "kind", "" ).AsString() == "cast";
			loose = loose < 0 && statement.Get( "kind", "" ).AsString() == "part" ? statement.Get( "line", 0 ).AsInt() : loose;
		}
		if ( casts && loose >= 0 )
		{
			return fail( "line " + std::to_string( loose ) + ": with a cast, what the motion's first step does to bodies goes under the cast (it happens while it holds)" );
		}
	}
	for ( size_t s = 0; s < steps.size(); ++s )
	{
		const Dict step = steps[s].AsDict();
		if ( s > 0 && step.Get( "timeout", false ).AsBool() )
		{
			text += "timeout\n";
		}
		else if ( s > 0 )
		{
			const std::string wait = Strip( step.Get( "wait", "" ).AsString() );
			if ( Plain( wait ) == false )
			{
				return fail( "then when \"" + wait + "\" cannot have a tab or a #" );
			}
			text += "step\t" + Num( step.Get( "after", 0.0 ).AsFloat() ) + "\t" + Num( step.Get( "within", 0.0 ).AsFloat() ) +
					( wait.empty() ? std::string() : "\t" + wait ) + "\n";
			if ( step.Get( "or", false ).AsBool() )
			{
				text += "goon\n";
			}
		}
		std::string guard;
		const List statements = step.Get( "statements", List() ).AsList();
		for ( size_t i = 0; i < statements.size(); ++i )
		{
			const Dict statement = statements[i].AsDict();
			const std::string line = "line " + std::to_string( statement.Get( "line", 0 ).AsInt() ) + ": ";
			const std::string under = Strip( statement.Get( "guard", "" ).AsString() );
			if ( Plain( under ) == false )
			{
				return fail( line + "if \"" + under + "\" cannot have a tab or a #" );
			}
			if ( under != guard )
			{
				text += under.empty() ? std::string( "guard\n" ) : "guard\t" + under + "\n";
				guard = under;
			}
			const std::string what = statement.Get( "kind", "" ).AsString();
			if ( what == "change" )
			{
				std::string why;
				const std::string changed = ChangeLine( statement.Get( "text", "" ).AsString(), why );
				if ( changed.empty() )
				{
					return fail( line + "change \"" + statement.Get( "text", "" ).AsString() + "\": " + why );
				}
				text += changed;
			}
			else if ( what == "emit" )
			{
				const std::string event = Strip( statement.Get( "text", "" ).AsString() );
				if ( Plain( event ) == false || Contains( event, " " ) )
				{
					return fail( line + "emit names one event" );
				}
				const std::string value = Strip( statement.Get( "value", "" ).AsString() );
				if ( Plain( value ) == false )
				{
					return fail( line + "an emit's value cannot have a tab or a #" );
				}
				// (An expression of several words is one word in brackets.)
				text += "emit\t" + event + ( value.empty() ? std::string() : "\t" + ( Contains( value, " " ) ? "(" + value + ")" : value ) ) + "\n";
			}
			else if ( what == "end" )
			{
				text += "end\n";
			}
			else if ( what == "part" )
			{
				std::string why;
				const std::string lines = BakeSheetPart( statement.Get( "part", Dict() ).AsDict(), kind, scene, why );
				if ( lines.empty() )
				{
					return fail( line + why );
				}
				text += lines;
			}
			else if ( what == "break" )
			{
				auto value = [&]( const char* key, const char* otherwise ) {
					const Value given = statement.Get( key, otherwise );
					const std::string v = Strip( given.AsString() );
					return Plain( v ) ? v : std::string();
				};
				const std::string radius = value( "radius", "0" ), speed = value( "speed", "10" ), seconds = value( "seconds", "0" );
				if ( radius.empty() || speed.empty() || seconds.empty() )
				{
					return fail( line + "break: no tab or # in its numbers" );
				}
				text += "break\t" + radius + "\t" + speed + "\t" + seconds + "\n";
			}
			else if ( what == "remove" )
			{
				text += "remove\n";
			}
			else if ( what == "every" )
			{
				text += "every\t" + Num( statement.Get( "seconds", 0.0 ).AsFloat() ) + "\n";
			}
			else if ( what == "follow" )
			{
				text += "follow\t" + statement.Get( "path", "" ).AsString() + "\t" + Num( statement.Get( "seconds", 0.0 ).AsFloat() ) + "\n";
			}
			else if ( what == "cast" )
			{
				// probe <range> <speed>, what it may hold, its shape; then what it does while it holds
				// (on what it holds, unless it says), and its event when it takes hold
				std::string why;
				const Dict shape = statement.Get( "shape", Dict() ).AsDict();
				std::string probeShape;
				if ( shape.Get( "kind", "" ).AsString() == "sphere" )
				{
					probeShape = "probe_shape\t" + Num( shape.Get( "radius", 0.0 ).AsFloat() ) + "\t0\t0\t0\n";
				}
				else if ( shape.Get( "kind", "" ).AsString() == "node" )
				{
					// One convex shape, around its own origin (the line's tip), forward -Z as drawn.
					std::string pieces;
					if ( FindShapeLines( shape, scene, true, pieces, why ) == false )
					{
						return fail( line + Replace( why, "find:", "cast:" ) );
					}
					if ( pieces.find( "\nshape\t" ) != std::string::npos )
					{
						return fail( line + "cast: its shape is one piece (one CollisionShape3D)" );
					}
					probeShape = "probe_" + pieces;
				}
				text += "probe\t" + Num( statement.Get( "range", 40.0 ).AsFloat() ) + "\t" + Num( statement.Get( "speed", 0.0 ).AsFloat() ) + "\n";
				if ( statement.Get( "along", "look" ).AsString() == "facing" )
				{
					text += "probe_frame\tfacing\n"; // level along the body's facing, not the camera's look
				}
				const Strings holds = statement.Get( "holds", Strings() ).AsStrings();
				if ( holds.empty() == false && holds.size() < 3 )
				{
					text += "holds";
					for ( const std::string& hold : holds )
					{
						text += "\t" + hold;
					}
					text += "\n";
				}
				text += probeShape;
				const List inside = statement.Get( "statements", List() ).AsList();
				for ( size_t k = 0; k < inside.size(); ++k )
				{
					const Dict each = inside[k].AsDict();
					const std::string at = "line " + std::to_string( each.Get( "line", 0 ).AsInt() ) + ": ";
					if ( each.Get( "kind", "" ).AsString() == "emit" )
					{
						const std::string event = Strip( each.Get( "text", "" ).AsString() );
						if ( Plain( event ) == false || Contains( event, " " ) )
						{
							return fail( at + "emit names one event" );
						}
						text += "hold_emit\t" + event + "\n";
						continue;
					}
					const std::string lines = BakeSheetPart( each.Get( "part", Dict() ).AsDict(), kind, scene, why );
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
				std::string why;
				std::string shape;
				if ( FindShapeLines( statement.Get( "shape", Dict() ).AsDict(), scene, false, shape, why ) == false )
				{
					return fail( line + why );
				}
				text += "find\t" + statement.Get( "who", "all" ).AsString() + "\t" + ( statement.Get( "self", false ).AsBool() ? "1" : "0" ) + "\t" +
						std::to_string( int( statement.Get( "most", 16 ).AsInt() ) ) + "\t" + statement.Get( "frame", "facing" ).AsString() + "\n" + shape;
				const List inside = statement.Get( "statements", List() ).AsList();
				for ( size_t k = 0; k < inside.size(); ++k )
				{
					const Dict each = inside[k].AsDict();
					const std::string at = "line " + std::to_string( each.Get( "line", 0 ).AsInt() ) + ": ";
					if ( each.Get( "kind", "" ).AsString() == "emit" )
					{
						const std::string event = Strip( each.Get( "text", "" ).AsString() );
						if ( Plain( event ) == false || Contains( event, " " ) )
						{
							return fail( at + "emit names one event" );
						}
						text += "emit\t" + event + "\n";
						continue;
					}
					if ( each.Get( "kind", "" ).AsString() == "change" )
					{
						// The owner's field, once for each thing found.
						const std::string changed = ChangeLine( each.Get( "text", "" ).AsString(), why );
						if ( changed.empty() )
						{
							return fail( at + "change \"" + each.Get( "text", "" ).AsString() + "\": " + why );
						}
						text += changed;
						continue;
					}
					const std::string lines = BakeSheetPart( each.Get( "part", Dict() ).AsDict(), kind, scene, why );
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
			if ( guard.empty() == false )
			{
				text += "guard\n";
				guard.clear();
			}
			for ( const std::string& raw : scene.UseChanges() )
			{
				if ( Strip( raw ).empty() )
				{
					continue;
				}
				Change change;
				std::string why;
				if ( ParseChange( raw, change, &why ) == false || Plain( change.field ) == false )
				{
					return fail( "use_changes \"" + raw + "\": " + why );
				}
				text += "change\t" + change.field + "\t" + ( change.op == '=' ? std::string( "=" ) : std::string( 1, change.op ) + "=" ) + "\t" + Num( change.value ) + "\n";
			}
			const std::string emits = Strip( scene.UseEmits() );
			if ( emits.empty() == false )
			{
				if ( Plain( emits ) == false || Contains( emits, " " ) )
				{
					return fail( "use_emits names one event" );
				}
				text += "emit\t" + emits + "\n";
			}
		}
	}

	// What the simulation's own compiler says about it.
	std::vector<Motion> compiled;
	std::string compileError, ignored;
	if ( CompileMotionSet( "check", "cinderbox_motions\t3\n" + text, ModSchema{}, compiled, compileError, ignored ) == false )
	{
		const size_t colon = compileError.find( ": " );
		std::string why = compileError.substr( colon == std::string::npos ? 0 : colon + 2 );
		const std::string prefix = "check/" + name + ": ";
		if ( why.rfind( prefix, 0 ) == 0 )
		{
			why = why.substr( prefix.size() );
		}
		return fail( why );
	}
	return text;
}

} // namespace cb::sheet
