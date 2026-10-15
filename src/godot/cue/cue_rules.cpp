#include "cue_rules.h"

#include "cue_prediction.h"
#include "cue_reaction.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

namespace cb::cue
{

namespace
{

// A word of a line, and where it starts (so "the rest of the line" can be taken as it was written).
struct Token
{
	String text;
	int64_t start = 0;
	bool quoted = false;
};

// The words of a line: spaces part them, double quotes keep spaces in one, # starts a comment.
std::vector<Token> Split( const String& line, String& rest )
{
	std::vector<Token> out;
	int64_t i = 0;
	const int64_t n = line.length();
	while ( i < n )
	{
		char32_t c = line[i];
		if ( c == ' ' || c == '\t' )
		{
			++i;
			continue;
		}
		if ( c == '#' )
		{
			break;
		}
		Token token;
		token.start = i;
		if ( c == '"' )
		{
			int64_t close = line.find( "\"", i + 1 );
			close = close < 0 ? n : close;
			token.text = line.substr( i + 1, close - i - 1 );
			token.quoted = true;
			i = close + 1;
		}
		else
		{
			int64_t end = i;
			// (A word ends at a space; brackets keep "rgb(1, 0, 0)" and "(1, 2, 3)" one word.)
			int depth = 0;
			while ( end < n && ( depth > 0 || ( line[end] != ' ' && line[end] != '\t' ) ) && line[end] != '#' )
			{
				depth += line[end] == '(' ? 1 : line[end] == ')' ? -1 : 0;
				++end;
			}
			token.text = line.substr( i, end - i );
			i = end;
		}
		out.push_back( token );
	}
	rest = line;
	return out;
}

// The line from a token on, as written, without a comment at its end.
String RestFrom( const String& line, const Token& token )
{
	String text = line.substr( token.start );
	int64_t hash = text.find( "#" );
	return ( hash >= 0 ? text.substr( 0, hash ) : text ).strip_edges();
}

bool Number( const Token& token, double& value )
{
	if ( token.quoted || token.text.is_valid_float() == false )
	{
		return false;
	}
	value = token.text.to_float();
	return true;
}

// A value to set: a number, a truth, a colour, a vector, else words.
Variant Value( const Token& token )
{
	const String& t = token.text;
	if ( token.quoted )
	{
		return t;
	}
	if ( t == "true" || t == "false" )
	{
		return t == "true";
	}
	if ( t.is_valid_int() )
	{
		return t.to_int();
	}
	if ( t.is_valid_float() )
	{
		return t.to_float();
	}
	auto numbers = [&]( const String& inside ) {
		PackedStringArray parts = inside.split( "," );
		std::vector<double> out;
		for ( int64_t i = 0; i < parts.size(); ++i )
		{
			String part = parts[i].strip_edges();
			if ( part.is_valid_float() == false )
			{
				return std::vector<double>{};
			}
			out.push_back( part.to_float() );
		}
		return out;
	};
	if ( ( t.begins_with( "rgb(" ) || t.begins_with( "rgba(" ) ) && t.ends_with( ")" ) )
	{
		int64_t open = t.find( "(" );
		std::vector<double> v = numbers( t.substr( open + 1, t.length() - open - 2 ) );
		if ( v.size() == 3 || v.size() == 4 )
		{
			return Color( float( v[0] ), float( v[1] ), float( v[2] ), v.size() == 4 ? float( v[3] ) : 1.0f );
		}
	}
	if ( t.begins_with( "(" ) && t.ends_with( ")" ) )
	{
		std::vector<double> v = numbers( t.substr( 1, t.length() - 2 ) );
		if ( v.size() == 3 )
		{
			return Vector3( float( v[0] ), float( v[1] ), float( v[2] ) );
		}
	}
	return t;
}

// "at <where> [from PATH] [offset X Y Z]", from the word after "at" up to `end`: the place into
// `into`, and where it stopped in `next`. False, with why, when it cannot be read.
bool ParseAt( const std::vector<Token>& w, size_t i, size_t end, Dictionary& into, size_t& next, String& why )
{
	if ( i >= end )
	{
		why = "at point, end, beam, follow, or a node's path";
		return false;
	}
	const String where = w[i].text;
	const int place = where == "point" ? 1 : where == "end" ? 2 : where == "beam" ? 3 : where == "follow" ? 5 : 4;
	into["place"] = place;
	if ( place == 4 )
	{
		into["place_node"] = where;
	}
	++i;
	if ( i + 1 < end && w[i].quoted == false && w[i].text == "from" )
	{
		into["place_node"] = w[i + 1].text; // a beam from a node
		i += 2;
	}
	if ( i < end && w[i].quoted == false && w[i].text == "offset" )
	{
		double x = 0, y = 0, z = 0;
		if ( i + 3 >= end || Number( w[i + 1], x ) == false || Number( w[i + 2], y ) == false || Number( w[i + 3], z ) == false )
		{
			why = "offset X Y Z";
			return false;
		}
		into["offset"] = Vector3( float( x ), float( y ), float( z ) );
		i += 4;
	}
	next = i;
	return true;
}

struct Building
{
	int line = 0;
	Dictionary settings;			 // for every action of the rule (a prediction: all of it)
	std::vector<Rule> actions;		 // its own properties, and its line
	bool prediction = false;
	PackedStringArray changes;		 // a prediction's
	bool bad = false;
};

} // namespace

bool IsPathProperty( const String& name )
{
	return name == "subject" || name == "place_node" || name == "target" || name == "scene_parent" || name == "animation_player";
}

std::vector<Rule> ParseRules( const String& text, PackedStringArray& errors )
{
	std::vector<Rule> out;
	Building rule;
	bool open = false;
	auto fail = [&]( int line, const String& why ) {
		errors.push_back( "line " + String::num_int64( line ) + ": " + why );
		rule.bad = true;
	};
	auto finish = [&]() {
		if ( open == false )
		{
			return;
		}
		open = false;
		if ( rule.bad )
		{
			return;
		}
		if ( rule.prediction )
		{
			Rule made;
			made.line = rule.line;
			made.prediction = true;
			made.properties = rule.settings;
			if ( rule.changes.is_empty() == false )
			{
				made.properties["changes"] = rule.changes;
			}
			out.push_back( made );
			return;
		}
		if ( rule.actions.empty() )
		{
			errors.push_back( "line " + String::num_int64( rule.line ) + ": the rule does nothing: give it an action (sound, scene, play, set, call, shake, flash)" );
			return;
		}
		for ( Rule& action : rule.actions )
		{
			Rule made;
			made.line = action.line;
			made.properties = rule.settings.duplicate();
			made.properties.merge( action.properties, true );
			out.push_back( made );
		}
	};
	PackedStringArray lines = text.replace( "\r", "" ).split( "\n" );
	for ( int64_t index = 0; index < lines.size(); ++index )
	{
		const int number = int( index + 1 );
		const String line = lines[index];
		String unused;
		std::vector<Token> w = Split( line, unused );
		if ( w.empty() )
		{
			continue;
		}
		const String verb = w[0].text;
		// --- a prediction: the viewer's own press, shown as the cue the server will answer with
		if ( verb == "predict" )
		{
			finish();
			rule = Building{};
			rule.line = number;
			rule.prediction = true;
			open = true;
			if ( w.size() < 4 || w[2].text != "as" )
			{
				fail( number, "predict <action> as <cue> [if <expression>]" );
				continue;
			}
			rule.settings["action"] = w[1].text;
			rule.settings["cue"] = w[3].text;
			if ( w.size() >= 5 )
			{
				if ( w[4].text != "if" || w.size() < 6 )
				{
					fail( number, "after the cue: if <expression>" );
					continue;
				}
				rule.settings["conditions"] = PackedStringArray( { RestFrom( line, w[5] ) } );
			}
			continue;
		}
		if ( open && rule.prediction )
		{
			double seconds = 0.0;
			if ( verb == "cooldown" && w.size() == 2 && Number( w[1], seconds ) )
			{
				rule.settings["cooldown"] = seconds;
			}
			else if ( verb == "held" && w.size() == 1 )
			{
				rule.settings["while_held"] = true;
			}
			else if ( verb == "change" && w.size() >= 4 )
			{
				rule.changes.push_back( RestFrom( line, w[1] ) );
			}
			else if ( verb == "stance" && ( w.size() == 2 || ( w.size() == 4 && w[2].text == "on" ) ) )
			{
				rule.settings["stance"] = w[1].text;
				if ( w.size() == 4 )
				{
					rule.settings["stance_layer"] = w[3].text;
				}
			}
			else if ( verb != "on" && verb != "while" )
			{
				fail( number, "a prediction takes cooldown S, held, change FIELD OP NUMBER, stance NAME [on LAYER]" );
				continue;
			}
			if ( verb != "on" && verb != "while" )
			{
				continue;
			}
		}
		// --- a new rule
		if ( verb == "on" || verb == "while" )
		{
			finish();
			rule = Building{};
			rule.line = number;
			open = true;
			if ( verb == "on" )
			{
				if ( w.size() < 2 )
				{
					fail( number, "on what? on <event> [if <expression>]" );
					continue;
				}
				rule.settings["when"] = int( gd::CbReaction::WHEN_EVENT );
				rule.settings["event"] = w[1].text;
				if ( w.size() >= 3 )
				{
					if ( w[2].text != "if" || w.size() < 4 )
					{
						fail( number, "after the event: if <expression>" );
						continue;
					}
					rule.settings["conditions"] = PackedStringArray( { RestFrom( line, w[3] ) } );
				}
			}
			else
			{
				if ( w.size() < 2 )
				{
					fail( number, "while what? while <expression>" );
					continue;
				}
				rule.settings["when"] = int( gd::CbReaction::WHEN_WHILE );
				rule.settings["conditions"] = PackedStringArray( { RestFrom( line, w[1] ) } );
			}
			continue;
		}
		if ( open == false )
		{
			errors.push_back( "line " + String::num_int64( number ) + ": \"" + verb + "\" outside a rule: start one with on <event> or while <expression>" );
			continue;
		}
		// --- what the rest of the line says, word by word: name value pairs after the action's own
		auto options = [&]( size_t from, Dictionary& into, const std::vector<std::pair<const char*, const char*>>& names ) {
			for ( size_t i = from; i < w.size(); i += 2 )
			{
				String name = w[i].text;
				if ( name == "volume_by" && i + 1 < w.size() )
				{
					into["volume_expression"] = RestFrom( line, w[i + 1] );
					return true;
				}
				const char* property = nullptr;
				for ( const auto& [word, prop] : names )
				{
					property = name == word ? prop : property;
				}
				double value = 0.0;
				if ( property == nullptr || i + 1 >= w.size() )
				{
					fail( number, "\"" + name + "\" is not something " + verb + " takes" );
					return false;
				}
				if ( String( property ) == "bus" || String( property ) == "animation_off" || String( property ) == "scene_parent" )
				{
					into[property] = w[i + 1].text; // (words, not numbers)
				}
				else if ( Number( w[i + 1], value ) )
				{
					into[property] = value;
				}
				else
				{
					fail( number, String( name ) + " wants a number, not \"" + w[i + 1].text + "\"" );
					return false;
				}
			}
			return true;
		};
		double number1 = 0.0;
		// --- settings
		if ( verb == "subject" || verb == "kind" || verb == "template" )
		{
			if ( w.size() != 2 )
			{
				fail( number, verb + String( " takes one word" ) );
				continue;
			}
			rule.settings[verb == "subject" ? "subject" : verb == "kind" ? "subject_kind" : "subject_template"] = w[1].text;
			continue;
		}
		if ( verb == "side" )
		{
			String side = w.size() == 2 ? w[1].text : String();
			if ( side != "a" && side != "b" && side != "either" )
			{
				fail( number, "side a, side b or side either" );
				continue;
			}
			rule.settings["event_side"] = side == "a" ? 0 : side == "b" ? 1 : 2;
			continue;
		}
		if ( verb == "delay" || verb == "chance" || verb == "cooldown" )
		{
			if ( w.size() != 2 || Number( w[1], number1 ) == false )
			{
				fail( number, verb + String( " takes a number" ) );
				continue;
			}
			rule.settings[verb] = number1;
			continue;
		}
		if ( verb == "wait" )
		{
			rule.settings["wait_for_server"] = true;
			continue;
		}
		if ( verb == "at" )
		{
			size_t next = 0;
			String why;
			if ( ParseAt( w, 1, w.size(), rule.settings, next, why ) == false || next != w.size() )
			{
				fail( number, why.is_empty() ? String( "after where: from PATH, offset X Y Z" ) : why );
			}
			continue;
		}
		// --- actions
		Rule action;
		action.line = number;
		Dictionary& p = action.properties;
		// Settings written on the action's own line (at, delay, chance, cooldown, wait): for this
		// action only, over the rule's. Not after an expression ("= ...", volume_by), which runs to
		// the end of the line.
		{
			size_t stop = w.size();
			for ( size_t k = 1; k < w.size(); ++k )
			{
				if ( w[k].quoted == false && ( w[k].text == "volume_by" || ( verb == "set" && w[k].text == "=" ) ) )
				{
					stop = k;
					break;
				}
			}
			std::vector<Token> kept{ w[0] };
			bool bad = false;
			for ( size_t k = 1; k < stop && bad == false; )
			{
				const Token& token = w[k];
				const String word = token.quoted ? String() : token.text;
				double value = 0.0;
				if ( word == "at" )
				{
					size_t next = 0;
					String why;
					if ( ParseAt( w, k + 1, stop, p, next, why ) == false )
					{
						fail( number, why );
						bad = true;
					}
					k = next;
				}
				else if ( ( word == "delay" || word == "chance" || word == "cooldown" ) && k + 1 < stop && Number( w[k + 1], value ) )
				{
					p[word] = value;
					k += 2;
				}
				else if ( word == "wait" )
				{
					p["wait_for_server"] = true;
					k += 1;
				}
				else
				{
					kept.push_back( token );
					k += 1;
				}
			}
			if ( bad )
			{
				continue;
			}
			for ( size_t k = stop; k < w.size(); ++k )
			{
				kept.push_back( w[k] );
			}
			w = kept;
		}
		if ( verb == "sound" )
		{
			if ( w.size() < 2 )
			{
				fail( number, "sound <file>" );
				continue;
			}
			p["sound"] = w[1].text;
			if ( options( 2, p, { { "volume", "volume_db" }, { "pitch", "pitch_scale" }, { "jitter", "pitch_jitter" }, { "range", "max_distance" }, { "bus", "bus" } } ) == false )
			{
				continue;
			}
		}
		else if ( verb == "scene" )
		{
			if ( w.size() < 2 )
			{
				fail( number, "scene <file>" );
				continue;
			}
			p["scene"] = w[1].text;
			if ( options( 2, p, { { "under", "scene_parent" }, { "life", "scene_lifetime" } } ) == false )
			{
				continue;
			}
		}
		else if ( verb == "play" )
		{
			if ( w.size() < 3 )
			{
				fail( number, "play <animation player> <animation> [off <animation>]" );
				continue;
			}
			p["animation_player"] = w[1].text;
			p["animation"] = w[2].text;
			if ( options( 3, p, { { "off", "animation_off" } } ) == false )
			{
				continue;
			}
		}
		else if ( verb == "set" )
		{
			if ( w.size() < 4 )
			{
				fail( number, "set <node> <property> <value> [blend S], or set <node> <property> = <expression>" );
				continue;
			}
			p["target"] = w[1].text;
			p["property"] = w[2].text;
			if ( w[3].text == "=" && w[3].quoted == false )
			{
				if ( w.size() < 5 )
				{
					fail( number, "set <node> <property> = <expression>" );
					continue;
				}
				p["value_expression"] = RestFrom( line, w[4] );
			}
			else
			{
				p["value"] = Value( w[3] );
				if ( options( 4, p, { { "blend", "blend_time" } } ) == false )
				{
					continue;
				}
			}
		}
		else if ( verb == "call" )
		{
			if ( w.size() < 3 )
			{
				fail( number, "call <node> <method> [arguments]" );
				continue;
			}
			p["target"] = w[1].text;
			p["method"] = w[2].text;
			Array args;
			for ( size_t i = 3; i < w.size(); ++i )
			{
				args.push_back( Value( w[i] ) );
			}
			p["method_args"] = args;
		}
		else if ( verb == "shake" )
		{
			if ( w.size() < 2 || Number( w[1], number1 ) == false )
			{
				fail( number, "shake <amount 0-1> [time S]" );
				continue;
			}
			p["shake"] = number1;
			if ( options( 2, p, { { "time", "shake_time" } } ) == false )
			{
				continue;
			}
		}
		else if ( verb == "flash" )
		{
			Variant color = w.size() >= 2 ? Value( w[1] ) : Variant();
			if ( color.get_type() != Variant::COLOR )
			{
				fail( number, "flash rgba(r, g, b, a) [time S]" );
				continue;
			}
			p["flash_color"] = color;
			if ( options( 2, p, { { "time", "flash_time" } } ) == false )
			{
				continue;
			}
		}
		else
		{
			fail( number, "\"" + verb + "\" is not a setting or an action of a rule" );
			continue;
		}
		rule.actions.push_back( action );
	}
	finish();
	return out;
}

} // namespace cb::cue

namespace cb::gd
{

void CbRules::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_file", "value" ), &CbRules::set_file );
	ClassDB::bind_method( D_METHOD( "get_file" ), &CbRules::get_file );
	ClassDB::bind_method( D_METHOD( "get_reaction_count" ), &CbRules::get_reaction_count );
	ClassDB::bind_method( D_METHOD( "get_problems" ), &CbRules::get_problems );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "file", PROPERTY_HINT_FILE, "*.rules" ), "set_file", "get_file" );
}

void CbRules::set_file( const String& v )
{
	m_file = v.strip_edges();
	if ( is_inside_tree() )
	{
		Clear();
		Build();
	}
	update_configuration_warnings();
}

int64_t CbRules::get_reaction_count() const
{
	return get_child_count( true ) - get_child_count( false );
}

void CbRules::_notification( int what )
{
	if ( what == NOTIFICATION_READY && m_built == false )
	{
		Build();
	}
}

void CbRules::Clear()
{
	for ( int i = get_child_count( true ) - 1; i >= 0; --i )
	{
		Node* child = get_child( i, true );
		if ( ( Object::cast_to<CbReaction>( child ) != nullptr || Object::cast_to<CbPrediction>( child ) != nullptr ) && child->get_owner() == nullptr )
		{
			remove_child( child );
			child->queue_free();
		}
	}
	m_built = false;
}

void CbRules::Build()
{
	m_built = true;
	m_problems.clear();
	if ( m_file.is_empty() )
	{
		return;
	}
	// Only a file of the pack itself.
	if ( m_file.begins_with( "res://" ) == false || m_file.contains( ".." ) )
	{
		m_problems.push_back( "the file is not one of this project's (res://...)" );
		UtilityFunctions::push_warning( "CbRules ", get_path(), ": ", m_problems[0] );
		return;
	}
	Ref<FileAccess> file = FileAccess::open( m_file, FileAccess::READ );
	if ( file.is_null() )
	{
		m_problems.push_back( "cannot read " + m_file );
		UtilityFunctions::push_warning( "CbRules ", get_path(), ": ", m_problems[0] );
		return;
	}
	std::vector<cue::Rule> rules = cue::ParseRules( file->get_as_text(), m_problems );
	// How far a plain path may climb: no higher than the scene (the rules are written from the node
	// this one is under).
	int depth = 0;
	Node* owner = get_owner() != nullptr ? get_owner() : nullptr;
	for ( Node* n = get_parent(); n != nullptr && owner != nullptr && n != owner; n = n->get_parent() )
	{
		++depth;
	}
	for ( const cue::Rule& rule : rules )
	{
		Node* reaction = rule.prediction ? static_cast<Node*>( memnew( CbPrediction ) ) : static_cast<Node*>( memnew( CbReaction ) );
		reaction->set_name( ( rule.prediction ? "Predict" : "Rule" ) + String::num_int64( rule.line ) );
		bool ok = true;
		Array keys = rule.properties.keys();
		for ( int64_t i = 0; i < keys.size(); ++i )
		{
			String key = keys[i];
			Variant value = rule.properties[key];
			if ( cue::IsPathProperty( key ) )
			{
				String path = value;
				bool anchored = path.begins_with( "^" ) || path.begins_with( "$" ) || path.begins_with( "@" );
				if ( path.begins_with( "/" ) )
				{
					m_problems.push_back( "line " + String::num_int64( rule.line ) + ": " + key + " " + path + ": not a path from the root" );
					ok = false;
					break;
				}
				if ( anchored == false && path.is_empty() == false )
				{
					int climbs = 0;
					PackedStringArray parts = path.split( "/" );
					for ( int64_t j = 0; j < parts.size() && parts[j] == ".."; ++j )
					{
						++climbs;
					}
					if ( climbs > depth )
					{
						m_problems.push_back( "line " + String::num_int64( rule.line ) + ": " + key + " " + path + " climbs out of the scene" );
						ok = false;
						break;
					}
					// From the reaction, under this node, to the node this one is under.
					path = path == "." ? String( "../.." ) : "../../" + path;
				}
				value = NodePath( path );
			}
			reaction->set( key, value );
		}
		if ( ok == false )
		{
			memdelete( reaction );
			continue;
		}
		add_child( reaction, false, INTERNAL_MODE_BACK );
	}
	for ( int64_t i = 0; i < m_problems.size(); ++i )
	{
		UtilityFunctions::push_warning( "CbRules ", m_file, ": ", m_problems[i] );
	}
}

PackedStringArray CbRules::_get_configuration_warnings() const
{
	PackedStringArray out;
	if ( m_file.is_empty() )
	{
		out.push_back( "Give it a rules file (file): the reactions of this scene, as lines of text." );
		return out;
	}
	if ( FileAccess::file_exists( m_file ) == false )
	{
		out.push_back( "No such file: " + m_file );
		return out;
	}
	PackedStringArray errors;
	cue::ParseRules( FileAccess::get_file_as_string( m_file ), errors );
	for ( int64_t i = 0; i < errors.size(); ++i )
	{
		out.push_back( m_file.get_file() + " " + errors[i] );
	}
	return out;
}

} // namespace cb::gd
