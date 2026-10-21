#include "cinderbox_motion.h"

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
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <functional>
#include <charconv>
#include <vector>

using namespace godot;

namespace cb::gd
{

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

// What a line of the file cannot hold.
bool Plain( const String& text )
{
	return text.contains( "\t" ) == false && text.contains( "\n" ) == false && text.contains( "#" ) == false;
}

// Several conditions as one expression: all of them (" and "), or any of them (" or ").
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
				text += std::string( "param\t" ) + MoveParamInfoOf( p ).name + "\t" + Num( double( parameters[names[i]] ) ) + "\n";
			}
		}
	}
	return text;
}

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

struct Info
{
	const char* key;
	const char* brief;
	const char* detail;
};

const char* const kTargetDetail =
	"[b]target[/b]: who it acts on. The player is the one the motion runs for. What the probe found is what the "
	"motion's CbProbe holds on to: a prop, another player; the world cannot be moved. The entity a field names is the "
	"one whose id a server mod wrote into [b]target_field[/b] (bind.partner): who the player is bound to.\n"
	"[b]frame[/b]: the direction, always as the player has it. Look is where the camera looks, pitch included. Move "
	"input is where WASD points (the facing when nothing is held). Facing is where the body faces. Up. World "
	"direction is [b]direction[/b] as given. Toward the target is from the player to the target, or for an effect on "
	"the player itself, to what the probe found.";

// One per group of each node, and one about the node ("").
const Info kMotionInfo[] = {
	{ "", "A motion: when it happens and for how long. Its children are what it does (CbProbe, CbImpulse, CbForce, CbLink, CbLaunch).",
	  "[b]Where[/b]: under a CbMotionSet, in a scene of the mod's project (motion_sets/). Saving the scene bakes the set.\n"
	  "[b]Who runs it[/b]: every simulation, the server's and each client's, from the player's input. So your own "
	  "dash starts on the tick of the press, and other players see the same dash.\n"
	  "[b]The server mod[/b] declares the action, the fields and the events named here, and names the set: "
	  "declare.Motions( \"dash.moves\" ). Who may use a motion is a field its conditions read.\n"
	  "[b]A reaction[/b] on the event it emits shows it; that needs no CbPrediction." },
	{ "When", "What it waits for, and what has to hold.",
	  "[b]when[/b]: On a press happens once, when the action goes down (a dash). While is on every tick its "
	  "conditions hold (flight, a glide). On a cue happens when a mod event is recorded at the player: a server mod "
	  "starts it (a stun), not a key.\n"
	  "[b]action[/b]: On a press: an action the server mod declares (dash), or the engine's own: jump, sprint.\n"
	  "[b]event[/b]: On a cue: the event (the server mod emits it at the player).\n"
	  "[b]conditions[/b]: all must hold, on the player, before this tick's movement. They read what a state machine "
	  "reads: grounded · airborne_time > 0.1 · speed · vertical_speed < 0 · a field (dash.charges > 0) · an item "
	  "kind · a stance · a key that is down (held.jump) or went down this tick (pressed.dash) · linked (a probe of the "
	  "player's holds on to something).\n"
	  "[b]cooldown[/b]: seconds between two uses; for a While, between its end and its next start." },
	{ "Uses", "How many times before it has to refill. Not for a While.",
	  "[b]uses[/b]: 0 is no limit. A double jump is 1.\n"
	  "[b]refill[/b]: On the ground gives them back when the player stands. After seconds gives them back that long "
	  "after the last use." },
	{ "While it is on", "How long it is on, and how the player moves meanwhile.",
	  "A motion is [b]on[/b] while its forces and links act and its parameters hold: a While, while its conditions hold; "
	  "a press or a cue, for its duration, or until something ends it.\n"
	  "[b]duration[/b]: seconds it stays on after a press or a cue.\n"
	  "[b]until[/b]: conditions, any of which ends it. With one, a press keeps the motion on until then: pressed.grapple "
	  "(the next press), not held.grapple (the key let go). A motion with a CbProbe is on from when the probe takes "
	  "hold until this, or until what it holds on to is gone.\n"
	  "[b]parameters[/b]: movement parameters the player has meanwhile: friction 0 for a dash that slides, airborne 1 "
	  "on a hook, gravity 0 and move_frame 1 to fly. The names: walk_speed, sprint_speed, accelerate, friction, "
	  "stop_speed, air_control, gravity, jump_speed, turn_rate, max_fall, air_friction, move_frame, airborne, mass." },
	{ "When it starts", "What else it does to the player: fields, and an event.",
	  "[b]changes[/b]: dash.charges -= 1 (operators -=, +=, =). Fields the server mod declares for the player. In a "
	  "While, -= and += are per second (flight.fuel -= 30, a Float field) and = is set when it starts.\n"
	  "[b]emits[/b]: a mod event at the player (dash.started). Reactions play on it, a character's state machine can "
	  "enter a state on it, and server mods hear it a tick later. A While emits it when it starts." },
};

const Info kLaunchInfo[] = {
	{ "", "Throws an item of a kind into the world when its motion starts.",
	  "[b]item_kind[/b]: what is thrown, an item kind a server mod declares and a CbItem bakes (its body, its weight, "
	  "how it bounces, its look).\n"
	  "[b]speed[/b] along the [b]frame[/b] (where the camera looks, usually), with [b]lift[/b] m/s upward added for an "
	  "arc, and the thrower's own velocity.\n"
	  "[b]ahead[/b]: how far in front of the thrower it starts, so it does not start inside the thrower.\n"
	  "[b]seconds[/b]: when it is removed again. 0: it stays, like anything dropped.\n"
	  "Every simulation makes it from the thrower's own input, so it leaves on the tick of the press on the thrower's "
	  "screen. It is an item like any other: it can be picked up, its scene's reactions play, and it counts against "
	  "the server's caps on what a player may leave in the world. What a hit means (damage, a blast) is the server "
	  "mod's: it sees what the item ran into and who threw it. Give the motion a cooldown or uses." },
};

const Info kProbeInfo[] = {
	{ "", "A line thrown at what is under the crosshair. Its motion happens only if it finds something.",
	  "[b]range[/b]: how far it reaches, in metres.\n"
	  "[b]travel[/b]: the speed it flies at; it takes hold after distance / speed. 0: at once. The motion's forces, "
	  "links and parameters wait for it to take hold.\n"
	  "[b]holds[/b]: what it may hold on to: the level (anything static), bodies (props, items, rubble, ragdolls), "
	  "players. Anything else in its way stops it, holding nothing (a grappling hook: the level only).\n"
	  "What it finds is the motion's [b]hit[/b]: a point of the world, of a prop, or of another player's body. Effects "
	  "act on it (target: What the probe found) or go toward it (frame: Toward the target), and a CbLink is a rope to "
	  "it. The motion is on until its [b]until[/b] holds or what it found is gone. A CbLinkLook draws the line.\n"
	  "In conditions, [b]linked[/b] is true while a probe of the player's holds on to something." },
};

const Info kImpulseInfo[] = {
	{ "", "Once, when the motion starts: a change of the target's velocity. A dash, a jump, a launch.", kTargetDetail },
	{ "Impulse", "How fast, and what of the velocity it replaces.",
	  "[b]speed[/b]: metres per second along the frame. Negative goes the other way.\n"
	  "[b]replace[/b]: what of the target's velocity is cleared first. Vertical speed makes a jump in the air the same "
	  "jump whatever the fall was; Horizontal velocity makes a dash the same dash whatever the run was." },
};

const Info kForceInfo[] = {
	{ "", "For as long as the motion is on: a push on the target. A thrust, a pull, a current.", kTargetDetail },
	{ "Force", "What kind of push, how strong, and how it comes in.",
	  "[b]kind[/b]: Acceleration is metres per second, per second, whatever the target weighs (a thrust that lifts "
	  "anyone alike). Force is newtons: a heavy target answers slowly (mass is a movement parameter; a prop has its "
	  "own). Velocity brings the target toward [b]speed[/b] along the frame, no faster than [b]strength[/b] lets it (a "
	  "conveyor, a current).\n"
	  "[b]speed[/b]: for Acceleration and Force, the speed along the push past which it pushes no more (0: no limit).\n"
	  "[b]ramp_in[/b]: seconds from nothing to full strength. A push that comes in softly has weight, and hides the "
	  "moment it started.\n"
	  "[b]react[/b]: the other end is pushed the other way with the same momentum: the player, when the force is on "
	  "something else; what the probe found, when it is on the player. A hook that pulls you to a crate pulls the "
	  "crate to you, by what each weighs." },
};

const Info kLinkInfo[] = {
	{ "", "For as long as the motion is on: a rope between the player and the target.",
	  "[b]target[/b]: what the rope goes to: what the probe found, or the entity a field names (two bound players).\n"
	  "Neither end can go further from the other than the rope is long. What holding them together takes is shared "
	  "by what they weigh: the world gives nothing, so the player swings; a light crate comes to the player; two "
	  "players pull each other. It only holds: pulling is a CbForce next to it." },
	{ "Link", "How long the rope is.",
	  "[b]length[/b]: metres. 0: the distance when the motion's probe takes hold.\n"
	  "[b]reel[/b]: metres of rope taken in a second, for a rope that started at the probe's distance. It is never "
	  "shorter than a metre.\n"
	  "[b]react[/b]: on, the rope pulls both ends by what they weigh (a crate comes to the player). Off, it acts on "
	  "the player alone: what it holds is never moved by it, as if it were part of the world; a player hooked to a "
	  "rolling ball is dragged along by it." },
};

const Info kSetInfo = { "", "A mod's motions: the CbMotion nodes under it, baked to motions/<set_name>.cfg for the server.",
						"[b]set_name[/b]: what the server mod asks for: declare.Motions( \"dash.moves\" ).\n"
						"[b]Baking[/b]: saving the scene, the Bake button, and publishing the mod. A bake that changes "
						"nothing writes nothing.\n"
						"[b]As many motions as the mods have[/b]: a player keeps 16 bytes for each, all its mods' sets together." };

template <size_t N>
const Info* FindInfo( const Info ( &table )[N], const String& key )
{
	for ( const Info& info : table )
	{
		if ( key == info.key )
		{
			return &info;
		}
	}
	return nullptr;
}

const Info* InfoFor( Object* object, const String& group )
{
	if ( Object::cast_to<CbMotionSet>( object ) != nullptr )
	{
		return group.is_empty() ? &kSetInfo : nullptr;
	}
	if ( Object::cast_to<CbMotion>( object ) != nullptr )
	{
		return FindInfo( kMotionInfo, group );
	}
	if ( Object::cast_to<CbProbe>( object ) != nullptr )
	{
		return FindInfo( kProbeInfo, group );
	}
	if ( Object::cast_to<CbLaunch>( object ) != nullptr )
	{
		return FindInfo( kLaunchInfo, group );
	}
	if ( Object::cast_to<CbImpulse>( object ) != nullptr )
	{
		return FindInfo( kImpulseInfo, group );
	}
	if ( Object::cast_to<CbForce>( object ) != nullptr )
	{
		return FindInfo( kForceInfo, group );
	}
	if ( Object::cast_to<CbLink>( object ) != nullptr )
	{
		return FindInfo( kLinkInfo, group );
	}
	return nullptr;
}

} // namespace

// --- CbMotionPart -----------------------------------------------------------------------------------

std::string CbMotionPart::Bake( String& ) const
{
	return std::string();
}

String CbMotionPart::Fail( String& error, const String& why ) const
{
	error = String( get_name() ) + ": " + why;
	return error;
}

void CbMotionPart::Changed()
{
	// What is wrong with a part is said on the motion and the set above it too.
	for ( Node* node = this; node != nullptr && Object::cast_to<CbMotionPart>( node ) != nullptr; node = node->get_parent() )
	{
		node->update_configuration_warnings();
	}
}

PackedStringArray CbMotionPart::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	String error;
	if ( Bake( error ).empty() && error.is_empty() == false )
	{
		warnings.push_back( error );
	}
	Advice( warnings );
	warnings.append_array( names::Problems( this ) );
	return warnings;
}

void CbMotionPart::_validate_property( PropertyInfo& property ) const
{
	names::Hint( this, property );
}

// --- CbMotion ---------------------------------------------------------------------------------------

#define CB_MOTION_PROP( Class, type, name, hint, hintText )                                                                      \
	ClassDB::bind_method( D_METHOD( "set_" #name, "value" ), &Class::set_##name );                                               \
	ClassDB::bind_method( D_METHOD( "get_" #name ), &Class::get_##name );                                                        \
	ADD_PROPERTY( PropertyInfo( type, #name, hint, hintText ), "set_" #name, "get_" #name );

void CbMotion::_bind_methods()
{
	ADD_GROUP( "When", "" );
	CB_MOTION_PROP( CbMotion, Variant::INT, when, PROPERTY_HINT_ENUM, "On a press,While,On a cue" )
	CB_MOTION_PROP( CbMotion, Variant::STRING, action, PROPERTY_HINT_PLACEHOLDER_TEXT, "dash, jump, sprint" )
	CB_MOTION_PROP( CbMotion, Variant::STRING, event, PROPERTY_HINT_PLACEHOLDER_TEXT, "stun.hit" )
	CB_MOTION_PROP( CbMotion, Variant::PACKED_STRING_ARRAY, conditions, PROPERTY_HINT_NONE, "" )
	CB_MOTION_PROP( CbMotion, Variant::INT, with_item, PROPERTY_HINT_ENUM, "In the hand,Carried" )
	CB_MOTION_PROP( CbMotion, Variant::FLOAT, cooldown, PROPERTY_HINT_RANGE, "0,60,0.01,or_greater,suffix:s" )
	ADD_GROUP( "Uses", "" );
	CB_MOTION_PROP( CbMotion, Variant::INT, uses, PROPERTY_HINT_RANGE, "0,100,1,or_greater" )
	CB_MOTION_PROP( CbMotion, Variant::INT, refill, PROPERTY_HINT_ENUM, "On the ground,After seconds" )
	CB_MOTION_PROP( CbMotion, Variant::FLOAT, refill_seconds, PROPERTY_HINT_RANGE, "0,60,0.01,or_greater,suffix:s" )
	ADD_GROUP( "While it is on", "" );
	CB_MOTION_PROP( CbMotion, Variant::FLOAT, duration, PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:s" )
	CB_MOTION_PROP( CbMotion, Variant::PACKED_STRING_ARRAY, until, PROPERTY_HINT_NONE, "" )
	CB_MOTION_PROP( CbMotion, Variant::DICTIONARY, parameters, PROPERTY_HINT_DICTIONARY_TYPE, "String;float" )
	ADD_GROUP( "When it starts", "" );
	CB_MOTION_PROP( CbMotion, Variant::PACKED_STRING_ARRAY, changes, PROPERTY_HINT_NONE, "" )
	CB_MOTION_PROP( CbMotion, Variant::STRING, emits, PROPERTY_HINT_PLACEHOLDER_TEXT, "dash.started" )

	BIND_ENUM_CONSTANT( WHEN_PRESS );
	BIND_ENUM_CONSTANT( WHEN_WHILE );
	BIND_ENUM_CONSTANT( WHEN_EVENT );
	BIND_ENUM_CONSTANT( REFILL_ON_GROUND );
	BIND_ENUM_CONSTANT( REFILL_AFTER_SECONDS );
}

std::string CbMotion::Bake( String& error ) const
{
	return BakeWith( error, String() );
}

String CbMotion::ItemCondition( const String& kind ) const
{
	return m_withItem == WITH_CARRIED ? String( "carried." ) + kind : kind;
}

std::string CbMotion::BakeWith( String& error, const String& alsoIf ) const
{
	auto fail = [&]( const String& why ) {
		Fail( error, why );
		return std::string();
	};
	String name = String( get_name() );
	String action = m_action.strip_edges();
	String event = m_event.strip_edges();
	if ( Plain( name ) == false )
	{
		return fail( "the node's name cannot have a tab or a #" );
	}
	std::string text = "motion\t" + Std( name ) + "\n";
	if ( m_when == WHEN_WHILE )
	{
		text += "when\twhile\n";
	}
	else if ( m_when == WHEN_EVENT )
	{
		if ( event.is_empty() || Plain( event ) == false || event.contains( " " ) )
		{
			return fail( "name the event it answers (one the server mod emits at the player)" );
		}
		text += "when\tevent\t" + Std( event ) + "\n";
	}
	else
	{
		if ( action.is_empty() || Plain( action ) == false || action.contains( " " ) )
		{
			return fail( "name the action whose press this answers (one the server mod declares, or jump, sprint)" );
		}
		text += "when\tpress\t" + Std( action ) + "\n";
	}

	String all, any, bad;
	if ( Joined( m_conditions, " and ", all, bad ) == false )
	{
		return fail( "condition \"" + bad + "\" cannot have a tab or a #" );
	}
	if ( Joined( m_until, " or ", any, bad ) == false )
	{
		return fail( "until \"" + bad + "\" cannot have a tab or a #" );
	}
	if ( alsoIf.is_empty() == false )
	{
		all = all.is_empty() ? alsoIf : alsoIf + String( " and ( " ) + all + String( " )" );
	}
	if ( all.is_empty() == false )
	{
		text += "if\t" + Std( all ) + "\n";
	}
	if ( any.is_empty() == false && m_when != WHEN_WHILE )
	{
		text += "until\t" + Std( any ) + "\n";
	}
	if ( m_cooldown > 0.0 )
	{
		text += "cooldown\t" + Num( m_cooldown ) + "\n";
	}
	if ( m_uses > 0 && m_when != WHEN_WHILE )
	{
		text += "uses\t" + std::to_string( m_uses ) + "\t" + ( m_refill == REFILL_ON_GROUND ? std::string( "ground" ) : Num( m_refillSeconds ) ) +
				"\n";
	}
	if ( m_duration > 0.0 && m_when != WHEN_WHILE )
	{
		text += "duration\t" + Num( m_duration ) + "\n";
	}
	// In the simulation's order, so the file does not depend on the dictionary's.
	Array names = m_parameters.keys();
	for ( int64_t i = 0; i < names.size(); ++i )
	{
		if ( MoveParamByName( Std( String( names[i] ).strip_edges() ).c_str() ) < 0 )
		{
			return fail( "parameters: \"" + String( names[i] ) + "\" is not a movement parameter" );
		}
	}
	for ( int p = 0; p < kMoveParams; ++p )
	{
		for ( int64_t i = 0; i < names.size(); ++i )
		{
			if ( MoveParamByName( Std( String( names[i] ).strip_edges() ).c_str() ) == p )
			{
				text += std::string( "param\t" ) + MoveParamInfoOf( p ).name + "\t" + Num( double( m_parameters[names[i]] ) ) + "\n";
			}
		}
	}

	// Its probe, then its effects, in the order of the tree.
	int probes = 0;
	for ( int pass = 0; pass < 2; ++pass )
	{
		for ( int i = 0; i < get_child_count(); ++i )
		{
			auto* part = Object::cast_to<CbMotionPart>( get_child( i ) );
			bool isProbe = Object::cast_to<CbProbe>( get_child( i ) ) != nullptr;
			if ( part == nullptr || isProbe != ( pass == 0 ) )
			{
				continue;
			}
			if ( Object::cast_to<CbMotion>( part ) != nullptr || Object::cast_to<CbMotionSet>( part ) != nullptr )
			{
				return fail( "a motion's children are a CbProbe, a CbLaunch and effects (CbImpulse, CbForce, CbLink), not another motion" );
			}
			probes += isProbe ? 1 : 0;
			if ( probes > 1 )
			{
				return fail( "a motion has one CbProbe" );
			}
			String childError;
			std::string lines = part->Bake( childError );
			if ( lines.empty() )
			{
				error = String( get_name() ) + "/" + childError;
				return std::string();
			}
			text += lines;
		}
	}

	for ( int64_t i = 0; i < m_changes.size(); ++i )
	{
		if ( m_changes[i].strip_edges().is_empty() )
		{
			continue;
		}
		cue::Change change;
		String why;
		if ( cue::ParseChange( m_changes[i], change, &why ) == false || Plain( change.field ) == false )
		{
			return fail( "change \"" + m_changes[i] + "\": " + why );
		}
		text += "change\t" + Std( change.field ) + "\t" + ( change.op == '=' ? std::string( "=" ) : std::string( 1, change.op ) + "=" ) + "\t" +
				Num( change.value ) + "\n";
	}
	String emits = m_emits.strip_edges();
	if ( emits.is_empty() == false )
	{
		if ( Plain( emits ) == false || emits.contains( " " ) )
		{
			return fail( "emits names one event" );
		}
		text += "emit\t" + Std( emits ) + "\n";
	}

	// What the simulation's own compiler says about it (names no mod declares are the server's to
	// report: the editor does not know them).
	std::vector<Motion> compiled;
	std::string compileError, ignored;
	if ( CompileMotionSet( "check", "cinderbox_motions\t2\n" + text, ModSchema{}, compiled, compileError, ignored ) == false )
	{
		// After "motions check, line N: " or "motions check: check/<name>: ".
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

void CbMotion::Advice( PackedStringArray& advice ) const
{
	if ( Object::cast_to<CbMotionSet>( get_parent() ) == nullptr && ( get_parent() == nullptr || get_parent()->is_class( "CbItem" ) == false ) )
	{
		advice.push_back( "Put it under a CbMotionSet (a motion every player has: the server mod names the set) or under a CbItem (a motion of who has the item)." );
	}
	bool effects = false;
	bool probe = false;
	for ( int i = 0; i < get_child_count(); ++i )
	{
		effects |= Object::cast_to<CbMotionEffect>( get_child( i ) ) != nullptr || Object::cast_to<CbLaunch>( get_child( i ) ) != nullptr;
		probe |= Object::cast_to<CbProbe>( get_child( i ) ) != nullptr;
	}
	bool until = false;
	for ( int64_t i = 0; i < m_until.size(); ++i )
	{
		until |= m_until[i].strip_edges().is_empty() == false;
	}
	if ( effects == false && m_parameters.is_empty() && m_changes.is_empty() && m_emits.strip_edges().is_empty() )
	{
		advice.push_back( "It does nothing yet: add a CbImpulse, a CbForce, a CbLink or a CbLaunch under it, or give it parameters, changes or an event." );
	}
	if ( m_parameters.is_empty() == false && m_duration <= 0.0 && m_when != WHEN_WHILE && until == false && probe == false )
	{
		advice.push_back( "Its parameters hold while it is on, and it is not: give it a duration, or an until." );
	}
	if ( probe && until == false )
	{
		advice.push_back( "Nothing lets its probe go (until): it holds until what it holds on to is gone, or the player dies." );
	}
	if ( m_when == WHEN_WHILE )
	{
		bool conditions = false;
		for ( int64_t i = 0; i < m_conditions.size(); ++i )
		{
			conditions |= m_conditions[i].strip_edges().is_empty() == false;
		}
		if ( conditions == false )
		{
			advice.push_back( "A While without conditions is always on, for every player." );
		}
		if ( m_uses > 0 || m_duration > 0.0 || until )
		{
			advice.push_back( "uses, duration and until are not used by a While: it is on while its conditions hold." );
		}
	}
}

// --- CbProbe ----------------------------------------------------------------------------------------

void CbProbe::_bind_methods()
{
	CB_MOTION_PROP( CbProbe, Variant::FLOAT, range, PROPERTY_HINT_RANGE, "1,200,0.5,or_greater,suffix:m" )
	CB_MOTION_PROP( CbProbe, Variant::FLOAT, travel, PROPERTY_HINT_RANGE, "0,200,1,or_greater,suffix:m/s" )
	CB_MOTION_PROP( CbProbe, Variant::INT, holds, PROPERTY_HINT_FLAGS, "The level,Bodies,Players" )
	ADD_GROUP( "Under an item", "" );
	CB_MOTION_PROP( CbProbe, Variant::INT, release, PROPERTY_HINT_FLAGS, "Using the item again,A jump" )
	CB_MOTION_PROP( CbProbe, Variant::FLOAT, hold_seconds, PROPERTY_HINT_RANGE, "0,60,0.05,or_greater,suffix:s" )
	ADD_GROUP( "", "" );
	CB_MOTION_PROP( CbProbe, Variant::STRING, hold_emits, PROPERTY_HINT_PLACEHOLDER_TEXT, "grapple.hooked" )
}

std::string CbProbe::Bake( String& error ) const
{
	if ( InMotion( this ) == false )
	{
		Fail( error, "put it under a CbMotion, or under a CbItem (what using the item does): it is the line thrown" );
		return std::string();
	}
	std::string out = "probe\t" + Num( m_range ) + "\t" + Num( m_travel ) + "\n";
	const int all = HOLDS_LEVEL | HOLDS_BODIES | HOLDS_PLAYERS;
	if ( ( m_holds & all ) == 0 )
	{
		Fail( error, "holds: it has to be able to hold on to something" );
		return std::string();
	}
	if ( ( m_holds & all ) != all )
	{
		out += "holds";
		out += ( m_holds & HOLDS_LEVEL ) ? "\tlevel" : "";
		out += ( m_holds & HOLDS_BODIES ) ? "\tbodies" : "";
		out += ( m_holds & HOLDS_PLAYERS ) ? "\tplayers" : "";
		out += "\n";
	}
	String emits = m_holdEmits.strip_edges();
	if ( emits.is_empty() == false )
	{
		if ( emits.contains( " " ) || emits.contains( "\t" ) )
		{
			Fail( error, "hold_emits names one event" );
			return std::string();
		}
		out += "hold_emit\t" + std::string( emits.utf8().get_data() ) + "\n";
	}
	return out;
}

// --- CbMove -----------------------------------------------------------------------------------------

void CbMove::_bind_methods()
{
	CB_MOTION_PROP( CbMove, Variant::DICTIONARY, parameters, PROPERTY_HINT_DICTIONARY_TYPE, "String;float" )
	CB_MOTION_PROP( CbMove, Variant::FLOAT, duration, PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:s" )
}

std::string CbMove::Bake( String& error ) const
{
	if ( InMotion( this ) == false || Object::cast_to<CbMotion>( get_parent() ) != nullptr )
	{
		Fail( error, "put it under a CbItem, or under its CbProbe or CbLink (a CbMotion has parameters of its own)" );
		return std::string();
	}
	if ( m_parameters.is_empty() )
	{
		Fail( error, "parameters: say how the player moves (friction 0, gravity 0)" );
		return std::string();
	}
	String why;
	std::string lines = ParamLines( m_parameters, why );
	if ( lines.empty() )
	{
		Fail( error, why );
	}
	return lines;
}

// --- What using an item does ------------------------------------------------------------------------

bool InMotion( const Node* node )
{
	if ( node == nullptr || node->get_parent() == nullptr )
	{
		return false;
	}
	if ( Object::cast_to<CbMotion>( node->get_parent() ) != nullptr )
	{
		return true;
	}
	for ( const Node* up = node->get_parent(); up != nullptr; up = up->get_parent() )
	{
		if ( Object::cast_to<CbMotion>( up ) != nullptr || Object::cast_to<CbMotionSet>( up ) != nullptr )
		{
			return false;
		}
		if ( up->is_class( "CbItem" ) )
		{
			return true;
		}
	}
	return false;
}

namespace
{
// The parts under `node` (not under a CbMotion), in tree order: the probe, the moves, the rest.
void CollectItemParts( const Node* node, const CbProbe*& probe, int& probes, std::vector<const CbMove*>& moves,
					   std::vector<const CbMotionPart*>& effects, int depth )
{
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		Node* child = node->get_child( i );
		if ( Object::cast_to<CbMotion>( child ) != nullptr || Object::cast_to<CbMotionSet>( child ) != nullptr )
		{
			continue;
		}
		if ( auto* p = Object::cast_to<CbProbe>( child ) )
		{
			probe = probe == nullptr ? p : probe;
			probes += 1;
		}
		else if ( auto* m = Object::cast_to<CbMove>( child ) )
		{
			moves.push_back( m );
		}
		else if ( auto* part = Object::cast_to<CbMotionPart>( child ) )
		{
			effects.push_back( part );
		}
		if ( Object::cast_to<CbMotionPart>( child ) != nullptr )
		{
			CollectItemParts( child, probe, probes, moves, effects, depth + 1 );
		}
	}
}
} // namespace

std::string BakeItemUse( const Node* item, const String& kind, int use, const PackedStringArray& conditions, const PackedStringArray& changes,
						 double cooldown, const String& emits, String& error )
{
	const CbProbe* probe = nullptr;
	int probes = 0;
	std::vector<const CbMove*> moves;
	std::vector<const CbMotionPart*> effects;
	CollectItemParts( item, probe, probes, moves, effects, 0 );
	if ( probe == nullptr && moves.empty() && effects.empty() )
	{
		return std::string();
	}
	auto fail = [&]( const String& why ) {
		error = why;
		return std::string();
	};
	if ( probes > 1 )
	{
		return fail( "an item throws one line: one CbProbe under it" );
	}
	// When: the use (its slot key, or the use button with it in the hand), or while it is in use.
	const bool hold = use == 2;
	std::string text = "motion\tUse\n";
	text += hold ? std::string( "when\twhile\n" ) : "when\tevent\t" + Std( kind ) + ".used\n";
	// If: in use (held), no line of the player's out (a press then lets one go, it does not throw
	// another), and the item's own rules.
	PackedStringArray all;
	if ( hold )
	{
		all.push_back( "using." + kind );
	}
	if ( probe != nullptr )
	{
		all.push_back( "not linked" );
	}
	all.append_array( conditions );
	String joined, bad;
	if ( Joined( all, " and ", joined, bad ) == false )
	{
		return fail( "use_conditions: \"" + bad + "\" cannot have a tab or a #" );
	}
	if ( joined.is_empty() == false )
	{
		text += "if\t" + Std( joined ) + "\n";
	}
	// Until: what lets the line go.
	if ( probe != nullptr && hold == false )
	{
		PackedStringArray until;
		if ( probe->get_release() & CbProbe::RELEASE_USE_AGAIN )
		{
			until.push_back( kind + String( ".used" ) );
		}
		if ( probe->get_release() & CbProbe::RELEASE_JUMP )
		{
			until.push_back( "pressed.jump" );
		}
		String any;
		if ( Joined( until, " or ", any, bad ) && any.is_empty() == false )
		{
			text += "until\t" + Std( any ) + "\n";
		}
	}
	if ( cooldown > 0.0 )
	{
		text += "cooldown\t" + Num( cooldown ) + "\n";
	}
	// How long it is on: the line's longest hold, or a move's time from the use.
	double duration = probe != nullptr ? probe->get_hold_seconds() : 0.0;
	for ( const CbMove* move : moves )
	{
		duration = probe == nullptr ? std::max( duration, move->get_duration() ) : duration;
	}
	if ( duration > 0.0 && hold == false )
	{
		text += "duration\t" + Num( duration ) + "\n";
	}
	// How the player moves while it is on (the moves together; a later one wins a parameter).
	Dictionary parameters;
	for ( const CbMove* move : moves )
	{
		String why;
		if ( move->Bake( why ).empty() )
		{
			return fail( String( move->get_name() ) + ": " + why );
		}
		parameters.merge( move->get_parameters(), true );
	}
	String why;
	std::string params = ParamLines( parameters, why );
	if ( why.is_empty() == false )
	{
		return fail( why );
	}
	text += params;
	// Its probe, then its effects, in the order of the tree.
	if ( probe != nullptr )
	{
		std::string line = probe->Bake( why );
		if ( line.empty() )
		{
			return fail( String( probe->get_name() ) + ": " + why );
		}
		text += line;
	}
	for ( const CbMotionPart* part : effects )
	{
		std::string lines = part->Bake( why );
		if ( lines.empty() )
		{
			return fail( String( part->get_name() ) + ": " + why );
		}
		text += lines;
	}
	for ( int64_t i = 0; i < changes.size(); ++i )
	{
		if ( changes[i].strip_edges().is_empty() )
		{
			continue;
		}
		cue::Change change;
		String reason;
		if ( cue::ParseChange( changes[i], change, &reason ) == false || Plain( change.field ) == false )
		{
			return fail( "use_changes \"" + changes[i] + "\": " + reason );
		}
		text += "change\t" + Std( change.field ) + "\t" + ( change.op == '=' ? std::string( "=" ) : std::string( 1, change.op ) + "=" ) + "\t" +
				Num( change.value ) + "\n";
	}
	String emit = emits.strip_edges();
	if ( emit.is_empty() == false )
	{
		if ( Plain( emit ) == false || emit.contains( " " ) )
		{
			return fail( "use_emits names one event" );
		}
		text += "emit\t" + Std( emit ) + "\n";
	}
	// What the simulation's own compiler says about it.
	std::vector<Motion> compiled;
	std::string compileError, ignored;
	if ( CompileMotionSet( "check", "cinderbox_motions\t2\n" + text, ModSchema{}, compiled, compileError, ignored ) == false )
	{
		size_t colon = compileError.find( ": " );
		std::string reason = compileError.substr( colon == std::string::npos ? 0 : colon + 2 );
		return fail( String::utf8( reason.c_str() ) );
	}
	return text;
}

// --- A rules sheet's motion blocks ------------------------------------------------------------------

namespace
{

// A statement's motion part (a sheet's impulse, force, move, launch) as lines: a move as parameters
// (with its seconds), the rest as the node it would be, under a motion, baked as one.
std::string BakeSheetPart( const Dictionary& part, const String& kind, String& why )
{
	Dictionary props = part.get( "properties", Dictionary() );
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
	if ( String( part["class"] ) == "CbLaunch" && int( props.get( "what", 0 ) ) == CbLaunch::WHAT_CARRIED && props.has( "item_kind" ) == false )
	{
		props["item_kind"] = kind; // (the item's own)
	}
	auto* node = Object::cast_to<CbMotionPart>( ClassDB::instantiate( StringName( String( part["class"] ) ) ) );
	if ( node == nullptr )
	{
		why = "this game has no " + String( part["class"] );
		return std::string();
	}
	Array keys = props.keys();
	for ( int64_t k = 0; k < keys.size(); ++k )
	{
		node->set( StringName( String( keys[k] ) ), props[keys[k]] );
	}
	CbMotion* holder = memnew( CbMotion );
	holder->add_child( node );
	std::string lines = node->Bake( why );
	holder->remove_child( node );
	memdelete( node );
	memdelete( holder );
	// An expression where its number is (an impulse's speed, a force's strength): in its place.
	String expression = String( props.get( "value_expression", "" ) ).strip_edges();
	if ( lines.empty() == false && expression.is_empty() == false )
	{
		if ( Plain( expression ) == false )
		{
			why = "\"" + expression + "\" cannot have a tab or a #";
			return std::string();
		}
		const int field = String( part["class"] ) == "CbImpulse" ? 2 : 4;
		size_t at = 0;
		for ( int k = 0; k < field; ++k )
		{
			at = lines.find( '\t', at ) + 1;
		}
		size_t end = lines.find_first_of( "\t\n", at );
		lines.replace( at, end - at, Std( expression ) );
	}
	return lines;
}

// The curves a motion's expressions read ("curve(Falloff, reach)"), from CbCurve nodes (or any
// node with a Curve in "curve") found by name from `from`: "curve" lines, a value at even steps.
bool CurveLines( const std::vector<std::string>& expressions, const Node* from, std::string& out, String& why )
{
	std::vector<std::string> names;
	for ( const std::string& text : expressions )
	{
		expr::Program program;
		std::string error;
		if ( text.empty() || expr::Compile( text, program, error ) == false )
		{
			continue; // (what is not an expression is said by the simulation's compiler)
		}
		for ( const std::string& name : program.curves )
		{
			if ( std::find( names.begin(), names.end(), name ) == names.end() )
			{
				names.push_back( name );
			}
		}
	}
	for ( const std::string& name : names )
	{
		const Node* node = from != nullptr ? from->get_node_or_null( NodePath( String::utf8( name.c_str() ) ) ) : nullptr;
		Ref<Curve> curve = node != nullptr ? Ref<Curve>( node->get( "curve" ) ) : Ref<Curve>();
		if ( curve.is_null() )
		{
			why = "curve(" + String::utf8( name.c_str() ) + ", ...): no CbCurve named " + String::utf8( name.c_str() ) + " with a curve beside the sheet";
			return false;
		}
		float lo = curve->get_min_domain(), hi = curve->get_max_domain();
		if ( hi <= lo )
		{
			hi = lo + 1.0f;
		}
		out += "curve\t" + name + "\t" + Num( lo ) + "\t" + Num( hi );
		for ( int k = 0; k < kCurveSamples; ++k )
		{
			out += "\t" + Num( curve->sample( lo + ( hi - lo ) * float( k ) / float( kCurveSamples - 1 ) ) );
		}
		out += "\n";
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
	// Written in the line: around the player's middle, a metre up.
	if ( kind == "sphere" )
	{
		out = "shape\t" + Num( double( shape["radius"] ) ) + PointWords( Vector3( 0, 1, 0 ) ) + "\n";
		return true;
	}
	if ( kind == "capsule" )
	{
		double r = shape["radius"], h = shape["height"];
		out = "shape\t" + Num( r ) + PointWords( Vector3( 0, float( 1.0 + h / 2.0 - r ), 0 ) ) + PointWords( Vector3( 0, float( 1.0 - h / 2.0 + r ), 0 ) ) + "\n";
		return true;
	}
	if ( kind == "box" )
	{
		Vector3 half = Vector3( shape["size"] ).abs() / 2.0f;
		out = "shape\t0";
		for ( int c = 0; c < 8; ++c )
		{
			out += PointWords( Vector3( c & 1 ? half.x : -half.x, 1.0f + ( c & 2 ? half.y : -half.y ), c & 4 ? half.z : -half.z ) );
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
		// Every expression it has, for the curves they read (written first: before what reads them).
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
				add( Dictionary( Dictionary( statement.get( "part", Dictionary() ) ).get( "properties", Dictionary() ) ).get( "value_expression", Variant() ) );
				statementsOf( statement.get( "statements", Array() ) );
			}
		};
		Array all = p.get( "steps", Array() );
		for ( int64_t k = 0; k < all.size(); ++k )
		{
			add( Dictionary( all[k] ).get( "wait", Variant() ) );
			statementsOf( Dictionary( all[k] ).get( "statements", Array() ) );
		}
		std::string curves;
		String why;
		if ( CurveLines( expressions, from, curves, why ) == false )
		{
			return fail( why );
		}
		text += curves;
	}
	const String trigger = p.get( "trigger", "" );
	PackedStringArray conditions;
	auto word = [&]( const char* key ) {
		String w = String( p.get( key, "" ) ).strip_edges();
		return Plain( w ) && w.contains( " " ) == false ? w : String();
	};
	if ( trigger == "use" )
	{
		if ( kind.is_empty() )
		{
			return fail( "on use is for an item's sheet (a motion set's motions are everyone's)" );
		}
		text += "when\tevent\t" + Std( kind ) + ".used\n";
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
		text += "when\tevent\t" + Std( word( "event" ) ) + "\n";
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
	// Who may: in an item's sheet, whoever has it out (or carries it); its use says so itself.
	if ( kind.is_empty() == false && trigger != "use" )
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
	if ( double( p.get( "cooldown", 0.0 ) ) > 0.0 )
	{
		text += "cooldown\t" + Num( double( p["cooldown"] ) ) + "\n";
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
				cue::Change change;
				String why;
				if ( cue::ParseChange( statement["text"], change, &why ) == false || Plain( change.field ) == false )
				{
					return fail( line + String( "change \"" ) + String( statement["text"] ) + "\": " + why );
				}
				text += "change\t" + Std( change.field ) + "\t" + ( change.op == '=' ? std::string( "=" ) : std::string( 1, change.op ) + "=" ) +
						"\t" + Num( change.value ) + "\n";
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
				std::string lines = BakeSheetPart( statement["part"], kind, why );
				if ( lines.empty() )
				{
					return fail( line + why );
				}
				text += lines;
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
					std::string lines = BakeSheetPart( each["part"], kind, why );
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
					std::string lines = BakeSheetPart( each["part"], kind, why );
					if ( lines.empty() )
					{
						return fail( at + why );
					}
					text += lines;
				}
				text += "endfind\n";
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

// --- CbLaunch ---------------------------------------------------------------------------------------

void CbLaunch::_bind_methods()
{
	CB_MOTION_PROP( CbLaunch, Variant::INT, what, PROPERTY_HINT_ENUM, "A new one of its item kind,The one the player carries" )
	CB_MOTION_PROP( CbLaunch, Variant::STRING, item_kind, PROPERTY_HINT_PLACEHOLDER_TEXT, "grenade.shell" )
	CB_MOTION_PROP( CbLaunch, Variant::FLOAT, speed, PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m/s" )
	CB_MOTION_PROP( CbLaunch, Variant::FLOAT, lift, PROPERTY_HINT_RANGE, "-20,20,0.1,or_greater,or_less,suffix:m/s" )
	CB_MOTION_PROP( CbLaunch, Variant::INT, frame, PROPERTY_HINT_ENUM, "Look,Move input,Facing,Up,World direction" )
	CB_MOTION_PROP( CbLaunch, Variant::VECTOR3, direction, PROPERTY_HINT_NONE, "" )
	CB_MOTION_PROP( CbLaunch, Variant::FLOAT, ahead, PROPERTY_HINT_RANGE, "0,5,0.05,suffix:m" )
	CB_MOTION_PROP( CbLaunch, Variant::FLOAT, seconds, PROPERTY_HINT_RANGE, "0,60,0.1,or_greater,suffix:s" )

	BIND_ENUM_CONSTANT( FRAME_LOOK );
	BIND_ENUM_CONSTANT( FRAME_MOVE );
	BIND_ENUM_CONSTANT( FRAME_FACING );
	BIND_ENUM_CONSTANT( FRAME_UP );
	BIND_ENUM_CONSTANT( FRAME_WORLD );
	BIND_ENUM_CONSTANT( WHAT_NEW );
	BIND_ENUM_CONSTANT( WHAT_CARRIED );
}

std::string CbLaunch::Bake( String& error ) const
{
	if ( InMotion( this ) == false )
	{
		Fail( error, "put it under a CbMotion, or under a CbItem: it is what is thrown" );
		return std::string();
	}
	String kind = m_itemKind.strip_edges();
	if ( kind.is_empty() && m_what == WHAT_CARRIED )
	{
		// Under a CbItem: the item's own kind.
		for ( const Node* up = get_parent(); up != nullptr && kind.is_empty(); up = up->get_parent() )
		{
			kind = up->is_class( "CbItem" ) ? String( up->get( "kind" ) ).strip_edges() : kind;
		}
	}
	if ( kind.is_empty() || Plain( kind ) == false || kind.contains( " " ) )
	{
		Fail( error, "name the item kind it throws (item_kind: one a server mod declares, and a CbItem bakes: grenade.shell; "
					 "the one the player carries, under a CbItem, may leave it empty for that item's)" );
		return std::string();
	}
	std::string line = std::string( m_what == WHAT_CARRIED ? "launch_carried\t" : "launch\t" ) + Std( kind ) + "\t" + Num( m_speed ) + "\t" + kFrames[m_frame >= 0 && m_frame <= FRAME_WORLD ? m_frame : FRAME_LOOK] +
					   "\t" + Num( m_lift ) + "\t" + Num( m_ahead ) + "\t" + Num( m_seconds );
	if ( m_frame == FRAME_WORLD )
	{
		Vector3 d = m_direction.normalized();
		line += "\t" + Num( d.x ) + "\t" + Num( d.y ) + "\t" + Num( d.z );
	}
	return line + "\n";
}

// --- CbMotionEffect ---------------------------------------------------------------------------------

void CbMotionEffect::_bind_methods()
{
	ADD_GROUP( "On", "" );
	CB_MOTION_PROP( CbMotionEffect, Variant::INT, target, PROPERTY_HINT_ENUM, "The player,What the probe found,The entity a field names" )
	CB_MOTION_PROP( CbMotionEffect, Variant::STRING, target_field, PROPERTY_HINT_PLACEHOLDER_TEXT, "bind.partner" )
	CB_MOTION_PROP( CbMotionEffect, Variant::INT, frame, PROPERTY_HINT_ENUM, "Look,Move input,Facing,Up,World direction,Toward the target" )
	CB_MOTION_PROP( CbMotionEffect, Variant::VECTOR3, direction, PROPERTY_HINT_NONE, "" )

	BIND_ENUM_CONSTANT( TARGET_SELF );
	BIND_ENUM_CONSTANT( TARGET_HIT );
	BIND_ENUM_CONSTANT( TARGET_FIELD );
	BIND_ENUM_CONSTANT( TARGET_FOUND );
	BIND_ENUM_CONSTANT( FRAME_LOOK );
	BIND_ENUM_CONSTANT( FRAME_MOVE );
	BIND_ENUM_CONSTANT( FRAME_FACING );
	BIND_ENUM_CONSTANT( FRAME_UP );
	BIND_ENUM_CONSTANT( FRAME_WORLD );
	BIND_ENUM_CONSTANT( FRAME_TO );
	BIND_ENUM_CONSTANT( FRAME_AWAY );
}

std::string CbMotionEffect::TargetWord( String& error ) const
{
	if ( InMotion( this ) == false )
	{
		Fail( error, "put it under a CbMotion, or under a CbItem (what using the item does)" );
		return std::string();
	}
	if ( m_target == TARGET_SELF )
	{
		return "self";
	}
	if ( m_target == TARGET_HIT )
	{
		return "hit";
	}
	if ( m_target == TARGET_FOUND )
	{
		return "found"; // (a sheet's find bakes it; a node outside one is refused by the compiler)
	}
	String field = m_targetField.strip_edges();
	if ( m_target != TARGET_FIELD || field.is_empty() || Plain( field ) == false || field.contains( " " ) )
	{
		Fail( error, "name the field whose entity it acts on (target_field: one the server mod writes an entity's id into)" );
		return std::string();
	}
	return "@" + Std( field );
}

std::string CbMotionEffect::FrameWord() const
{
	return kFrames[m_frame >= 0 && m_frame <= FRAME_AWAY ? m_frame : FRAME_MOVE];
}

std::string CbMotionEffect::WorldWords() const
{
	if ( m_frame != FRAME_WORLD )
	{
		return std::string();
	}
	Vector3 d = m_direction.normalized();
	return "\t" + Num( d.x ) + "\t" + Num( d.y ) + "\t" + Num( d.z );
}

// --- CbImpulse --------------------------------------------------------------------------------------

void CbImpulse::_bind_methods()
{
	ADD_GROUP( "Impulse", "" );
	CB_MOTION_PROP( CbImpulse, Variant::FLOAT, speed, PROPERTY_HINT_RANGE, "-50,50,0.1,or_greater,or_less,suffix:m/s" )
	CB_MOTION_PROP( CbImpulse, Variant::INT, replace, PROPERTY_HINT_ENUM, "Nothing,Vertical speed,Horizontal velocity,All" )

	BIND_ENUM_CONSTANT( REPLACE_NOTHING );
	BIND_ENUM_CONSTANT( REPLACE_VERTICAL );
	BIND_ENUM_CONSTANT( REPLACE_HORIZONTAL );
	BIND_ENUM_CONSTANT( REPLACE_ALL );
}

std::string CbImpulse::Bake( String& error ) const
{
	std::string target = TargetWord( error );
	if ( target.empty() )
	{
		return std::string();
	}
	return "impulse\t" + target + "\t" + Num( m_speed ) + "\t" + FrameWord() + "\t" +
		   kReplaces[m_replace >= 0 && m_replace <= REPLACE_ALL ? m_replace : REPLACE_NOTHING] + WorldWords() + "\n";
}

// --- CbForce ----------------------------------------------------------------------------------------

void CbForce::_bind_methods()
{
	ADD_GROUP( "Force", "" );
	CB_MOTION_PROP( CbForce, Variant::INT, kind, PROPERTY_HINT_ENUM, "Acceleration (m/s²),Force (N),Velocity (toward a speed)" )
	CB_MOTION_PROP( CbForce, Variant::FLOAT, strength, PROPERTY_HINT_RANGE, "-200,200,0.1,or_greater,or_less" )
	CB_MOTION_PROP( CbForce, Variant::FLOAT, speed, PROPERTY_HINT_RANGE, "-50,50,0.1,or_greater,or_less,suffix:m/s" )
	CB_MOTION_PROP( CbForce, Variant::FLOAT, ramp_in, PROPERTY_HINT_RANGE, "0,5,0.01,or_greater,suffix:s" )
	CB_MOTION_PROP( CbForce, Variant::BOOL, react, PROPERTY_HINT_NONE, "" )

	BIND_ENUM_CONSTANT( KIND_ACCELERATION );
	BIND_ENUM_CONSTANT( KIND_FORCE );
	BIND_ENUM_CONSTANT( KIND_VELOCITY );
}

std::string CbForce::Bake( String& error ) const
{
	std::string target = TargetWord( error );
	if ( target.empty() )
	{
		return std::string();
	}
	return "force\t" + target + "\t" + FrameWord() + "\t" + kPushes[m_kind >= 0 && m_kind <= KIND_VELOCITY ? m_kind : KIND_ACCELERATION] + "\t" +
		   Num( m_strength ) + "\t" + Num( m_speed ) + "\t" + Num( m_ramp ) + "\t" + ( m_react ? "1" : "0" ) + WorldWords() + "\n";
}

// --- CbLink -----------------------------------------------------------------------------------------

void CbLink::_bind_methods()
{
	ADD_GROUP( "Link", "" );
	CB_MOTION_PROP( CbLink, Variant::FLOAT, length, PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:m" )
	CB_MOTION_PROP( CbLink, Variant::FLOAT, reel, PROPERTY_HINT_RANGE, "0,50,0.1,or_greater,suffix:m/s" )
	CB_MOTION_PROP( CbLink, Variant::BOOL, react, PROPERTY_HINT_NONE, "" )
}

void CbLink::_validate_property( PropertyInfo& property ) const
{
	// A rope has no direction of its own.
	if ( property.name == StringName( "frame" ) || property.name == StringName( "direction" ) )
	{
		property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

std::string CbLink::Bake( String& error ) const
{
	std::string target = TargetWord( error );
	if ( target.empty() )
	{
		return std::string();
	}
	if ( m_target == TARGET_SELF )
	{
		Fail( error, "a rope goes to something else: what the probe found, or the entity a field names" );
		return std::string();
	}
	// (A rope that reacts, the default, bakes as it always did.)
	return "link\t" + target + "\t" + Num( m_length ) + "\t" + Num( m_reel ) + ( m_react ? "" : "\t0" ) + "\n";
}

#undef CB_MOTION_PROP

// --- CbCurve ----------------------------------------------------------------------------------------

void CbCurve::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_curve", "curve" ), &CbCurve::set_curve );
	ClassDB::bind_method( D_METHOD( "get_curve" ), &CbCurve::get_curve );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "curve", PROPERTY_HINT_RESOURCE_TYPE, "Curve" ), "set_curve", "get_curve" );
}

PackedStringArray CbCurve::_get_configuration_warnings() const
{
	PackedStringArray out;
	if ( m_curve.is_null() )
	{
		out.push_back( "Give it a Curve: a rules sheet's motion reads it by this node's name, curve(" + String( get_name() ) + ", x)." );
	}
	return out;
}

// --- CbMotionSet ------------------------------------------------------------------------------------

void CbMotionSet::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_set_name", "name" ), &CbMotionSet::set_set_name );
	ClassDB::bind_method( D_METHOD( "get_set_name" ), &CbMotionSet::get_set_name );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "set_name", PROPERTY_HINT_PLACEHOLDER_TEXT, "dash.moves" ), "set_set_name", "get_set_name" );
	ClassDB::bind_method( D_METHOD( "bake" ), &CbMotionSet::bake );
	ClassDB::bind_method( D_METHOD( "bake_to_project" ), &CbMotionSet::bake_to_project );
	ClassDB::bind_method( D_METHOD( "get_bake_button" ), &CbMotionSet::get_bake_button );
	ADD_PROPERTY( PropertyInfo( Variant::CALLABLE, "bake_button", PROPERTY_HINT_TOOL_BUTTON, "Bake motions,Save", PROPERTY_USAGE_EDITOR ), "",
				  "get_bake_button" );
}

Callable CbMotionSet::get_bake_button()
{
	return Callable( this, "bake_to_project" );
}

std::string CbMotionSet::Bake( String& error ) const
{
	String name = m_name.strip_edges();
	if ( name.is_empty() || name.contains( "/" ) || name.contains( " " ) || name.contains( "\\" ) || Plain( name ) == false )
	{
		error = "set_name must be one word (dash.moves): the server mod asks for the set by it";
		return std::string();
	}
	std::string text = "cinderbox_motions\t2\n# Baked from the CbMotion nodes of a CbMotionSet. Edit the scene and bake again.\n";
	int count = 0;
	std::vector<std::string> names;
	for ( int i = 0; i < get_child_count(); ++i )
	{
		auto* motion = Object::cast_to<CbMotion>( get_child( i ) );
		if ( motion == nullptr )
		{
			// A probe or an effect here would silently do nothing: it says where it belongs.
			if ( auto* part = Object::cast_to<CbMotionPart>( get_child( i ) ); part != nullptr )
			{
				error = String( part->get_name() ) + ": put it under a CbMotion: it is what that motion does";
				return std::string();
			}
			continue;
		}
		std::string lines = motion->Bake( error );
		if ( lines.empty() )
		{
			return std::string();
		}
		text += lines;
		count += 1;
		names.push_back( Std( String( motion->get_name() ) ) );
	}
	// The motion blocks of its rules sheets (a CbRules under it): a file of version 3.
	std::string sheetLines;
	size_t before = names.size();
	if ( BakeSheetMotions( this, String(), sheetLines, names, error ) == false )
	{
		return std::string();
	}
	if ( sheetLines.empty() == false )
	{
		text.replace( 0, std::string( "cinderbox_motions\t2" ).size(), "cinderbox_motions\t3" );
		text += sheetLines;
		count += int( names.size() - before );
	}
	if ( count > kMotionLimit )
	{
		error = vformat( "%d motions: a server runs at most %d, all its mods' sets together", count, kMotionLimit );
		return std::string();
	}
	return text;
}

Dictionary CbMotionSet::bake() const
{
	Dictionary result;
	String error;
	std::string text = Bake( error );
	// (How many: its lines that start one.)
	int motions = 0;
	for ( size_t at = text.find( "\nmotion\t" ); at != std::string::npos; at = text.find( "\nmotion\t", at + 1 ) )
	{
		motions += 1;
	}
	result["text"] = String::utf8( text.c_str() );
	result["error"] = text.empty() ? error : String();
	result["motions"] = motions;
	return result;
}

void CbMotionSet::bake_to_project()
{
	Dictionary baked = bake();
	if ( String( baked["text"] ).is_empty() )
	{
		UtilityFunctions::push_error( "Motions bake failed: ", baked["error"] );
		return;
	}
	DirAccess::make_dir_recursive_absolute( "res://motions" );
	String path = "res://motions/" + m_name.strip_edges() + ".cfg";
	PackedByteArray bytes = String( baked["text"] ).to_utf8_buffer();
	// A bake that changes nothing touches nothing: it runs on every save and every publish.
	if ( FileAccess::file_exists( path ) && FileAccess::get_file_as_bytes( path ) == bytes )
	{
		return;
	}
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::WRITE );
	if ( file.is_null() )
	{
		UtilityFunctions::push_error( "Motions bake: cannot write ", path );
		return;
	}
	file->store_buffer( bytes );
	UtilityFunctions::print( "Baked ", baked["motions"], " motions into ", path );
}

// --- The inspector's help -----------------------------------------------------------------------------

bool CbMotionInspector::_can_handle( Object* object ) const
{
	return Object::cast_to<CbMotionPart>( object ) != nullptr;
}

void CbMotionInspector::_parse_begin( Object* object )
{
	if ( const Info* info = InfoFor( object, String() ) )
	{
		add_custom_control( InfoRow( info->brief, info->detail ) );
	}
}

void CbMotionInspector::_parse_group( Object* object, const String& group )
{
	if ( group.is_empty() )
	{
		return;
	}
	if ( const Info* info = InfoFor( object, group ) )
	{
		add_custom_control( InfoRow( info->brief, info->detail ) );
	}
	else if ( group == "On" && Object::cast_to<CbMotionEffect>( object ) != nullptr )
	{
		add_custom_control( InfoRow( "Who it acts on, and along what.", kTargetDetail ) );
	}
}

} // namespace cb::gd
