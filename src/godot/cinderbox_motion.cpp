#include "cinderbox_motion.h"

#include "cue/cue_info.h"
#include "cue/cue_prediction.h" // ParseChange: a motion's changes are written as a prediction's are

#include "mod_schema.h"
#include "motions.h"
#include "move_params.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

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

const char* const kFrames[] = { "look", "move", "facing", "up", "world", "to" };
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
	{ "", "A motion: when it happens and for how long. Its children are what it does (CbProbe, CbImpulse, CbForce, CbLink).",
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

const Info kProbeInfo[] = {
	{ "", "A line thrown at what is under the crosshair. Its motion happens only if it finds something.",
	  "[b]range[/b]: how far it reaches, in metres.\n"
	  "[b]travel[/b]: the speed it flies at; it takes hold after distance / speed. 0: at once. The motion's forces, "
	  "links and parameters wait for it to take hold.\n"
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
	  "shorter than a metre." },
};

const Info kSetInfo = { "", "A mod's motions: the CbMotion nodes under it, baked to motions/<set_name>.cfg for the server.",
						"[b]set_name[/b]: what the server mod asks for: declare.Motions( \"dash.moves\" ).\n"
						"[b]Baking[/b]: saving the scene, the Bake button, and publishing the mod. A bake that changes "
						"nothing writes nothing.\n"
						"[b]At most 16 motions[/b] on a server, all its mods' sets together." };

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
	return warnings;
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
				return fail( "a motion's children are a CbProbe and effects (CbImpulse, CbForce, CbLink), not another motion" );
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
	if ( Object::cast_to<CbMotionSet>( get_parent() ) == nullptr )
	{
		advice.push_back( "Put it under a CbMotionSet: the set is what is baked and what the server mod names." );
	}
	bool effects = false;
	bool probe = false;
	for ( int i = 0; i < get_child_count(); ++i )
	{
		effects |= Object::cast_to<CbMotionEffect>( get_child( i ) ) != nullptr;
		probe |= Object::cast_to<CbProbe>( get_child( i ) ) != nullptr;
	}
	bool until = false;
	for ( int64_t i = 0; i < m_until.size(); ++i )
	{
		until |= m_until[i].strip_edges().is_empty() == false;
	}
	if ( effects == false && m_parameters.is_empty() && m_changes.is_empty() && m_emits.strip_edges().is_empty() )
	{
		advice.push_back( "It does nothing yet: add a CbImpulse, a CbForce or a CbLink under it, or give it parameters, changes or an event." );
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
}

std::string CbProbe::Bake( String& error ) const
{
	if ( Object::cast_to<CbMotion>( get_parent() ) == nullptr )
	{
		Fail( error, "put it under a CbMotion: it is that motion's probe" );
		return std::string();
	}
	return "probe\t" + Num( m_range ) + "\t" + Num( m_travel ) + "\n";
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
	BIND_ENUM_CONSTANT( FRAME_LOOK );
	BIND_ENUM_CONSTANT( FRAME_MOVE );
	BIND_ENUM_CONSTANT( FRAME_FACING );
	BIND_ENUM_CONSTANT( FRAME_UP );
	BIND_ENUM_CONSTANT( FRAME_WORLD );
	BIND_ENUM_CONSTANT( FRAME_TO );
}

std::string CbMotionEffect::TargetWord( String& error ) const
{
	if ( Object::cast_to<CbMotion>( get_parent() ) == nullptr )
	{
		Fail( error, "put it under a CbMotion: it is what that motion does" );
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
	return kFrames[m_frame >= 0 && m_frame <= FRAME_TO ? m_frame : FRAME_MOVE];
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
	return "link\t" + target + "\t" + Num( m_length ) + "\t" + Num( m_reel ) + "\n";
}

#undef CB_MOTION_PROP

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
	}
	if ( count > kMaxMotions )
	{
		error = vformat( "%d motions: a server runs at most %d, all its mods' sets together", count, kMaxMotions );
		return std::string();
	}
	return text;
}

Dictionary CbMotionSet::bake() const
{
	Dictionary result;
	String error;
	std::string text = Bake( error );
	int motions = 0;
	for ( int i = 0; i < get_child_count(); ++i )
	{
		motions += Object::cast_to<CbMotion>( get_child( i ) ) != nullptr ? 1 : 0;
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
