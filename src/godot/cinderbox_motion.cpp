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

const char* const kFrames[] = { "look", "move", "facing", "up", "world" };
const char* const kReplaces[] = { "none", "vertical", "horizontal", "all" };

struct Info
{
	const char* key;
	const char* brief;
	const char* detail;
};

// The motion inspector's texts: one per group, and one about the node ("").
const Info kMotionInfo[] = {
	{ "", "Adds to how players move with no code: on a press, on a cue, or while its conditions hold, it changes the mover.",
	  "[b]Where[/b]: under a CbMotionSet, in a scene of the mod's project (motion_sets/). Saving the scene bakes the set.\n"
	  "[b]Who runs it[/b]: every simulation, the server's and each client's, from the player's input. So your own "
	  "dash starts on the tick of the press, and other players see the same dash.\n"
	  "[b]The server mod[/b] declares the action, the fields and the events named here, and names the set: "
	  "declare.Motions( \"dash.moves\" ). Who may use a motion is a field its conditions read.\n"
	  "[b]A reaction[/b] on the event it emits shows it; that needs no CbPrediction." },
	{ "When", "What it waits for, and what has to hold.",
	  "[b]when[/b]: On a press happens once, when the action goes down (a dash). While is on every tick its "
	  "conditions hold (flight, a glide, a jetpack). On a cue happens when a mod event is recorded at the player: "
	  "a server mod starts it (a stun), not a key.\n"
	  "[b]action[/b]: On a press: an action the server mod declares (dash), or the engine's own: jump, sprint.\n"
	  "[b]event[/b]: On a cue: the event (the server mod emits it at the player).\n"
	  "[b]conditions[/b]: all must hold, on the player, before this tick's movement. They read what a state machine "
	  "reads: grounded · airborne_time > 0.1 · speed · vertical_speed < 0 · a field (dash.charges > 0) · an item "
	  "kind (grapple.gun) · a stance · a key that is down (held.jump, held.dash).\n"
	  "[b]cooldown[/b]: seconds between two uses; for a While, between its end and its next start." },
	{ "Uses", "How many times before it has to refill. Not for a While.",
	  "[b]uses[/b]: 0 is no limit. A double jump is 1.\n"
	  "[b]refill[/b]: On the ground gives them back when the player stands. After seconds gives them back that long "
	  "after the last use." },
	{ "Impulse", "The change of velocity, and its direction.",
	  "[b]impulse[/b]: metres per second. A While adds that much every second it is on (a thrust).\n"
	  "[b]impulse_frame[/b]: Look is where the camera looks, pitch included. Move input is where WASD points (the "
	  "facing when nothing is held): a dash. Facing is where the body faces. Up is a jump. World direction is "
	  "impulse_direction as given.\n"
	  "[b]replace[/b]: what of the velocity is cleared first. Vertical speed makes a jump in the air the same jump "
	  "whatever the fall was; Horizontal velocity makes a dash the same dash whatever the run was. Not for a While." },
	{ "While it lasts", "While it is on, the player moves by these parameters.",
	  "[b]duration[/b]: seconds it stays on after a press or a cue. A While is on while its conditions hold.\n"
	  "[b]parameters[/b]: movement parameters by name, with the value they have while the motion is on: friction 0 "
	  "for a dash that slides, gravity 4 for a float. The names: walk_speed, sprint_speed, accelerate, friction, "
	  "stop_speed, air_control, gravity, jump_speed, turn_rate, max_fall, air_friction, move_frame (1: WASD "
	  "moves along the camera, up and down too: flight)." },
	{ "When it happens", "What else the press does: fields of the player, and an event.",
	  "[b]changes[/b]: dash.charges -= 1 (operators -=, +=, =). Fields the server mod declares for the player. In a "
	  "While, -= and += are per second (jetpack.fuel -= 20, a Float field) and = is set when it starts.\n"
	  "[b]emits[/b]: a mod event at the player (dash.started). Reactions play on it, a character's state machine can "
	  "enter a state on it, and server mods hear it a tick later. A While emits it when it starts." },
};

const Info* FindInfo( const String& key )
{
	for ( const Info& info : kMotionInfo )
	{
		if ( key == info.key )
		{
			return &info;
		}
	}
	return nullptr;
}

const Info kSetInfo = { "", "A mod's motions: the CbMotion nodes under it, baked to motions/<set_name>.cfg for the server.",
						"[b]set_name[/b]: what the server mod asks for: declare.Motions( \"dash.moves\" ).\n"
						"[b]Baking[/b]: saving the scene, the Bake button, and publishing the mod. A bake that changes "
						"nothing writes nothing.\n"
						"[b]At most 16 motions[/b] on a server, all its mods' sets together." };

} // namespace

// --- CbMotion ---------------------------------------------------------------------------------------

void CbMotion::_bind_methods()
{
#define CB_MOTION_PROP( type, name, hint, hintText )                                                                              \
	ClassDB::bind_method( D_METHOD( "set_" #name, "value" ), &CbMotion::set_##name );                                            \
	ClassDB::bind_method( D_METHOD( "get_" #name ), &CbMotion::get_##name );                                                     \
	ADD_PROPERTY( PropertyInfo( type, #name, hint, hintText ), "set_" #name, "get_" #name );

	ADD_GROUP( "When", "" );
	CB_MOTION_PROP( Variant::INT, when, PROPERTY_HINT_ENUM, "On a press,While,On a cue" )
	CB_MOTION_PROP( Variant::STRING, action, PROPERTY_HINT_PLACEHOLDER_TEXT, "dash, jump, sprint" )
	CB_MOTION_PROP( Variant::STRING, event, PROPERTY_HINT_PLACEHOLDER_TEXT, "stun.hit" )
	CB_MOTION_PROP( Variant::PACKED_STRING_ARRAY, conditions, PROPERTY_HINT_NONE, "" )
	CB_MOTION_PROP( Variant::FLOAT, cooldown, PROPERTY_HINT_RANGE, "0,60,0.01,or_greater,suffix:s" )
	ADD_GROUP( "Uses", "" );
	CB_MOTION_PROP( Variant::INT, uses, PROPERTY_HINT_RANGE, "0,100,1,or_greater" )
	CB_MOTION_PROP( Variant::INT, refill, PROPERTY_HINT_ENUM, "On the ground,After seconds" )
	CB_MOTION_PROP( Variant::FLOAT, refill_seconds, PROPERTY_HINT_RANGE, "0,60,0.01,or_greater,suffix:s" )
	ADD_GROUP( "Impulse", "" );
	CB_MOTION_PROP( Variant::FLOAT, impulse, PROPERTY_HINT_RANGE, "-50,50,0.1,or_greater,or_less,suffix:m/s" )
	CB_MOTION_PROP( Variant::INT, impulse_frame, PROPERTY_HINT_ENUM, "Look,Move input,Facing,Up,World direction" )
	CB_MOTION_PROP( Variant::VECTOR3, impulse_direction, PROPERTY_HINT_NONE, "" )
	CB_MOTION_PROP( Variant::INT, replace, PROPERTY_HINT_ENUM, "Nothing,Vertical speed,Horizontal velocity,All" )
	ADD_GROUP( "While it lasts", "" );
	CB_MOTION_PROP( Variant::FLOAT, duration, PROPERTY_HINT_RANGE, "0,10,0.01,or_greater,suffix:s" )
	CB_MOTION_PROP( Variant::DICTIONARY, parameters, PROPERTY_HINT_DICTIONARY_TYPE, "String;float" )
	ADD_GROUP( "When it happens", "" );
	CB_MOTION_PROP( Variant::PACKED_STRING_ARRAY, changes, PROPERTY_HINT_NONE, "" )
	CB_MOTION_PROP( Variant::STRING, emits, PROPERTY_HINT_PLACEHOLDER_TEXT, "dash.started" )
#undef CB_MOTION_PROP

	BIND_ENUM_CONSTANT( WHEN_PRESS );
	BIND_ENUM_CONSTANT( WHEN_WHILE );
	BIND_ENUM_CONSTANT( WHEN_EVENT );
	BIND_ENUM_CONSTANT( FRAME_LOOK );
	BIND_ENUM_CONSTANT( FRAME_MOVE );
	BIND_ENUM_CONSTANT( FRAME_FACING );
	BIND_ENUM_CONSTANT( FRAME_UP );
	BIND_ENUM_CONSTANT( FRAME_WORLD );
	BIND_ENUM_CONSTANT( REPLACE_NOTHING );
	BIND_ENUM_CONSTANT( REPLACE_VERTICAL );
	BIND_ENUM_CONSTANT( REPLACE_HORIZONTAL );
	BIND_ENUM_CONSTANT( REPLACE_ALL );
	BIND_ENUM_CONSTANT( REFILL_ON_GROUND );
	BIND_ENUM_CONSTANT( REFILL_AFTER_SECONDS );
}

std::string CbMotion::Bake( String& error ) const
{
	auto fail = [&]( const String& why ) {
		error = String( get_name() ) + ": " + why;
		return std::string();
	};
	String name = String( get_name() );
	String action = m_action.strip_edges();
	if ( Plain( name ) == false )
	{
		return fail( "the node's name cannot have a tab or a #" );
	}
	String event = m_event.strip_edges();
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

	// All of the conditions: one expression.
	String all;
	for ( int64_t i = 0; i < m_conditions.size(); ++i )
	{
		String condition = m_conditions[i].strip_edges();
		if ( condition.is_empty() )
		{
			continue;
		}
		if ( Plain( condition ) == false )
		{
			return fail( "condition \"" + condition + "\" cannot have a tab or a #" );
		}
		all += String( all.is_empty() ? "" : " and " ) + "( " + condition + " )";
	}
	if ( all.is_empty() == false )
	{
		text += "if\t" + Std( all ) + "\n";
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
	if ( m_impulse != 0.0 )
	{
		if ( m_frame < 0 || m_frame > FRAME_WORLD || m_replace < 0 || m_replace > REPLACE_ALL )
		{
			return fail( "impulse_frame or replace is not one of its choices" );
		}
		text += "impulse\t" + Num( m_impulse ) + "\t" + kFrames[m_frame] + "\t" + kReplaces[m_when == WHEN_WHILE ? 0 : m_replace];
		if ( m_frame == FRAME_WORLD )
		{
			Vector3 d = m_direction.normalized();
			text += "\t" + Num( d.x ) + "\t" + Num( d.y ) + "\t" + Num( d.z );
		}
		text += "\n";
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
	if ( CompileMotionSet( "check", "cinderbox_motions\t1\n" + text, ModSchema{}, compiled, compileError, ignored ) == false )
	{
		size_t colon = compileError.find( ": " ); // after "motions check, line N"
		return fail( String::utf8( compileError.substr( colon == std::string::npos ? 0 : colon + 2 ).c_str() ) );
	}
	return text;
}

PackedStringArray CbMotion::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	String error;
	if ( Bake( error ).empty() )
	{
		warnings.push_back( error );
	}
	if ( Object::cast_to<CbMotionSet>( get_parent() ) == nullptr )
	{
		warnings.push_back( "Put it under a CbMotionSet: the set is what is baked and what the server mod names." );
	}
	if ( m_impulse == 0.0 && m_parameters.is_empty() && m_changes.is_empty() && m_emits.strip_edges().is_empty() )
	{
		warnings.push_back( "It does nothing yet: give it an impulse, parameters with a duration, changes, or an event to emit." );
	}
	if ( m_parameters.is_empty() == false && m_duration <= 0.0 && m_when != WHEN_WHILE )
	{
		warnings.push_back( "Its parameters hold for its duration, which is 0." );
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
			warnings.push_back( "A While without conditions is always on, for every player." );
		}
		if ( m_uses > 0 || m_duration > 0.0 || m_replace != REPLACE_NOTHING )
		{
			warnings.push_back( "uses, duration and replace are not used by a While: it is on while its conditions hold, and its impulse is per second." );
		}
	}
	return warnings;
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

Dictionary CbMotionSet::bake() const
{
	Dictionary result;
	result["text"] = String();
	result["error"] = String();
	String name = m_name.strip_edges();
	if ( name.is_empty() || name.contains( "/" ) || name.contains( " " ) || name.contains( "\\" ) || Plain( name ) == false )
	{
		result["error"] = "set_name must be one word (dash.moves): the server mod asks for the set by it";
		return result;
	}
	std::string text = "cinderbox_motions\t1\n# Baked from the CbMotion nodes of a CbMotionSet. Edit the scene and bake again.\n";
	int count = 0;
	for ( int i = 0; i < get_child_count(); ++i )
	{
		auto* motion = Object::cast_to<CbMotion>( get_child( i ) );
		if ( motion == nullptr )
		{
			continue;
		}
		String error;
		std::string lines = motion->Bake( error );
		if ( lines.empty() )
		{
			result["error"] = error;
			return result;
		}
		text += lines;
		count += 1;
	}
	if ( count > kMaxMotions )
	{
		result["error"] = vformat( "%d motions: a server runs at most %d, all its mods' sets together", count, kMaxMotions );
		return result;
	}
	result["text"] = String::utf8( text.c_str() );
	result["motions"] = count;
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

PackedStringArray CbMotionSet::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	Dictionary baked = bake();
	if ( String( baked["text"] ).is_empty() )
	{
		warnings.push_back( baked["error"] );
	}
	return warnings;
}

// --- The inspector's help -----------------------------------------------------------------------------

bool CbMotionInspector::_can_handle( Object* object ) const
{
	return Object::cast_to<CbMotion>( object ) != nullptr || Object::cast_to<CbMotionSet>( object ) != nullptr;
}

void CbMotionInspector::_parse_begin( Object* object )
{
	const Info* info = Object::cast_to<CbMotionSet>( object ) != nullptr ? &kSetInfo : FindInfo( "" );
	add_custom_control( InfoRow( info->brief, info->detail ) );
}

void CbMotionInspector::_parse_group( Object* object, const String& group )
{
	if ( Object::cast_to<CbMotion>( object ) == nullptr )
	{
		return;
	}
	if ( const Info* info = FindInfo( group ) )
	{
		add_custom_control( InfoRow( info->brief, info->detail ) );
	}
}

} // namespace cb::gd
