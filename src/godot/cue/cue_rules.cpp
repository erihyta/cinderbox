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

#include <vector>

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

// How far in a line's text starts: a tab is four spaces.
int Column( const String& line )
{
	int column = 0;
	for ( int64_t i = 0; i < line.length(); ++i )
	{
		if ( line[i] == ' ' )
		{
			column += 1;
		}
		else if ( line[i] == '\t' )
		{
			column += 4;
		}
		else
		{
			break;
		}
	}
	return column;
}

// --- Where a line of a motion block may stand -------------------------------------------------------
// One table: every refusal of a line for where it stands is made from it, and the reference
// (docs/rules-language.md, "Where a line may stand") is the same table.
enum Place : uint8_t
{
	kInStep = 1,	 // a step's own lines (under an if in it too)
	kInEvery = 2,	 // after an every, in its step: what happens again and again
	kInTimeout = 4,	 // a timeout's lines: once, and the motion ends
	kInFind = 8,	 // what happens to each thing a find found
	kInCast = 16,	 // what a cast does while it holds
	kInItem = 32,	 // an item's motion (owner item): it all happens in one tick
	kItemOnly = 64,	 // (only an item's motion has it)
};

struct LineRule
{
	const char* verb;
	uint8_t places;
};

const LineRule kLineRules[] = {
	{ "change", kInStep | kInEvery | kInTimeout | kInFind | kInItem },
	{ "emit", kInStep | kInEvery | kInTimeout | kInFind | kInCast | kInItem },
	{ "impulse", kInStep | kInEvery | kInTimeout | kInFind | kInCast | kInItem },
	{ "launch", kInStep | kInEvery | kInTimeout },
	{ "break", kInStep | kInEvery | kInTimeout | kInItem },
	{ "find", kInStep | kInEvery | kInTimeout | kInItem },
	{ "if", kInStep | kInEvery | kInTimeout | kInItem },
	{ "else", kInStep | kInEvery | kInTimeout | kInItem },
	{ "move for", kInStep | kInFind | kInCast },
	{ "move", kInStep | kInCast },
	{ "force", kInStep | kInCast },
	{ "link", kInStep | kInCast },
	{ "follow", kInStep },
	{ "end", kInStep },
	{ "remove", kInStep | kInItem | kItemOnly },
	{ "cast", kInStep },
	{ "every", kInStep },
};

const LineRule* LineRuleOf( const String& verb )
{
	for ( const LineRule& rule : kLineRules )
	{
		if ( verb == rule.verb )
		{
			return &rule;
		}
	}
	return nullptr;
}

String PlaceName( uint8_t place )
{
	return place == kInEvery	 ? "after an every"
		   : place == kInTimeout ? "in a timeout"
		   : place == kInFind	 ? "in a find"
		   : place == kInCast	 ? "in a cast"
		   : place == kInItem	 ? "in an item's motion (it happens in one tick)"
								 : "in a step";
}

// Why `verb` may not stand at `place` (in an item's motion too, with `item`); empty: it may.
String Misplaced( const String& verb, uint8_t place, bool item )
{
	const LineRule* rule = LineRuleOf( verb );
	if ( rule == nullptr )
	{
		return String();
	}
	const bool here = ( rule->places & place ) != 0;
	const bool forItem = item == false || ( rule->places & kInItem ) != 0;
	const bool itemOnly = ( rule->places & kItemOnly ) == 0 || item;
	if ( here && forItem && itemOnly )
	{
		return String();
	}
	if ( itemOnly == false )
	{
		return "\"" + verb + "\" is an item's motion's (owner item)";
	}
	const uint8_t where = here ? uint8_t( kInItem ) : place;
	String list;
	std::vector<const char*> verbs;
	for ( const LineRule& other : kLineRules )
	{
		const bool fits = where == kInItem ? ( other.places & kInItem ) != 0 : ( other.places & where ) != 0 && ( other.places & kItemOnly ) == 0;
		if ( fits )
		{
			verbs.push_back( other.verb );
		}
	}
	for ( size_t k = 0; k < verbs.size(); ++k )
	{
		list += ( k == 0 ? "" : k + 1 == verbs.size() ? " or " : ", " ) + String( verbs[k] );
	}
	return "\"" + verb + "\" cannot stand " + PlaceName( where ) + ": there a line is " + list;
}

// "on target": what the block a line is in is about (a find's thing found, a cast's hold), until the
// block resolves it (ResolveTarget).
constexpr int kBlockTarget = 4;

// A part's "on target" made what its block is about: what a cast holds (1) or a find found (3).
// False outside a find or a cast.
bool ResolveTarget( Dictionary& props, bool inFind, bool inCast )
{
	if ( int( props.get( "target", 0 ) ) != kBlockTarget )
	{
		return true;
	}
	if ( inFind == false && inCast == false )
	{
		return false;
	}
	props["target"] = inCast ? 1 : 3;
	return true;
}

// A motion part (impulse, force, move, launch): its class and properties, made into a node by the
// bake. False, with why, when the line is none of them or cannot be read.
bool ParsePart( const std::vector<Token>& w, Dictionary& part, String& why )
{
	const String verb = w[0].text;
	double value = 0.0;
	Dictionary props;
	size_t i = 1;
	auto frameOf = [&]( const String& word ) {
		return word == "look" ? 0 : word == "move" ? 1 : word == "facing" ? 2 : word == "up" ? 3 : word == "world" ? 4 : word == "to" ? 5 : word == "away" ? 6 : -1;
	};
	// The words a part shares: on, along, direction.
	auto common = [&]( const String& word, size_t& k ) {
		if ( word == "on" && k + 1 < w.size() )
		{
			// self; target (what the find found, or what the cast holds: kBlockTarget until the block
			// says which); found and hit, the older words for the same; else a field naming an entity.
			String who = w[k + 1].text;
			props["target"] = who == "self" ? 0 : who == "hit" ? 1 : who == "found" ? 3 : who == "target" ? kBlockTarget : 2;
			if ( who != "self" && who != "hit" && who != "found" && who != "target" )
			{
				props["target_field"] = who;
			}
			k += 2;
			return true;
		}
		if ( word == "along" && k + 2 < w.size() && w[k + 1].text == "world" && Value( w[k + 2] ).get_type() == Variant::VECTOR3 )
		{
			// along world (x, y, z)
			props["frame"] = 4;
			props["direction"] = Value( w[k + 2] );
			k += 3;
			return true;
		}
		if ( word == "along" && k + 1 < w.size() && frameOf( w[k + 1].text ) >= 0 )
		{
			props["frame"] = frameOf( w[k + 1].text );
			k += 2;
			return true;
		}
		if ( word == "along" && k + 1 < w.size() && w[k + 1].quoted == false )
		{
			// Along a marker drawn in the scene: its forward (-Z), turned with the look.
			props["marker"] = w[k + 1].text;
			k += 2;
			return true;
		}
		if ( word == "turn" && k + 1 < w.size() )
		{
			// turn (yaw, pitch): degrees, right and up, after its frame gives the direction.
			String inside = w[k + 1].text;
			PackedStringArray two = inside.begins_with( "(" ) && inside.ends_with( ")" ) ? inside.substr( 1, inside.length() - 2 ).split( "," )
																						  : PackedStringArray();
			if ( two.size() != 2 || two[0].strip_edges().is_valid_float() == false || two[1].strip_edges().is_valid_float() == false )
			{
				why = "turn (yaw, pitch): degrees, right and up";
				k += 2;
				return true;
			}
			props["turn"] = Vector2( float( two[0].strip_edges().to_float() ), float( two[1].strip_edges().to_float() ) );
			k += 2;
			return true;
		}
		if ( word == "direction" && k + 1 < w.size() && Value( w[k + 1] ).get_type() == Variant::VECTOR3 )
		{
			props["direction"] = Value( w[k + 1] );
			k += 2;
			return true;
		}
		return false;
	};
	if ( verb == "impulse" || verb == "force" )
	{
		// How much: a number, or an expression up to the first word that is an option (read when it
		// acts: "9 * curve(Falloff, reach)", "2 * dash.power").
		size_t options = 1;
		while ( options < w.size() && w[options].quoted == false &&
				( w[options].text == "on" || w[options].text == "along" || w[options].text == "direction" || w[options].text == "replace" ||
				  w[options].text == "kind" || w[options].text == "top" || w[options].text == "ramp" || w[options].text == "react" ) == false )
		{
			++options;
		}
		if ( options < 2 )
		{
			why = verb + String( " <how much> ..." );
			return false;
		}
		part["class"] = verb == "impulse" ? "CbImpulse" : "CbForce";
		if ( options == 2 && Number( w[1], value ) )
		{
			props[verb == "impulse" ? "speed" : "strength"] = value;
		}
		else
		{
			String expression;
			for ( size_t k = 1; k < options; ++k )
			{
				expression += ( k > 1 ? " " : "" ) + w[k].text;
			}
			props[verb == "impulse" ? "speed" : "strength"] = 0.0;
			props["value_expression"] = expression;
		}
		for ( i = options; i < w.size() && why.is_empty(); )
		{
			const String word = w[i].text;
			if ( common( word, i ) )
			{
				continue;
			}
			if ( verb == "impulse" && word == "replace" && i + 1 < w.size() )
			{
				String r = w[i + 1].text;
				int replace = r == "nothing" ? 0 : r == "vertical" ? 1 : r == "horizontal" ? 2 : r == "all" ? 3 : -1;
				if ( replace < 0 )
				{
					why = "replace nothing, vertical, horizontal or all";
				}
				props["replace"] = replace;
				i += 2;
			}
			else if ( verb == "impulse" && word == "kind" && i + 1 < w.size() )
			{
				// kind velocity (the default: m/s, whatever it weighs) or momentum (N s: heavy moves less)
				String k = w[i + 1].text;
				if ( k != "velocity" && k != "momentum" )
				{
					why = "kind velocity or momentum";
				}
				props["momentum"] = k == "momentum";
				i += 2;
			}
			else if ( verb == "force" && word == "kind" && i + 1 < w.size() )
			{
				String k = w[i + 1].text;
				int kind = k == "acceleration" ? 0 : k == "force" ? 1 : k == "velocity" ? 2 : -1;
				if ( kind < 0 )
				{
					why = "kind acceleration, force or velocity";
				}
				props["kind"] = kind;
				i += 2;
			}
			else if ( verb == "force" && ( word == "top" || word == "ramp" ) && i + 1 < w.size() && Number( w[i + 1], value ) )
			{
				props[word == "top" ? "speed" : "ramp_in"] = value;
				i += 2;
			}
			else if ( verb == "force" && word == "react" )
			{
				props["react"] = true;
				i += 1;
			}
			else
			{
				why = "\"" + word + "\" is not something " + verb + " takes";
			}
		}
	}
	else if ( verb == "link" )
	{
		// link [length L] [reel S] [alone] [on ...]: a rope to what the cast holds (or a field's entity)
		part["class"] = "CbLink";
		for ( i = 1; i < w.size() && why.is_empty(); )
		{
			const String word = w[i].text;
			if ( word == "on" && common( word, i ) )
			{
				continue;
			}
			if ( ( word == "length" || word == "reel" ) && i + 1 < w.size() && Number( w[i + 1], value ) )
			{
				props[word] = value;
				i += 2;
			}
			else if ( word == "alone" )
			{
				props["react"] = false; // (it moves the player alone: what it holds is as the world)
				i += 1;
			}
			else
			{
				why = "\"" + word + "\" is not something link takes: length L, reel S, alone, on ...";
			}
		}
	}
	else if ( verb == "move" )
	{
		part["class"] = "CbMove";
		Dictionary parameters;
		i = 1;
		if ( i + 1 < w.size() && w[i].text == "for" && Number( w[i + 1], value ) )
		{
			props["duration"] = value;
			i += 2;
		}
		for ( ; i + 1 < w.size() && why.is_empty(); i += 2 )
		{
			if ( Number( w[i + 1], value ) == false )
			{
				why = w[i].text + String( " wants a number" );
			}
			parameters[w[i].text] = value;
		}
		if ( i < w.size() && why.is_empty() )
		{
			why = "move [for S] PARAMETER VALUE ...";
		}
		props["parameters"] = parameters;
	}
	else if ( verb == "launch" )
	{
		part["class"] = "CbLaunch";
		if ( w.size() >= 2 && w[1].text == "carried" )
		{
			props["what"] = 1;
			i = 2;
		}
		else if ( w.size() >= 3 && w[1].text == "new" )
		{
			props["what"] = 0;
			props["item_kind"] = w[2].text;
			i = 3;
		}
		else
		{
			why = "launch new <kind>, or launch carried";
		}
		while ( i < w.size() && why.is_empty() )
		{
			const String word = w[i].text;
			if ( common( word, i ) )
			{
				continue;
			}
			if ( ( word == "speed" || word == "lift" || word == "ahead" || word == "seconds" ) && i + 1 < w.size() && Number( w[i + 1], value ) )
			{
				props[word] = value;
				i += 2;
			}
			else if ( word == "speed" && i + 1 < w.size() && w[i + 1].text.begins_with( "(" ) )
			{
				// speed (EXPRESSION): read when it is thrown.
				props["speed"] = 0.0;
				props["value_expression"] = w[i + 1].text;
				i += 2;
			}
			else if ( word == "from" && i + 1 < w.size() && w[i + 1].quoted == false )
			{
				props["from"] = w[i + 1].text; // (a marker's place: where it starts)
				i += 2;
			}
			else
			{
				why = "\"" + word + "\" is not something launch takes";
			}
		}
	}
	else
	{
		why = "\"" + verb + "\" is not one of: change, cooldown, emit, impulse, force, link, move, launch";
	}
	if ( why.is_empty() == false )
	{
		return false;
	}
	part["properties"] = props;
	return true;
}

struct Building
{
	int line = 0;
	Dictionary settings;			 // when it is (on, while, its conditions); a prediction: all of it
	Dictionary current;				 // the settings read so far: each action below takes them as they are
	int pendingSetting = 0;			 // the line of a setting no action has come after yet
	std::vector<Rule> actions;		 // its own properties, and its line
	bool prediction = false;
	// A motion block (for the bake: run by the simulation). Its steps; the ifs open around the line
	// being read (their column, their condition, whether it is their else now); its lines' columns.
	bool motion = false;
	Array steps;
	struct Frame
	{
		int column = 0;
		String condition;
		bool otherwise = false;
	};
	std::vector<Frame> frames;
	int headColumn = 0;
	int bodyColumn = -1;
	// The find being read (its column; -1: none): the lines further in are what happens to each thing
	// it found.
	int findColumn = -1;
	Dictionary find;
	bool castBlock = false; // the open block is a cast (else a find)
	bool cast = false;		// the motion has one
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
		if ( rule.prediction == false && rule.pendingSetting != 0 )
		{
			errors.push_back( "line " + String::num_int64( rule.pendingSetting ) +
							  ": a setting after the last action does nothing (a setting is for the actions below it)" );
			return;
		}
		if ( rule.motion )
		{
			// An item's motion (owner item) happens in one tick: what it may have is checked as a whole,
			// since "owner item" may come after its first lines.
			if ( String( rule.settings.get( "owner", "player" ) ) == "item" )
			{
				const char* once = "an item's motion (owner item) happens in one tick, on an event said at it: ";
				if ( rule.steps.size() > 1 )
				{
					errors.push_back( "line " + String::num_int64( int64_t( Dictionary( rule.steps[1] ).get( "line", rule.line ) ) ) + ": " + String( once ) +
									  "no then, after or timeout" );
					return;
				}
				for ( const char* setting : { "cooldown", "uses", "lasts", "until", "conditions" } )
				{
					if ( rule.settings.has( setting ) )
					{
						errors.push_back( "line " + String::num_int64( rule.line ) + ": " + String( once ) + "no " +
										  ( String( setting ) == "conditions" ? String( "if on its first line (an if block is fine)" ) : String( setting ) ) );
						return;
					}
				}
				Array list = Dictionary( rule.steps[0] ).get( "statements", Array() );
				for ( int64_t k = 0; k < list.size(); ++k )
				{
					Dictionary statement = list[k];
					const String misplaced = Misplaced( statement.get( "verb", "" ), kInStep, true );
					if ( misplaced.is_empty() == false )
					{
						errors.push_back( "line " + String::num_int64( int64_t( statement.get( "line", rule.line ) ) ) + ": " + misplaced );
						return;
					}
				}
			}
			else
			{
				for ( int64_t s = 0; s < rule.steps.size(); ++s )
				{
					Array list = Dictionary( rule.steps[s] ).get( "statements", Array() );
					for ( int64_t k = 0; k < list.size(); ++k )
					{
						Dictionary statement = list[k];
						if ( String( statement.get( "verb", "" ) ) == "remove" )
						{
							errors.push_back( "line " + String::num_int64( int64_t( statement.get( "line", rule.line ) ) ) + ": " +
											  Misplaced( "remove", kInStep, false ) );
							return;
						}
					}
				}
			}
			Rule made;
			made.line = rule.line;
			made.type = "motion";
			made.properties = rule.settings;
			made.properties["steps"] = rule.steps;
			out.push_back( made );
			return;
		}
		if ( rule.prediction )
		{
			Rule made;
			made.line = rule.line;
			made.prediction = true;
			made.type = "CbPrediction";
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
			made.head = rule.line;
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
		// --- a HUD's line: show, text, bind (a CbFieldBinding each, on a control of the HUD)
		if ( verb == "show" || verb == "text" || verb == "bind" )
		{
			finish();
			// "... if <expression>": the condition, to the end of the line.
			size_t cut = w.size();
			for ( size_t k = 2; k < w.size(); ++k )
			{
				if ( w[k].quoted == false && w[k].text == "if" )
				{
					cut = k;
					break;
				}
			}
			Rule made;
			made.line = number;
			made.type = "CbFieldBinding";
			Dictionary& p = made.properties;
			if ( cut < w.size() )
			{
				if ( cut + 1 >= w.size() )
				{
					errors.push_back( "line " + String::num_int64( number ) + ": if what? if <expression>" );
					continue;
				}
				p["conditions"] = PackedStringArray( { RestFrom( line, w[cut + 1] ) } );
			}
			String why;
			if ( w.size() < 2 )
			{
				why = verb + String( " what? " ) + ( verb == "show" ? "show <node> if <expression>" : verb == "text" ? "text <node> \"format\" [into <property>] [if <expression>]" : "bind <node> <property> = <expression> [if <expression>]" );
			}
			else if ( verb == "show" )
			{
				if ( cut != 2 || p.has( "conditions" ) == false )
				{
					why = "show <node> if <expression>";
				}
			}
			else if ( verb == "text" )
			{
				p["hides"] = false;
				if ( cut < 3 )
				{
					why = "text <node> \"format\" [into <property>] [if <expression>]";
				}
				else
				{
					p["text_format"] = w[2].text;
					if ( cut == 5 && w[3].text == "into" )
					{
						p["property"] = w[4].text;
					}
					else if ( cut != 3 )
					{
						why = "after the format: into <property>, or if <expression>";
					}
				}
			}
			else
			{
				p["hides"] = false;
				if ( cut < 5 || w[3].text != "=" )
				{
					why = "bind <node> <property> = <expression> [if <expression>]";
				}
				else
				{
					p["property"] = w[2].text;
					// (The expression, up to "if" or the end of the line.)
					String expression = RestFrom( line, w[4] );
					if ( cut < w.size() )
					{
						expression = line.substr( w[4].start, w[cut].start - w[4].start ).strip_edges();
					}
					p["field"] = expression;
				}
			}
			if ( why.is_empty() == false )
			{
				errors.push_back( "line " + String::num_int64( number ) + ": " + why );
				continue;
			}
			p["target"] = w[1].text;
			out.push_back( made );
			continue;
		}
		// --- a motion block: for the bake, run by the simulation (docs/rules-language.md)
		if ( verb == "motion" )
		{
			finish();
			rule = Building{};
			rule.line = number;
			rule.motion = true;
			rule.headColumn = Column( line );
			open = true;
			Dictionary first;
			first["statements"] = Array();
			rule.steps.push_back( first );
			// motion NAME on use | on press ACTION | on EVENT | while EXPR | when EXPR, then [if EXPR]
			const char* usage = "motion NAME on use, on press ACTION, on EVENT, while EXPRESSION or when EXPRESSION, then [if EXPRESSION]";
			if ( w.size() < 3 || w[1].text.contains( "/" ) || w[1].text.contains( "." ) || w[1].quoted )
			{
				fail( number, usage );
				continue;
			}
			rule.settings["name"] = w[1].text;
			const String how = w[2].text;
			size_t cut = w.size();
			for ( size_t k = 3; k < w.size(); ++k )
			{
				if ( w[k].quoted == false && w[k].text == "if" )
				{
					cut = k;
					break;
				}
			}
			if ( how == "on" && w.size() >= 4 && w[3].text == "use" && cut == 4 )
			{
				rule.settings["trigger"] = "use";
			}
			else if ( how == "on" && w.size() >= 5 && w[3].text == "press" && cut == 5 )
			{
				rule.settings["trigger"] = "press";
				rule.settings["action"] = w[4].text;
			}
			else if ( how == "on" && cut == 4 )
			{
				rule.settings["trigger"] = "event";
				rule.settings["event"] = w[3].text;
			}
			else if ( ( how == "while" || how == "when" ) && cut > 3 )
			{
				rule.settings["trigger"] = how;
				rule.settings["expression"] = line.substr( w[3].start, ( cut < w.size() ? w[cut].start : line.length() ) - w[3].start ).strip_edges();
				int64_t hash = String( rule.settings["expression"] ).find( "#" );
				if ( hash >= 0 )
				{
					rule.settings["expression"] = String( rule.settings["expression"] ).substr( 0, hash ).strip_edges();
				}
			}
			else
			{
				fail( number, usage );
				continue;
			}
			if ( cut < w.size() )
			{
				if ( cut + 1 >= w.size() || how == "while" )
				{
					fail( number, how == "while" ? String( "a while's expression is its condition: no if" ) : String( "if what? if <expression>" ) );
					continue;
				}
				rule.settings["conditions"] = PackedStringArray( { RestFrom( line, w[cut + 1] ) } );
			}
			continue;
		}
		if ( open && rule.motion && verb != "on" && verb != "while" && verb != "predict" && verb != "use" && verb != "show" &&
			 verb != "text" && verb != "bind" )
		{
			const int column = Column( line );
			if ( column <= rule.headColumn )
			{
				fail( number, "a motion's lines are indented under it" );
				continue;
			}
			rule.bodyColumn = rule.bodyColumn < 0 ? column : rule.bodyColumn;
			// --- inside a find: what happens to each thing it found (an impulse, a move for, an emit)
			if ( rule.findColumn >= 0 && column <= rule.findColumn )
			{
				rule.findColumn = -1;
			}
			if ( rule.findColumn >= 0 )
			{
				Array inside = rule.find["statements"];
				Dictionary statement;
				statement["line"] = number;
				const String placed = verb == "move" && w.size() > 1 && w[1].text == "for" ? String( "move for" ) : verb;
				const String misplaced = Misplaced( placed, rule.castBlock ? kInCast : kInFind, false );
				if ( misplaced.is_empty() == false )
				{
					fail( number, misplaced );
					continue;
				}
				statement["verb"] = placed;
				if ( verb == "change" )
				{
					// (Of the motion's owner, once for each thing found: a count of what was hit.)
					if ( w.size() < 4 )
					{
						fail( number, "change FIELD = VALUE, += or -=" );
						continue;
					}
					statement["kind"] = "change";
					statement["text"] = RestFrom( line, w[1] );
				}
				else if ( verb == "emit" && w.size() == 2 )
				{
					if ( rule.castBlock && inside.size() > 0 )
					{
						bool emitted = false;
						for ( int64_t k = 0; k < inside.size(); ++k )
						{
							emitted = emitted || String( Dictionary( inside[k] ).get( "kind", "" ) ) == "emit";
						}
						if ( emitted )
						{
							fail( number, "a cast says one event when it takes hold" );
							continue;
						}
					}
					statement["kind"] = "emit";
					statement["text"] = w[1].text;
				}
				else
				{
					Dictionary part;
					String why;
					if ( ParsePart( w, part, why ) == false )
					{
						fail( number, why );
						continue;
					}
					Dictionary props = part["properties"];
					if ( ( verb == "impulse" || ( rule.castBlock && verb == "force" ) ) && props.has( "target" ) == false )
					{
						props["target"] = rule.castBlock ? 1 : 3; // (what it holds, or found, unless it says)
					}
					ResolveTarget( props, rule.castBlock == false, rule.castBlock );
					statement["kind"] = "part";
					statement["part"] = part;
				}
				inside.push_back( statement );
				continue;
			}
			// The ifs this line is out of are closed (an else stays in its if).
			while ( rule.frames.empty() == false &&
					( verb == "else" ? rule.frames.back().column > column : rule.frames.back().column >= column ) )
			{
				rule.frames.pop_back();
			}
			const bool top = column <= rule.bodyColumn && rule.frames.empty();
			Dictionary step = rule.steps[rule.steps.size() - 1];
			Array statements = step["statements"];
			// What the line is under: every open if (its else: not it), as one condition.
			auto guard = [&]() {
				String all;
				for ( const auto& frame : rule.frames )
				{
					String one = frame.otherwise ? "not ( " + frame.condition + " )" : "( " + frame.condition + " )";
					all += all.is_empty() ? one : " and " + one;
				}
				return all;
			};
			auto add = [&]( Dictionary statement ) {
				statement["guard"] = guard();
				statement["line"] = number;
				statements.push_back( statement );
			};
			double value = 0.0;
			if ( verb == "if" )
			{
				if ( w.size() < 2 )
				{
					fail( number, "if <expression>" );
					continue;
				}
				rule.frames.push_back( { column, RestFrom( line, w[1] ), false } );
				continue;
			}
			if ( verb == "else" )
			{
				if ( w.size() != 1 || rule.frames.empty() || rule.frames.back().column != column || rule.frames.back().otherwise )
				{
					fail( number, "an else is under an if, as far in as it" );
					continue;
				}
				rule.frames.back().otherwise = true;
				continue;
			}
			// --- what starts a step, or the motion's own settings: not under an if
			const bool stepLine = verb == "then" || verb == "after" || verb == "timeout";
			if ( verb == "every" )
			{
				// every S: what follows in this step, again and again while it is the step
				double seconds = 0.0;
				bool again = false;
				for ( int64_t k = 0; k < statements.size(); ++k )
				{
					again = again || String( Dictionary( statements[k] ).get( "kind", "" ) ) == "every";
				}
				if ( bool( step.get( "timeout", false ) ) || again )
				{
					fail( number, Misplaced( "every", again ? kInEvery : kInTimeout, false ) );
					continue;
				}
				if ( top == false || w.size() != 2 || Number( w[1], seconds ) == false || seconds <= 0.0 )
				{
					fail( number, "every S: as far in as the motion's first line, not under an if; what follows in its step happens every S seconds" );
					continue;
				}
				Dictionary every;
				every["kind"] = "every";
				every["verb"] = "every";
				every["seconds"] = seconds;
				every["line"] = number;
				every["guard"] = String();
				statements.push_back( every );
				step["statements"] = statements;
				rule.steps[rule.steps.size() - 1] = step;
				continue;
			}
			const bool setting = verb == "cooldown" || verb == "uses" || verb == "lasts" || verb == "until" || verb == "item" || verb == "owner";
			if ( ( stepLine || setting ) && top == false )
			{
				fail( number, "\"" + verb + "\" is the motion's: as far in as its first line, not under an if" );
				continue;
			}
			if ( stepLine )
			{
				Dictionary next;
				next["statements"] = Array();
				next["line"] = number;
				if ( verb == "timeout" )
				{
					if ( w.size() != 1 || double( step.get( "within", 0.0 ) ) <= 0.0 || bool( step.get( "timeout", false ) ) )
					{
						fail( number, "a timeout comes right after a then when ... within S" );
						continue;
					}
					next["timeout"] = true;
				}
				else if ( verb == "after" )
				{
					if ( w.size() != 2 || Number( w[1], value ) == false || value < 0.0 )
					{
						fail( number, "after <seconds>" );
						continue;
					}
					next["after"] = value;
				}
				else
				{
					// then when EXPRESSION [within S]
					size_t end = w.size();
					if ( w.size() >= 4 && w[w.size() - 2].text == "within" )
					{
						if ( Number( w[w.size() - 1], value ) == false || value <= 0.0 )
						{
							fail( number, "within <seconds>" );
							continue;
						}
						next["within"] = value;
						end = w.size() - 2;
					}
					if ( w.size() < 3 || w[1].text != "when" || end < 3 )
					{
						fail( number, "then when <expression> [within S]" );
						continue;
					}
					next["wait"] = line.substr( w[2].start, w[end - 1].start + w[end - 1].text.length() - w[2].start ).strip_edges();
				}
				step["statements"] = statements;
				rule.steps[rule.steps.size() - 1] = step;
				rule.steps.push_back( next );
				continue;
			}
			if ( setting )
			{
				if ( rule.steps.size() > 1 )
				{
					fail( number, "\"" + verb + "\" is the motion's: before its first then or after" );
					continue;
				}
				if ( verb == "until" && w.size() >= 2 )
				{
					PackedStringArray until = rule.settings.get( "until", PackedStringArray() );
					until.push_back( RestFrom( line, w[1] ) );
					rule.settings["until"] = until;
				}
				else if ( ( verb == "cooldown" || verb == "lasts" ) && w.size() == 2 && Number( w[1], value ) && value >= 0.0 )
				{
					rule.settings[verb] = value;
				}
				else if ( verb == "uses" && w.size() >= 2 && w[1].text.is_valid_int() && w[1].text.to_int() > 0 &&
						  ( w.size() == 2 || ( w.size() == 4 && w[2].text == "refill" && w[3].text == "ground" ) ||
							( w.size() == 5 && w[2].text == "refill" && w[3].text == "after" && Number( w[4], value ) && value > 0.0 ) ) )
				{
					rule.settings["uses"] = w[1].text.to_int();
					rule.settings["refill"] = w.size() == 4 ? Variant( "ground" ) : Variant( w.size() == 5 ? value : 0.0 );
				}
				else if ( verb == "item" && w.size() == 2 && ( w[1].text == "carried" || w[1].text == "hand" ) )
				{
					rule.settings["item"] = w[1].text;
				}
				else if ( verb == "owner" && w.size() == 2 && ( w[1].text == "item" || w[1].text == "player" ) )
				{
					rule.settings["owner"] = w[1].text;
				}
				else
				{
					fail( number, verb == "until"	 ? "until <expression>"
								  : verb == "uses" ? "uses N, uses N refill ground, or uses N refill after S"
								  : verb == "item" ? "item hand (the default: whoever has it out) or item carried (in the hand or put away)"
								  : verb == "owner" ? "owner item (the item runs it, lying in the world) or owner player (the default)"
												   : verb + String( " <seconds>" ) );
				}
				continue;
			}
			// --- statements, where they may stand (the table above): a step's own, after its every, a
			// timeout's (an item's motion is checked as a whole when it ends)
			Dictionary statement;
			{
				bool afterEvery = false;
				for ( int64_t k = 0; k < statements.size(); ++k )
				{
					afterEvery = afterEvery || String( Dictionary( statements[k] ).get( "kind", "" ) ) == "every";
				}
				const uint8_t place = bool( step.get( "timeout", false ) ) ? kInTimeout : afterEvery ? kInEvery : kInStep;
				const String placed = verb == "move" && w.size() > 1 && w[1].text == "for" ? String( "move for" ) : verb;
				const bool ownerItem = String( rule.settings.get( "owner", "player" ) ) == "item";
				// (An item's own line, before "owner item" was said, is checked when the motion ends.)
				const String misplaced = placed == "remove" ? String() : Misplaced( placed, place, ownerItem );
				if ( misplaced.is_empty() == false )
				{
					fail( number, misplaced );
					continue;
				}
				statement["verb"] = placed;
			}
			if ( verb == "find" )
			{
				// find SHAPE [players] [bodies] [self] [max N] [turned facing|look|none] [at (X,Y,Z)]; SHAPE: a node's
				// path (an Area3D, a CollisionShape3D), sphere R, box (X,Y,Z), capsule R H
				const char* usage = "find SHAPE [players] [bodies] [self] [max N] [turned facing|look|none] [at (X,Y,Z)], SHAPE a node's path, sphere R, box (X,Y,Z) or capsule R H";
				size_t i = 1;
				Dictionary shape;
				double a = 0.0, b = 0.0;
				if ( w.size() >= 3 && w[1].text == "sphere" && Number( w[2], a ) && a > 0.0 )
				{
					shape["kind"] = "sphere";
					shape["radius"] = a;
					i = 3;
				}
				else if ( w.size() >= 3 && w[1].text == "box" && Value( w[2] ).get_type() == Variant::VECTOR3 )
				{
					shape["kind"] = "box";
					shape["size"] = Value( w[2] );
					i = 3;
				}
				else if ( w.size() >= 4 && w[1].text == "capsule" && Number( w[2], a ) && Number( w[3], b ) && a > 0.0 && b >= 2.0 * a )
				{
					shape["kind"] = "capsule";
					shape["radius"] = a;
					shape["height"] = b;
					i = 4;
				}
				else if ( w.size() >= 2 && w[1].quoted == false && w[1].text != "sphere" && w[1].text != "box" && w[1].text != "capsule" )
				{
					shape["kind"] = "node";
					shape["path"] = w[1].text;
					i = 2;
				}
				else
				{
					fail( number, usage );
					continue;
				}
				bool players = false, bodies = false, ok = true;
				for ( ; i < w.size() && ok; )
				{
					const String word = w[i].text;
					if ( word == "players" || word == "bodies" )
					{
						( word == "players" ? players : bodies ) = true;
						i += 1;
					}
					else if ( word == "self" )
					{
						statement["self"] = true;
						i += 1;
					}
					else if ( word == "max" && i + 1 < w.size() && w[i + 1].text.is_valid_int() && w[i + 1].text.to_int() >= 1 && w[i + 1].text.to_int() <= 16 )
					{
						statement["most"] = w[i + 1].text.to_int();
						i += 2;
					}
					else if ( ( word == "turned" && i + 1 < w.size() && ( w[i + 1].text == "facing" || w[i + 1].text == "look" || w[i + 1].text == "none" ) ) ||
							  ( word == "along" && i + 1 < w.size() && ( w[i + 1].text == "facing" || w[i + 1].text == "look" || w[i + 1].text == "world" ) ) )
					{
						// turned facing (the default), look (with the pitch) or none; "along" the older word
						statement["frame"] = w[i + 1].text == "none" ? String( "world" ) : w[i + 1].text;
						i += 2;
					}
					else if ( word == "at" && i + 1 < w.size() && Value( w[i + 1] ).get_type() == Variant::VECTOR3 )
					{
						// at (x, y, z): moved from where it is placed, in the player's frame (metres right,
						// up and ahead)
						shape["at"] = Value( w[i + 1] );
						i += 2;
					}
					else
					{
						ok = false;
					}
				}
				if ( ok == false )
				{
					fail( number, usage );
					continue;
				}
				statement["kind"] = "find";
				statement["shape"] = shape;
				statement["who"] = players == bodies ? "all" : players ? "players" : "bodies";
				statement["statements"] = Array();
				add( statement );
				step["statements"] = statements;
				rule.steps[rule.steps.size() - 1] = step;
				rule.findColumn = column;
				rule.find = statement;
				rule.castBlock = false;
				continue;
			}
			if ( verb == "cast" )
			{
				// cast SHAPE range R [speed V] [holds level|bodies|players ...]; SHAPE: ray, sphere R, or a
				// node's path (one convex shape, around its origin)
				const char* usage = "cast SHAPE range R [speed V] [holds level bodies players], SHAPE ray, sphere R, or a node's path";
				if ( rule.steps.size() > 1 || rule.frames.empty() == false || rule.cast )
				{
					fail( number, rule.cast ? "a motion casts once" : "a cast is thrown when the motion starts: in its first step, not under an if" );
					continue;
				}
				size_t i = 1;
				Dictionary shape;
				double a = 0.0;
				if ( w.size() >= 2 && w[1].text == "ray" )
				{
					shape["kind"] = "ray";
					i = 2;
				}
				else if ( w.size() >= 3 && w[1].text == "sphere" && Number( w[2], a ) && a > 0.0 )
				{
					shape["kind"] = "sphere";
					shape["radius"] = a;
					i = 3;
				}
				else if ( w.size() >= 2 && w[1].quoted == false && w[1].text != "range" )
				{
					shape["kind"] = "node";
					shape["path"] = w[1].text;
					i = 2;
				}
				else
				{
					fail( number, usage );
					continue;
				}
				PackedStringArray holds;
				bool ok = true;
				for ( ; i < w.size() && ok; )
				{
					const String word = w[i].text;
					if ( ( word == "range" || word == "speed" ) && i + 1 < w.size() && Number( w[i + 1], a ) && a >= 0.0 )
					{
						statement[word] = a;
						i += 2;
					}
					else if ( word == "holds" )
					{
						i += 1;
						for ( ; i < w.size() && ( w[i].text == "level" || w[i].text == "bodies" || w[i].text == "players" ); ++i )
						{
							holds.push_back( w[i].text );
						}
						ok = holds.is_empty() == false;
					}
					else
					{
						ok = false;
					}
				}
				if ( ok == false || statement.has( "range" ) == false )
				{
					fail( number, usage );
					continue;
				}
				statement["kind"] = "cast";
				statement["shape"] = shape;
				statement["holds"] = holds;
				statement["statements"] = Array();
				add( statement );
				step["statements"] = statements;
				rule.steps[rule.steps.size() - 1] = step;
				rule.findColumn = column;
				rule.find = statement;
				rule.castBlock = true;
				rule.cast = true;
				continue;
			}
			if ( verb == "follow" )
			{
				// follow PATH over S: carried along a Path3D drawn in the scene
				double seconds = 0.0;
				if ( w.size() != 4 || w[1].quoted || w[2].text != "over" || Number( w[3], seconds ) == false || seconds <= 0.0 )
				{
					fail( number, "follow PATH over S: a Path3D beside the sheet, and the seconds it takes" );
					continue;
				}
				statement["kind"] = "follow";
				statement["path"] = w[1].text;
				statement["seconds"] = seconds;
				add( statement );
				step["statements"] = statements;
				rule.steps[rule.steps.size() - 1] = step;
				continue;
			}
			if ( verb == "break" )
			{
				// break radius R [speed S] [seconds S]: walls near the owner. A number, a name, or an
				// expression in brackets each.
				bool ok = w.size() >= 3 && w.size() % 2 == 1;
				for ( size_t k = 1; ok && k + 1 < w.size(); k += 2 )
				{
					const String word = w[k].text;
					ok = ( word == "radius" || word == "speed" || word == "seconds" ) && w[k + 1].quoted == false;
					statement[word] = w[k + 1].text;
				}
				if ( ok == false || statement.has( "radius" ) == false )
				{
					fail( number, "break radius R [speed S] [seconds S]: the walls within R of it, their pieces flung at S, staying S seconds (0: for good)" );
					continue;
				}
				statement["kind"] = "break";
				add( statement );
				step["statements"] = statements;
				rule.steps[rule.steps.size() - 1] = step;
				continue;
			}
			if ( verb == "remove" )
			{
				if ( w.size() != 2 || w[1].text != "self" )
				{
					fail( number, "remove self (an item's motion: the item leaves the world)" );
					continue;
				}
				statement["kind"] = "remove";
				add( statement );
				step["statements"] = statements;
				rule.steps[rule.steps.size() - 1] = step;
				continue;
			}
			if ( verb == "change" )
			{
				if ( w.size() < 4 )
				{
					fail( number, "change FIELD = VALUE, += or -=" );
					continue;
				}
				statement["kind"] = "change";
				statement["text"] = RestFrom( line, w[1] );
			}
			else if ( verb == "emit" )
			{
				if ( w.size() != 2 )
				{
					fail( number, "emit <event>" );
					continue;
				}
				statement["kind"] = "emit";
				statement["text"] = w[1].text;
			}
			else if ( verb == "end" )
			{
				if ( w.size() != 1 )
				{
					fail( number, "end (alone on its line)" );
					continue;
				}
				statement["kind"] = "end";
			}
			else
			{
				Dictionary part;
				String why;
				if ( ParsePart( w, part, why ) == false )
				{
					fail( number, why.begins_with( "\"" + verb + "\" is not one of" )
									  ? "\"" + verb + "\" is not a line of a motion: if, else, then when, after, timeout, cooldown, uses, lasts, until, item, owner, change, emit, end, every, find, cast, impulse, force, link, move, launch, follow, break, remove"
									  : why );
					continue;
				}
				Dictionary props = part["properties"];
				if ( ResolveTarget( props, false, false ) == false )
				{
					fail( number, "on target is what a find found or what a cast holds: inside one" );
					continue;
				}
				statement["kind"] = "part";
				statement["part"] = part;
			}
			add( statement );
			step["statements"] = statements;
			rule.steps[rule.steps.size() - 1] = step;
			continue;
		}
		// --- an item's use: "use [if EXPRESSION]" is "motion Use on use [if EXPRESSION]", word for word
		if ( verb == "use" )
		{
			finish();
			rule = Building{};
			rule.line = number;
			rule.motion = true;
			rule.headColumn = Column( line );
			open = true;
			Dictionary first;
			first["statements"] = Array();
			rule.steps.push_back( first );
			rule.settings["name"] = "Use";
			rule.settings["trigger"] = "use";
			if ( w.size() >= 2 )
			{
				if ( w[1].text != "if" || w.size() < 3 )
				{
					fail( number, "use [if <expression>]" );
					continue;
				}
				rule.settings["conditions"] = PackedStringArray( { RestFrom( line, w[2] ) } );
			}
			continue;
		}
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
			rule.current[verb == "subject" ? "subject" : verb == "kind" ? "subject_kind" : "subject_template"] = w[1].text;
			rule.pendingSetting = number;
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
			rule.current["event_side"] = side == "a" ? 0 : side == "b" ? 1 : 2;
			rule.pendingSetting = number;
			continue;
		}
		if ( verb == "delay" || verb == "chance" || verb == "cooldown" )
		{
			if ( w.size() != 2 || Number( w[1], number1 ) == false )
			{
				fail( number, verb + String( " takes a number" ) );
				continue;
			}
			rule.current[verb] = number1;
			rule.pendingSetting = number;
			continue;
		}
		if ( verb == "wait" )
		{
			if ( w.size() > 2 || ( w.size() == 2 && w[1].text != "off" ) )
			{
				fail( number, "wait, or wait off" );
				continue;
			}
			rule.current["wait_for_server"] = w.size() == 1;
			rule.pendingSetting = number;
			continue;
		}
		if ( verb == "at" )
		{
			size_t next = 0;
			String why;
			// (A new place: what the last one said about a node or an offset goes with it.)
			rule.current.erase( "place_node" );
			rule.current.erase( "offset" );
			if ( w.size() == 2 && w[1].text == "here" )
			{
				rule.current["place"] = 0; // where the sheet's node is (under its parent)
			}
			else if ( ParseAt( w, 1, w.size(), rule.current, next, why ) == false || next != w.size() )
			{
				fail( number, why.is_empty() ? String( "after where: from PATH, offset X Y Z" ) : why );
				continue;
			}
			rule.pendingSetting = number;
			continue;
		}
		// --- actions
		Rule action;
		action.line = number;
		Dictionary& p = action.properties;
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
		// The settings read so far, as they are now: a setting below it is for the actions after it.
		Dictionary taken = rule.current.duplicate();
		taken.merge( action.properties, true );
		action.properties = taken;
		rule.pendingSetting = 0;
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
		if ( child->get_owner() == nullptr )
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
		if ( rule.type == "use" || rule.type == "motion" )
		{
			continue; // (baked with the item or the motion set: motions the simulation runs)
		}
		Node* reaction = rule.type == "CbReaction"	   ? static_cast<Node*>( memnew( CbReaction ) )
						 : rule.type == "CbPrediction" ? static_cast<Node*>( memnew( CbPrediction ) )
													   : Object::cast_to<Node>( ClassDB::instantiate( rule.type ) );
		if ( reaction == nullptr )
		{
			m_problems.push_back( "line " + String::num_int64( rule.line ) + ": this game has no " + rule.type );
			continue;
		}
		reaction->set_name( rule.type.substr( 2 ) + String::num_int64( rule.line ) );
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
