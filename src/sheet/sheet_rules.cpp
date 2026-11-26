#include "sheet_rules.h"

#include "sheet_text.h"
#include "sheet_tree.h"

#include <algorithm>
#include <utility>
#include <vector>

// The sheet compiler's lowering (M293, step 6.5): the tree of a version 1 sheet (sheet_tree.h) into
// the rules the engine runs. The words of a line are read by the helpers below (moved from
// src/godot/cue/cue_rules.cpp in M283); what a line governs is what is under it in the tree.

namespace cb::sheet
{

namespace
{

std::string Num64( int64_t n )
{
	return std::to_string( n );
}

// A word of a line, and where it starts (so "the rest of the line" can be taken as it was written).
struct Token
{
	std::string text;
	size_t start = 0;
	bool quoted = false;
};

// The words of a line: spaces part them, double quotes keep spaces in one, # starts a comment.
std::vector<Token> SplitLine( const std::string& line )
{
	std::vector<Token> out;
	size_t i = 0;
	const size_t n = line.size();
	while ( i < n )
	{
		const char c = line[i];
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
			size_t close = line.find( '"', i + 1 );
			close = close == std::string::npos ? n : close;
			token.text = line.substr( i + 1, close - i - 1 );
			token.quoted = true;
			i = close + 1;
		}
		else
		{
			size_t end = i;
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
	return out;
}

// The line from a token on, as written, without a comment at its end.
std::string RestFrom( const std::string& line, const Token& token )
{
	std::string text = line.substr( std::min( token.start, line.size() ) );
	const size_t hash = text.find( '#' );
	return Strip( hash != std::string::npos ? text.substr( 0, hash ) : text );
}

bool Number( const Token& token, double& value )
{
	if ( token.quoted || IsValidFloat( token.text ) == false )
	{
		return false;
	}
	value = ToFloat( token.text );
	return true;
}

// A value to set: a number, a truth, a colour, a vector, else words.
Value ValueOf( const Token& token )
{
	const std::string& t = token.text;
	if ( token.quoted )
	{
		return t;
	}
	if ( t == "true" || t == "false" )
	{
		return t == "true";
	}
	if ( IsValidInt( t ) )
	{
		return ToInt( t );
	}
	if ( IsValidFloat( t ) )
	{
		return ToFloat( t );
	}
	auto numbers = [&]( const std::string& inside ) {
		std::vector<std::string> parts = Split( inside, "," );
		std::vector<double> out;
		for ( const std::string& raw : parts )
		{
			const std::string part = Strip( raw );
			if ( IsValidFloat( part ) == false )
			{
				return std::vector<double>{};
			}
			out.push_back( ToFloat( part ) );
		}
		return out;
	};
	if ( ( BeginsWith( t, "rgb(" ) || BeginsWith( t, "rgba(" ) ) && EndsWith( t, ")" ) )
	{
		const size_t open = t.find( '(' );
		std::vector<double> v = numbers( t.substr( open + 1, t.size() - open - 2 ) );
		if ( v.size() == 3 || v.size() == 4 )
		{
			return Color4{ float( v[0] ), float( v[1] ), float( v[2] ), v.size() == 4 ? float( v[3] ) : 1.0f };
		}
	}
	if ( BeginsWith( t, "(" ) && EndsWith( t, ")" ) )
	{
		std::vector<double> v = numbers( t.substr( 1, t.size() - 2 ) );
		if ( v.size() == 3 )
		{
			return Float3{ float( v[0] ), float( v[1] ), float( v[2] ) };
		}
	}
	return t;
}

// "at <where> [from PATH] [offset X Y Z]", from the word after "at" up to `end`: the place into
// `into`, and where it stopped in `next`. False, with why, when it cannot be read.
bool ParseAt( const std::vector<Token>& w, size_t i, size_t end, Dict& into, size_t& next, std::string& why )
{
	if ( i >= end )
	{
		why = "at point, end, beam, follow, or a node's path";
		return false;
	}
	const std::string where = w[i].text;
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
		into["offset"] = Float3{ float( x ), float( y ), float( z ) };
		i += 4;
	}
	next = i;
	return true;
}


// --- Where a line of a motion block may stand -------------------------------------------------------
// One table: every refusal of a line for where it stands is made from it, and the reference
// (docs/sheets.md, "Where a line may stand") is the same table.
enum Place : uint8_t
{
	kInStep = 1,	 // a step's own lines (under an if in it too)
	kInEvery = 2,	 // after an every, in its step: what happens again and again
	kInTimeout = 4,	 // a timeout's lines: once, and the motion ends
	kInFind = 8,	 // what happens to each thing a find found
	kInCast = 16,	 // what a cast does while it holds
	kInItem = 32,	 // an item's motion (on item): it all happens in one tick
	kItemOnly = 64,	 // (only an item's motion has it)
	kInGrab = 128,	 // what a grab does to what it holds (M290): a find's lines, and forces
};

struct LineRule
{
	const char* verb;
	uint8_t places;
};

const LineRule kLineRules[] = {
	{ "change", kInStep | kInEvery | kInTimeout | kInFind | kInGrab | kInItem },
	{ "emit", kInStep | kInEvery | kInTimeout | kInFind | kInGrab | kInCast | kInItem },
	{ "impulse", kInStep | kInEvery | kInTimeout | kInFind | kInGrab | kInCast | kInItem },
	{ "throw", kInFind | kInGrab | kInCast },
	{ "launch", kInStep | kInEvery | kInTimeout },
	{ "break", kInStep | kInEvery | kInTimeout | kInItem },
	{ "find", kInStep | kInEvery | kInTimeout | kInItem },
	{ "if", kInStep | kInEvery | kInTimeout | kInFind | kInGrab | kInItem },
	{ "else", kInStep | kInEvery | kInTimeout | kInFind | kInGrab | kInItem },
	{ "move for", kInStep | kInFind | kInGrab | kInCast },
	{ "apply", kInStep | kInEvery | kInTimeout | kInFind | kInGrab | kInCast },
	{ "move", kInStep | kInCast },
	{ "force", kInStep | kInGrab | kInCast },
	{ "steer", kInStep | kInGrab | kInCast },
	{ "grab", kInStep },
	{ "link", kInStep | kInCast },
	{ "follow", kInStep },
	{ "end", kInStep },
	{ "remove", kInStep | kInItem | kItemOnly },
	{ "cast", kInStep },
	{ "every", kInStep },
};

const LineRule* LineRuleOf( const std::string& verb )
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

std::string PlaceName( uint8_t place )
{
	return place == kInEvery	 ? "after an every"
		   : place == kInTimeout ? "in a timeout"
		   : place == kInFind	 ? "in a find"
		   : place == kInCast	 ? "in a cast"
		   : place == kInGrab	 ? "in a grab"
		   : place == kInItem	 ? "in an item's motion (it happens in one tick)"
								 : "in a step";
}

// Why `verb` may not stand at `place` (in an item's motion too, with `item`); empty: it may.
std::string Misplaced( const std::string& verb, uint8_t place, bool item )
{
	const LineRule* rule = LineRuleOf( verb );
	if ( rule == nullptr )
	{
		return std::string();
	}
	const bool here = ( rule->places & place ) != 0;
	const bool forItem = item == false || ( rule->places & kInItem ) != 0;
	const bool itemOnly = ( rule->places & kItemOnly ) == 0 || item;
	if ( here && forItem && itemOnly )
	{
		return std::string();
	}
	if ( itemOnly == false )
	{
		return "\"" + verb + "\" is an item's motion's (on item)";
	}
	const uint8_t where = here ? uint8_t( kInItem ) : place;
	std::string list;
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
		list += std::string( k == 0 ? "" : k + 1 == verbs.size() ? " or " : ", " ) + verbs[k];
	}
	return "\"" + verb + "\" cannot stand " + PlaceName( where ) + ": there a line is " + list;
}

// The movement parameters' names (SetMoveParameterNames): where a move's value ends and the next
// parameter starts.
Strings& MoveParameters()
{
	static Strings names;
	return names;
}

bool IsMoveParameter( const std::string& name )
{
	for ( const std::string& one : MoveParameters() )
	{
		if ( name == one )
		{
			return true;
		}
	}
	return false;
}

// An amount a line says: the words from w[k] up to the next that `stops` (unquoted), or the line's
// end. One word that is a number is a number; anything else is an expression, read when the line
// acts. False when there is no word.
struct Amount
{
	bool numeric = false;
	double number = 0.0;
	std::string text;
};

template <typename Stops>
bool TakeAmount( const std::vector<Token>& w, size_t& k, Stops stops, Amount& out )
{
	size_t end = k;
	while ( end < w.size() && ( w[end].quoted || stops( w[end].text ) == false ) )
	{
		++end;
	}
	if ( end == k )
	{
		return false;
	}
	out = Amount{};
	out.numeric = end == k + 1 && Number( w[k], out.number );
	for ( size_t i = k; i < end; ++i )
	{
		out.text += ( i > k ? " " : "" ) + w[i].text;
	}
	k = end;
	return true;
}

// "on target": what the block a line is in is about (a find's thing found, a cast's hold), until the
// block resolves it (ResolveTarget).
constexpr int kBlockTarget = 4;

// A part's "on target" made what its block is about: what a cast holds (1) or a find found (3).
// False outside a find or a cast.
bool ResolveTarget( Dict& props, bool inFind, bool inCast )
{
	if ( props.Get( "target", 0 ).AsInt() != kBlockTarget )
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

// A motion part (impulse, force, move, launch): its class and properties, read by the bake
// (sheet_bake.cpp). False, with why, when the line is none of them or cannot be read.
bool ParsePart( const std::vector<Token>& w, Dict& part, std::string& why )
{
	const std::string verb = w[0].text;
	double value = 0.0;
	Dict props;
	// The amounts that are expressions, by their place in the line the part bakes to (its first): the
	// bake writes each there in place of its number (BakeSheetPart). Keys are the fields' numbers.
	Dict amounts;
	// An amount into the property `property` (its number, or 0 and the expression at `field`).
	auto amountInto = [&]( const Amount& amount, const char* property, int field ) {
		props[property] = amount.numeric ? amount.number : 0.0;
		if ( amount.numeric == false )
		{
			amounts[Num64( field )] = amount.text;
		}
	};
	size_t i = 1;
	auto frameOf = [&]( const std::string& word ) {
		return word == "look" ? 0 : word == "move" ? 1 : word == "facing" ? 2 : word == "up" ? 3 : word == "world" ? 4 : word == "to" ? 5 : word == "away" ? 6 : -1;
	};
	// The words a part shares: on, along, direction, toward.
	auto common = [&]( const std::string& word, size_t& k ) {
		if ( word == "toward" && k + 1 < w.size() )
		{
			// toward owner | hit | look D (M290): from its target to the point.
			const std::string to = w[k + 1].text;
			if ( to == "look" && k + 2 < w.size() )
			{
				props["toward"] = "look " + w[k + 2].text;
				k += 3;
				return true;
			}
			if ( to != "owner" && to != "hit" )
			{
				why = "toward owner, hit or look D";
			}
			props["toward"] = to;
			k += 2;
			return true;
		}
		if ( word == "on" && k + 1 < w.size() )
		{
			// self; target (what the find found, or what the cast holds: kBlockTarget until the block
			// says which); found and hit, the older words for the same; else a field naming an entity.
			const std::string who = w[k + 1].text;
			props["target"] = who == "self" ? 0 : who == "hit" ? 1 : who == "found" ? 3 : who == "target" ? kBlockTarget : 2;
			if ( who != "self" && who != "hit" && who != "found" && who != "target" )
			{
				props["target_field"] = who;
			}
			k += 2;
			return true;
		}
		if ( word == "along" && k + 1 < w.size() && w[k + 1].text == "down" )
		{
			// along down (M296): along world (0, -1, 0)
			props["frame"] = 4;
			props["direction"] = Float3{ 0.0f, -1.0f, 0.0f };
			k += 2;
			return true;
		}
		if ( word == "along" && k + 2 < w.size() && w[k + 1].text == "world" && ValueOf( w[k + 2] ).type() == Value::Type::Vec3 )
		{
			// along world (x, y, z)
			props["frame"] = 4;
			props["direction"] = ValueOf( w[k + 2] );
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
			const std::string inside = w[k + 1].text;
			std::vector<std::string> two = BeginsWith( inside, "(" ) && EndsWith( inside, ")" ) ? Split( inside.substr( 1, inside.size() - 2 ), "," )
																								 : std::vector<std::string>();
			if ( two.size() != 2 || IsValidFloat( Strip( two[0] ) ) == false || IsValidFloat( Strip( two[1] ) ) == false )
			{
				why = "turn (yaw, pitch): degrees, right and up";
				k += 2;
				return true;
			}
			props["turn"] = Float2{ float( ToFloat( Strip( two[0] ) ) ), float( ToFloat( Strip( two[1] ) ) ) };
			k += 2;
			return true;
		}
		if ( word == "direction" && k + 1 < w.size() && ValueOf( w[k + 1] ).type() == Value::Type::Vec3 )
		{
			props["direction"] = ValueOf( w[k + 1] );
			k += 2;
			return true;
		}
		return false;
	};
	if ( verb == "steer" )
	{
		// steer [on WHO] (toward POINT | along DIRECTION) speed S strength A [ramp S] [react] (M296): a force
		// that brings the velocity to S, A how hard (until M296: force A kind velocity top S).
		part["class"] = "CbForce";
		props["kind"] = 2;
		auto steerStops = []( const std::string& word ) {
			return word == "on" || word == "along" || word == "direction" || word == "speed" || word == "strength" || word == "ramp" || word == "react" ||
				   word == "turn" || word == "toward";
		};
		bool hasSpeed = false, hasStrength = false;
		for ( i = 1; i < w.size() && why.empty(); )
		{
			const std::string word = w[i].text;
			if ( common( word, i ) )
			{
				continue;
			}
			if ( ( word == "speed" || word == "strength" ) && i + 1 < w.size() )
			{
				// (Amounts: the line's fifth and sixth fields, as a force's strength and top.)
				i += 1;
				Amount amount;
				TakeAmount( w, i, steerStops, amount );
				amountInto( amount, word == "speed" ? "speed" : "strength", word == "speed" ? 5 : 4 );
				( word == "speed" ? hasSpeed : hasStrength ) = true;
			}
			else if ( word == "ramp" && i + 1 < w.size() && Number( w[i + 1], value ) )
			{
				props["ramp_in"] = value;
				i += 2;
			}
			else if ( word == "react" )
			{
				props["react"] = true;
				i += 1;
			}
			else
			{
				why = "\"" + word + "\" is not something steer takes: speed S, strength A, toward POINT, along DIRECTION, on WHO, ramp S, react";
			}
		}
		if ( why.empty() && ( hasSpeed == false || hasStrength == false ) )
		{
			why = "steer ... speed S strength A: the speed it brings the velocity to, and how hard";
		}
	}
	else if ( verb == "impulse" || verb == "force" || verb == "throw" )
	{
		// How much: a number, or an expression up to the first word that is an option (read when it
		// acts: "9 * curve(Falloff, reach)", "2 * dash.power").
		auto forceStops = []( const std::string& word ) {
			return word == "on" || word == "along" || word == "direction" || word == "replace" || word == "kind" || word == "top" ||
				   word == "ramp" || word == "react" || word == "turn" || word == "toward" || word == "marks";
		};
		size_t options = 1;
		while ( options < w.size() && ( w[options].quoted || forceStops( w[options].text ) == false ) )
		{
			++options;
		}
		if ( options < 2 )
		{
			why = verb + " <how much> ...";
			return false;
		}
		// (A part's "class" says what it is to the bake: the names of the motion nodes it was baked
		// through until M268.)
		part["class"] = verb != "force" ? "CbImpulse" : "CbForce";
		{
			// (The words before the first option word, found above.)
			Amount main;
			main.numeric = options == 2 && Number( w[1], main.number );
			for ( size_t j = 1; j < options; ++j )
			{
				main.text += ( j > 1 ? " " : "" ) + w[j].text;
			}
			// (An impulse's speed is its line's third field, a force's strength its fifth.)
			amountInto( main, verb != "force" ? "speed" : "strength", verb != "force" ? 2 : 4 );
		}
		for ( i = options; i < w.size() && why.empty(); )
		{
			const std::string word = w[i].text;
			if ( common( word, i ) )
			{
				continue;
			}
			if ( verb == "throw" && word == "marks" && i + 3 < w.size() && w[i + 2].text == "for" && Number( w[i + 3], value ) && value > 0.0 )
			{
				// throw ... marks EVENT for S (M290): what it throws strikes someone within S: EVENT.
				props["marks"] = w[i + 1].text;
				props["marks_seconds"] = value;
				i += 4;
			}
			else if ( verb != "force" && word == "replace" && i + 1 < w.size() )
			{
				const std::string r = w[i + 1].text;
				const int replace = r == "nothing" ? 0 : r == "vertical" ? 1 : r == "horizontal" ? 2 : r == "all" ? 3 : -1;
				if ( replace < 0 )
				{
					why = "replace nothing, vertical, horizontal or all";
				}
				props["replace"] = replace;
				i += 2;
			}
			else if ( verb != "force" && word == "kind" && i + 1 < w.size() )
			{
				// kind velocity (the default: m/s, whatever it weighs) or momentum (N s: heavy moves less)
				const std::string k = w[i + 1].text;
				if ( k != "velocity" && k != "momentum" )
				{
					why = "kind velocity or momentum";
				}
				props["momentum"] = k == "momentum";
				i += 2;
			}
			else if ( verb == "force" && word == "kind" && i + 1 < w.size() )
			{
				const std::string k = w[i + 1].text;
				const int kind = k == "acceleration" ? 0 : k == "force" ? 1 : -1;
				if ( k == "velocity" )
				{
					why = "a force toward a speed is steer (M296): steer ... speed S strength A";
				}
				else if ( kind < 0 )
				{
					why = "kind acceleration or force (toward a speed: steer)";
				}
				props["kind"] = kind;
				i += 2;
			}
			else if ( verb == "force" && word == "top" && i + 1 < w.size() )
			{
				// top SPEED: an amount (the line's sixth field)
				i += 1;
				Amount top;
				TakeAmount( w, i, forceStops, top );
				amountInto( top, "speed", 5 );
			}
			else if ( verb != "force" && word == "top" && i + 1 < w.size() && Number( w[i + 1], value ) && value > 0.0 )
			{
				props["cap"] = value; // (M292: an impulse's change of velocity at most, m/s)
				i += 2;
			}
			else if ( verb == "force" && word == "ramp" && i + 1 < w.size() && Number( w[i + 1], value ) )
			{
				props["ramp_in"] = value; // (a time of the motion's: a number)
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
		for ( i = 1; i < w.size() && why.empty(); )
		{
			const std::string word = w[i].text;
			if ( word == "on" && common( word, i ) )
			{
				continue;
			}
			if ( ( word == "length" || word == "reel" ) && i + 1 < w.size() )
			{
				// length L, reel S: amounts (the line's third and fourth fields)
				i += 1;
				Amount amount;
				TakeAmount( w, i, []( const std::string& next ) { return next == "length" || next == "reel" || next == "alone" || next == "on"; }, amount );
				amountInto( amount, word == "length" ? "length" : "reel", word == "length" ? 2 : 3 );
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
		Dict parameters;
		i = 1;
		if ( i + 1 < w.size() && w[i].text == "for" )
		{
			// (Its seconds: a time of the motion's, a number.)
			if ( Number( w[i + 1], value ) == false )
			{
				why = "move for S: its seconds are a number";
				return false;
			}
			props["duration"] = value;
			i += 2;
		}
		// PARAMETER VALUE ...: a value is the words up to the next parameter's name (a number, or an
		// expression read when its step starts). Without the names (the parser used alone), one word each.
		while ( i < w.size() && why.empty() )
		{
			const std::string name = w[i].text;
			i += 1;
			Amount amount;
			const bool named = MoveParameters().empty() == false;
			size_t stop = i;
			bool took = false;
			if ( named )
			{
				took = TakeAmount( w, stop, []( const std::string& next ) { return IsMoveParameter( next ); }, amount );
			}
			else if ( i < w.size() )
			{
				amount.numeric = Number( w[i], amount.number );
				amount.text = w[i].text;
				stop = i + 1;
				took = true;
			}
			if ( took == false )
			{
				why = "move [for S] PARAMETER VALUE ...: " + name + " wants a value";
				break;
			}
			if ( named && IsMoveParameter( name ) == false )
			{
				why = "move: \"" + name + "\" is not a movement parameter";
				break;
			}
			parameters[name] = amount.numeric ? Value( amount.number ) : Value( amount.text );
			i = stop;
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
		while ( i < w.size() && why.empty() )
		{
			const std::string word = w[i].text;
			if ( common( word, i ) )
			{
				continue;
			}
			if ( ( word == "speed" || word == "lift" || word == "ahead" || word == "seconds" ) && i + 1 < w.size() )
			{
				// Amounts, read when it is thrown: the line's third, fifth, sixth and seventh fields.
				i += 1;
				Amount amount;
				TakeAmount( w, i,
							[]( const std::string& next ) {
								return next == "speed" || next == "lift" || next == "ahead" || next == "seconds" || next == "from" || next == "along" ||
									   next == "on" || next == "turn" || next == "direction";
							},
							amount );
				const int field = word == "speed" ? 2 : word == "lift" ? 4 : word == "ahead" ? 5 : 6;
				amountInto( amount, word == "speed" ? "speed" : word == "lift" ? "lift" : word == "ahead" ? "ahead" : "seconds", field );
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
		why = "\"" + verb + "\" is not one of: change, cooldown, emit, impulse, force, steer, link, move, launch";
	}
	if ( why.empty() == false )
	{
		return false;
	}
	if ( amounts.empty() == false )
	{
		props["amounts"] = amounts;
	}
	part["properties"] = props;
	return true;
}

// --- Lowering: the tree's blocks into rules (M293) -------------------------------------------------
//
// Each block of the tree (sheet_tree.h) becomes its rules here, read from the tree: what a line
// governs is what is under it. The words of a line are read as the engine's (SplitLine, ParsePart)
// once its expressions are in the engine's spelling (EngineExpression).

struct Lowering
{
	const Tree& tree;
	Strings& errors;
	std::vector<Rule>& out;
	void Fail( int line, const std::string& why ) const
	{
		errors.push_back( tree.Mistake( line, why ) );
	}
};

// A line's code with its expressions in the engine's spelling: the verb as it is, the rest
// translated (EngineExpression).
std::string Resolved( const Tree& tree, const std::string& code, bool inMotion )
{
	const std::vector<Word> w = WordsOf( code );
	if ( w.size() < 2 )
	{
		return code;
	}
	return w[0].text + " " + EngineExpression( tree, From( code, w, 1 ), inMotion );
}

// An if that is open around a line: its condition, and whether the line is in its else.
struct Frame
{
	std::string condition;
	bool otherwise = false;
};

std::string Conjunction( const std::vector<Frame>& frames )
{
	std::string all;
	for ( const Frame& frame : frames )
	{
		const std::string one = frame.otherwise ? "not ( " + frame.condition + " )" : "( " + frame.condition + " )";
		all += all.empty() ? one : " and " + one;
	}
	return all;
}

// A motion block (motion NAME TRIGGER [if EXPRESSION], or use [if EXPRESSION]): its settings and its
// steps, for the bake (sheet_bake.h) and the simulation.
class MotionLowering
{
public:
	MotionLowering( const Lowering& lowering, const Node& block )
		: m_l( lowering )
		, m_block( block )
	{
	}

	void Lower()
	{
		Dict first;
		first["statements"] = List();
		m_steps.push_back( first );
		if ( Head() == false )
		{
			return;
		}
		std::vector<Frame> frames;
		Lines( m_block.under, frames );
		Finish();
	}

private:
	const Lowering& m_l;
	const Node& m_block;
	Dict m_settings;
	List m_steps;
	bool m_cast = false; // the motion has one
	bool m_bad = false;

	void Fail( int line, const std::string& why )
	{
		m_l.Fail( line, why );
		m_bad = true;
	}

	bool Head()
	{
		const int number = m_block.line.number;
		const std::string line = Resolved( m_l.tree, m_block.line.code, true );
		const std::vector<Token> w = SplitLine( line );
		if ( w[0].text == "use" )
		{
			// use [if EXPRESSION]: motion Use on use [if EXPRESSION], word for word
			m_settings["name"] = "Use";
			m_settings["trigger"] = "use";
			if ( w.size() >= 2 )
			{
				if ( w[1].text != "if" || w.size() < 3 )
				{
					Fail( number, "use [if EXPRESSION]" );
					return false;
				}
				m_settings["conditions"] = Strings{ RestFrom( line, w[2] ) };
			}
			return true;
		}
		// motion NAME on use | on press ACTION | on release ACTION | on tap ACTION [S] | on hold ACTION [S]
		// | on EVENT | while EXPRESSION | when EXPRESSION, then [if EXPRESSION]
		const char* usage = "motion NAME on use, on press ACTION, on release ACTION, on tap ACTION [S], on hold ACTION [S], on EVENT, while EXPRESSION or "
							"when EXPRESSION, then [if EXPRESSION]";
		if ( w.size() < 3 || Contains( w[1].text, "/" ) || Contains( w[1].text, "." ) || w[1].quoted )
		{
			Fail( number, usage );
			return false;
		}
		m_settings["name"] = w[1].text;
		const std::string how = w[2].text;
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
			m_settings["trigger"] = "use";
		}
		else if ( how == "on" && w.size() >= 5 && w[3].text == "press" && cut == 5 )
		{
			m_settings["trigger"] = "press";
			m_settings["action"] = w[4].text;
		}
		else if ( how == "on" && w.size() >= 5 && ( w[3].text == "release" || w[3].text == "tap" || w[3].text == "hold" ) && ( cut == 5 || cut == 6 ) )
		{
			// on release ACTION, on tap ACTION [S], on hold ACTION [S] (M288)
			m_settings["trigger"] = w[3].text;
			m_settings["action"] = w[4].text;
			double seconds = 0.0;
			if ( cut == 6 )
			{
				if ( w[3].text == "release" || Number( w[5], seconds ) == false || seconds <= 0.0 )
				{
					Fail( number, "on tap ACTION [S], on hold ACTION [S]: a tap is released before S, a hold still down at S (0.25 s)" );
					return false;
				}
				m_settings["seconds"] = seconds;
			}
		}
		else if ( how == "on" && cut == 4 )
		{
			m_settings["trigger"] = "event";
			m_settings["event"] = w[3].text;
		}
		else if ( ( how == "while" || how == "when" ) && cut > 3 )
		{
			m_settings["trigger"] = how;
			m_settings["expression"] = Strip( line.substr( w[3].start, ( cut < w.size() ? w[cut].start : line.size() ) - w[3].start ) );
		}
		else
		{
			Fail( number, usage );
			return false;
		}
		if ( cut < w.size() )
		{
			if ( cut + 1 >= w.size() || how == "while" )
			{
				Fail( number, how == "while" ? std::string( "a while's expression is its condition: no if" ) : std::string( "if what? if EXPRESSION" ) );
				return false;
			}
			m_settings["conditions"] = Strings{ RestFrom( line, w[cut + 1] ) };
		}
		return true;
	}

	// Lines under the motion (`frames` empty) or under an if in it.
	void Lines( const std::vector<Node>& lines, std::vector<Frame>& frames )
	{
		const Node* lastIf = nullptr; // the if just before, while an else may still follow it
		for ( const Node& node : lines )
		{
			const Node* previous = lastIf;
			lastIf = nullptr;
			const int number = node.line.number;
			const std::string verb = node.Verb();
			const bool top = frames.empty();
			if ( verb == "if" )
			{
				const std::string line = Resolved( m_l.tree, node.line.code, true );
				const std::vector<Token> w = SplitLine( line );
				if ( w.size() < 2 )
				{
					Fail( number, "if EXPRESSION" );
					continue;
				}
				frames.push_back( { RestFrom( line, w[1] ), false } );
				Lines( node.under, frames );
				frames.pop_back();
				lastIf = &node;
				continue;
			}
			if ( verb == "else" )
			{
				if ( WordsOf( node.line.code ).size() != 1 || previous == nullptr )
				{
					Fail( number, "an else comes right after an if's lines, as far in as the if" );
					continue;
				}
				const std::string line = Resolved( m_l.tree, previous->line.code, true );
				frames.push_back( { RestFrom( line, SplitLine( line )[1] ), true } );
				Lines( node.under, frames );
				frames.pop_back();
				continue;
			}
			Statement( node, frames, top );
		}
	}

	// A line of a step (not an if): a setting, a wait, an every, or a statement.
	void Statement( const Node& node, const std::vector<Frame>& frames, bool top )
	{
		const int number = node.line.number;
		const std::vector<Word> words = WordsOf( node.line.code );
		const std::string verb = words[0].text;
		const bool opens = verb == "find" || verb == "grab" || verb == "cast";
		if ( node.under.empty() == false && opens == false )
		{
			Fail( node.under[0].line.number, "nothing goes under \"" + verb + "\": only an if, an else, a find, a grab and a cast have lines under them" );
		}
		Dict step = m_steps[m_steps.size() - 1].AsDict();
		List statements = step["statements"].AsList();
		const bool stepLine = verb == "wait" || verb == "timeout";
		const bool setting = verb == "cooldown" || verb == "uses" || verb == "lasts" || verb == "until" || verb == "for" || ( verb == "on" && words.size() == 2 ) ||
							 verb == "tags" || verb == "blocked" || verb == "cancels";
		if ( ( stepLine || setting || verb == "every" ) && top == false )
		{
			Fail( number, "\"" + verb + "\" is the motion's: as far in as its first line, not under an if" );
			return;
		}
		if ( stepLine )
		{
			Wait( node, step, statements );
			return;
		}
		if ( setting )
		{
			Setting( node );
			return;
		}
		const std::string line = Resolved( m_l.tree, node.line.code, true );
		const std::vector<Token> w = SplitLine( line );
		double value = 0.0;
		if ( verb == "every" )
		{
			// every S: what follows in this step, again and again while it is the step
			bool again = false;
			for ( size_t k = 0; k < statements.size(); ++k )
			{
				again = again || statements[k].AsDict().Get( "kind", "" ).AsString() == "every";
			}
			if ( step.Get( "timeout", false ).AsBool() || again )
			{
				Fail( number, Misplaced( "every", again ? kInEvery : kInTimeout, false ) );
				return;
			}
			if ( w.size() != 2 || Number( w[1], value ) == false || value <= 0.0 )
			{
				Fail( number, "every S: S seconds (more than 0); what follows in its step happens every S seconds" );
				return;
			}
			Dict every;
			every["kind"] = "every";
			every["verb"] = "every";
			every["seconds"] = value;
			every["line"] = number;
			every["guard"] = std::string();
			statements.push_back( every );
			return;
		}
		// --- statements, where they may stand (the table above): a step's own, after its every, a
		// timeout's (an item's motion is checked as a whole when it ends)
		Dict statement;
		{
			bool afterEvery = false;
			for ( size_t k = 0; k < statements.size(); ++k )
			{
				afterEvery = afterEvery || statements[k].AsDict().Get( "kind", "" ).AsString() == "every";
			}
			const uint8_t place = step.Get( "timeout", false ).AsBool() ? kInTimeout : afterEvery ? kInEvery : kInStep;
			const std::string placed = verb == "move" && w.size() > 1 && w[1].text == "for" ? std::string( "move for" ) : verb;
			const bool ownerItem = m_settings.Get( "owner", "player" ).AsString() == "item";
			// (An item's own line, before "on item" was said, is checked when the motion ends.)
			const std::string misplaced = placed == "remove" ? std::string() : Misplaced( placed, place, ownerItem );
			if ( misplaced.empty() == false )
			{
				Fail( number, misplaced );
				return;
			}
			statement["verb"] = placed;
		}
		auto add = [&]() {
			statement["guard"] = Conjunction( frames );
			statement["line"] = number;
			statements.push_back( statement );
		};
		if ( verb == "find" || verb == "grab" )
		{
			if ( Find( w, number, statement ) )
			{
				add();
				Inside( node.under, statement, false );
			}
			return;
		}
		if ( verb == "cast" )
		{
			if ( m_steps.size() > 1 || frames.empty() == false || m_cast )
			{
				Fail( number, m_cast ? "a motion casts once" : "a cast is thrown when the motion starts: in its first step, not under an if" );
				return;
			}
			if ( Cast( w, number, statement ) )
			{
				add();
				m_cast = true;
				Inside( node.under, statement, true );
			}
			return;
		}
		if ( verb == "follow" )
		{
			// follow PATH over S: carried along a Path3D drawn in the scene
			if ( w.size() != 4 || w[1].quoted || w[2].text != "over" || Number( w[3], value ) == false || value <= 0.0 )
			{
				Fail( number, "follow PATH over S: a Path3D beside the sheet, and the seconds it takes" );
				return;
			}
			statement["kind"] = "follow";
			statement["path"] = w[1].text;
			statement["seconds"] = value;
			add();
			return;
		}
		if ( verb == "break" )
		{
			// break radius R [speed S] [seconds S] [at look D | hit]: walls near the owner. A number, a
			// name, or an expression in brackets each.
			bool ok = w.size() >= 3;
			auto breakStops = []( const std::string& next ) { return next == "radius" || next == "speed" || next == "seconds" || next == "at"; };
			for ( size_t k = 1; ok && k < w.size(); )
			{
				const std::string word = w[k].text;
				k += 1;
				if ( word == "at" )
				{
					// at look D | hit (M290): around a point, not the owner.
					ok = k < w.size() && ( w[k].text == "hit" || ( w[k].text == "look" && k + 1 < w.size() ) );
					statement["point"] = ok ? ( w[k].text == "hit" ? std::string( "hit" ) : "look " + w[k + 1].text ) : std::string();
					k += ok && w[k].text == "look" ? 2 : 1;
					continue;
				}
				Amount amount;
				ok = breakStops( word ) && TakeAmount( w, k, breakStops, amount );
				statement[word] = amount.text; // (a number, or an expression: the bake writes it as it is)
			}
			if ( ok == false || statement.Has( "radius" ) == false )
			{
				Fail( number, "break radius R [speed S] [seconds S] [at look D | hit]: the walls within R of it, their pieces flung at S, staying S seconds (0: for good)" );
				return;
			}
			statement["kind"] = "break";
			add();
			return;
		}
		if ( verb == "remove" )
		{
			if ( w.size() != 2 || w[1].text != "self" )
			{
				Fail( number, "remove self (an item's motion: the item leaves the world)" );
				return;
			}
			statement["kind"] = "remove";
			add();
			return;
		}
		if ( verb == "change" )
		{
			if ( w.size() < 4 )
			{
				Fail( number, "change FIELD = VALUE, += or -=" );
				return;
			}
			// (M296) A while motion's += and -= go on every tick, a rate: the line says "per second".
			// Nothing else does (until M296 a while's += was per second without saying so).
			std::string text = RestFrom( line, w[1] );
			const bool whileMotion = m_settings.Get( "trigger", "" ).AsString() == "while";
			const bool rate = w[2].text == "+=" || w[2].text == "-=";
			const bool says = w.size() >= 6 && w[w.size() - 2].text == "per" && w.back().text == "second";
			if ( whileMotion && rate && says == false )
			{
				Fail( number, "a while motion's += and -= are per second: say so (change " + w[1].text + " " + w[2].text + " " + RestFrom( line, w[3] ) + " per second)" );
				return;
			}
			if ( says && ( whileMotion == false || rate == false ) )
			{
				Fail( number, "per second is for += and -= in a while motion (anywhere else a change is once)" );
				return;
			}
			if ( says )
			{
				text = Strip( text.substr( 0, text.rfind( " per " ) ) );
			}
			statement["kind"] = "change";
			statement["text"] = text;
		}
		else if ( verb == "emit" )
		{
			// emit EVENT [value EXPRESSION] (M287: the event's value, read when it happens)
			if ( w.size() != 2 && ( w.size() < 4 || w[2].text != "value" ) )
			{
				Fail( number, "emit EVENT [value VALUE]: a number or an expression" );
				return;
			}
			statement["kind"] = "emit";
			statement["text"] = w[1].text;
			if ( w.size() >= 4 )
			{
				statement["value"] = RestFrom( line, w[3] );
			}
		}
		else if ( verb == "end" )
		{
			if ( w.size() != 1 )
			{
				Fail( number, "end (alone on its line)" );
				return;
			}
			statement["kind"] = "end";
		}
		else if ( verb == "apply" )
		{
			// apply STATUS for S (M288): on the owner
			if ( w.size() < 4 || w[2].text != "for" )
			{
				Fail( number, "apply STATUS for S" );
				return;
			}
			statement["kind"] = "apply";
			statement["text"] = w[1].text;
			statement["value"] = RestFrom( line, w[3] );
		}
		else
		{
			Dict part;
			std::string why;
			if ( ParsePart( w, part, why ) == false )
			{
				Fail( number, BeginsWith( why, "\"" + verb + "\" is not one of" )
								  ? "\"" + verb +
										"\" is not a line of a motion: if, else, wait, timeout, every, cooldown, uses, lasts, until, tags, blocked by, cancels, for, on item, change, "
										"emit, end, apply, find, grab, cast, impulse, throw, force, steer, link, move, launch, follow, break, remove"
								  : why );
				return;
			}
			Dict props = part["properties"].AsDict();
			if ( ResolveTarget( props, false, false ) == false )
			{
				Fail( number, "on target is what a find found or what a cast holds: inside one" );
				return;
			}
			statement["kind"] = "part";
			statement["part"] = part;
		}
		add();
	}

	// wait S | wait for EXPRESSION [within S | or S] | timeout: a new step.
	void Wait( const Node& node, Dict& step, const List& statements )
	{
		const int number = node.line.number;
		const std::string& code = node.line.code;
		const std::vector<Word> w = WordsOf( code );
		Dict next;
		next["statements"] = List();
		next["line"] = number;
		double value = 0.0;
		if ( w[0].text == "timeout" )
		{
			if ( w.size() != 1 || step.Get( "within", 0.0 ).AsFloat() <= 0.0 || step.Get( "timeout", false ).AsBool() || step.Get( "or", false ).AsBool() )
			{
				Fail( number, "a timeout comes right after a wait for ... within S (a wait for ... or S goes on: it has none)" );
				return;
			}
			next["timeout"] = true;
		}
		else if ( w.size() == 2 && w[1].text != "for" )
		{
			const Token seconds{ w[1].text, 0, false };
			if ( Number( seconds, value ) == false || value < 0.0 )
			{
				Fail( number, "wait S: S seconds" );
				return;
			}
			next["after"] = value;
		}
		else
		{
			if ( w.size() < 3 || w[1].text != "for" )
			{
				Fail( number, "wait S, or wait for EXPRESSION [within S | or S]" );
				return;
			}
			size_t last = w.size();
			if ( w.size() >= 5 && ( w[w.size() - 2].text == "within" || w[w.size() - 2].text == "or" ) && IsValidFloat( w[w.size() - 1].text ) )
			{
				value = ToFloat( w[w.size() - 1].text );
				if ( value <= 0.0 )
				{
					Fail( number, "wait for EXPRESSION within S, or S: S more than 0" );
					return;
				}
				next["within"] = value;
				if ( w[w.size() - 2].text == "or" )
				{
					next["or"] = true; // (it goes on after S without it, M287)
				}
				last = w.size() - 2;
			}
			next["wait"] = Strip( EngineExpression( m_l.tree, code.substr( w[2].start, w[last - 1].end - w[2].start ), true ) );
		}
		step["statements"] = statements;
		m_steps[m_steps.size() - 1] = step;
		m_steps.push_back( next );
	}

	// The motion's own settings: as far in as its first line, before its first wait.
	void Setting( const Node& node )
	{
		const int number = node.line.number;
		const std::vector<Word> words = WordsOf( node.line.code );
		const std::string verb = words[0].text;
		if ( m_steps.size() > 1 )
		{
			Fail( number, "\"" + verb + "\" is the motion's: before its first wait" );
			return;
		}
		if ( verb == "for" )
		{
			if ( words.size() != 2 || ( words[1].text != "holder" && words[1].text != "carrier" ) )
			{
				Fail( number, "for holder (the default: whoever has the item out) or for carrier (in the hand or put away)" );
				return;
			}
			m_settings["item"] = words[1].text == "holder" ? "hand" : "carried";
			return;
		}
		if ( verb == "on" )
		{
			if ( words[1].text != "item" )
			{
				Fail( number, "on item: the item runs the motion, where it lies" );
				return;
			}
			m_settings["owner"] = "item";
			return;
		}
		const std::string line = Resolved( m_l.tree, node.line.code, true );
		const std::vector<Token> w = SplitLine( line );
		double value = 0.0;
		if ( verb == "until" && w.size() >= 2 )
		{
			Strings until = m_settings.Get( "until", Strings() ).AsStrings();
			until.push_back( RestFrom( line, w[1] ) );
			m_settings["until"] = until;
		}
		else if ( ( verb == "cooldown" || verb == "lasts" ) && w.size() == 2 && Number( w[1], value ) && value >= 0.0 )
		{
			m_settings[verb] = value;
		}
		else if ( verb == "cooldown" && w.size() == 4 && w[2].text == "shared" && Number( w[1], value ) && value >= 0.0 )
		{
			// cooldown S shared NAME (M288): every motion naming it waits for it
			m_settings["cooldown"] = value;
			m_settings["cooldown_shared"] = w[3].text;
		}
		else if ( ( verb == "tags" || verb == "cancels" || ( verb == "blocked" && w.size() >= 3 && w[1].text == "by" ) ) && w.size() >= 2 )
		{
			// tags A, B; blocked by A, B; cancels A, B (M288): names, commas or spaces between
			Strings names = m_settings.Get( verb, Strings() ).AsStrings();
			for ( size_t k = verb == "blocked" ? 2 : 1; k < w.size(); ++k )
			{
				for ( const std::string& one : Split( Replace( w[k].text, ",", " " ), " " ) )
				{
					if ( one.empty() == false )
					{
						names.push_back( one );
						// (M296) Every name is one the sheet knows: a tag it declares, for blocked by and
						// cancels also a motion of its own, for blocked by also a status it declares.
						const Tree& tree = m_l.tree;
						const bool tag = tree.tags.count( one ) > 0;
						const bool motion = verb != "tags" && tree.motions.count( one ) > 0;
						const bool status = verb == "blocked" && tree.statuses.count( one ) > 0;
						if ( tag == false && motion == false && status == false )
						{
							Fail( number, verb == "tags" ? "\"" + one + "\" is no tag this sheet declares: tag " + one
										  : verb == "cancels"
											  ? "\"" + one + "\" is no tag this sheet declares, nor one of its motions (declare it: tag " + one + ")"
											  : "\"" + one + "\" is no tag or status this sheet declares, nor one of its motions (declare it: tag " + one +
													", or status " + one + ")" );
						}
					}
				}
			}
			m_settings[verb] = names;
		}
		else if ( verb == "uses" && w.size() >= 2 && IsValidInt( w[1].text ) && ToInt( w[1].text ) > 0 &&
				  ( w.size() == 2 || ( w.size() == 4 && w[2].text == "refill" && w[3].text == "ground" ) ||
					( w.size() == 5 && w[2].text == "refill" && w[3].text == "after" && Number( w[4], value ) && value > 0.0 ) ) )
		{
			m_settings["uses"] = ToInt( w[1].text );
			m_settings["refill"] = w.size() == 4 ? Value( "ground" ) : Value( w.size() == 5 ? value : 0.0 );
		}
		else
		{
			Fail( number, verb == "until"							? std::string( "until EXPRESSION" )
						  : verb == "uses"							? std::string( "uses N, uses N refill ground, or uses N refill after S" )
						  : verb == "tags" || verb == "cancels"	? verb + " NAME, NAME ..."
						  : verb == "blocked"						? std::string( "blocked by NAME, NAME ... (a tag, a motion or a status)" )
																	: verb + " S: S seconds" );
		}
	}

	// find SHAPE [players] [bodies] [self] [max N] [turned facing|look|none] [at POINT] [players first] [in sight];
	// grab, the same, what it finds held while its step lasts.
	bool Find( const std::vector<Token>& w, int number, Dict& statement )
	{
		const std::string verb = w[0].text;
		const char* usage = verb == "find" ? "find SHAPE [players] [bodies] [self] [max N] [turned facing|look|none] [at POINT] [players first] [in sight], SHAPE a node's path, "
											 "sphere R, box (X,Y,Z) or capsule R H; POINT (x, y, z), a marker, look D or hit"
										   : "grab SHAPE [players] [bodies] [max N] [turned facing|look|none] [at POINT] [players first], SHAPE a node's path, "
											 "sphere R, box (X,Y,Z) or capsule R H; POINT (x, y, z), a marker, look D or hit";
		size_t i = 1;
		Dict shape;
		double a = 0.0, b = 0.0;
		if ( w.size() >= 3 && w[1].text == "sphere" && Number( w[2], a ) && a > 0.0 )
		{
			shape["kind"] = "sphere";
			shape["radius"] = a;
			i = 3;
		}
		else if ( w.size() >= 3 && w[1].text == "box" && ValueOf( w[2] ).type() == Value::Type::Vec3 )
		{
			shape["kind"] = "box";
			shape["size"] = ValueOf( w[2] );
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
			Fail( number, usage );
			return false;
		}
		bool players = false, bodies = false, ok = true;
		for ( ; i < w.size() && ok; )
		{
			const std::string word = w[i].text;
			if ( word == "in" && i + 1 < w.size() && w[i + 1].text == "sight" )
			{
				statement["sight"] = true; // (M292: what the level does not hide from its place)
				i += 2;
			}
			else if ( word == "players" && i + 1 < w.size() && w[i + 1].text == "first" )
			{
				statement["first"] = true; // (M290: players before bodies, whatever is nearer)
				i += 2;
			}
			else if ( word == "players" || word == "bodies" )
			{
				( word == "players" ? players : bodies ) = true;
				i += 1;
			}
			else if ( word == "self" )
			{
				statement["self"] = true;
				i += 1;
			}
			else if ( word == "max" && i + 1 < w.size() && IsValidInt( w[i + 1].text ) && ToInt( w[i + 1].text ) >= 1 && ToInt( w[i + 1].text ) <= 16 )
			{
				statement["most"] = ToInt( w[i + 1].text );
				i += 2;
			}
			else if ( ( word == "turned" && i + 1 < w.size() && ( w[i + 1].text == "facing" || w[i + 1].text == "look" || w[i + 1].text == "none" ) ) ||
					  ( word == "along" && i + 1 < w.size() && ( w[i + 1].text == "facing" || w[i + 1].text == "look" || w[i + 1].text == "world" ) ) )
			{
				// turned facing (the default), look (with the pitch) or none; "along" the older word
				statement["frame"] = w[i + 1].text == "none" ? std::string( "world" ) : w[i + 1].text;
				i += 2;
			}
			else if ( word == "at" && i + 1 < w.size() && ValueOf( w[i + 1] ).type() == Value::Type::Vec3 )
			{
				// at (x, y, z): moved from where it is placed, in the player's frame (metres right, up and
				// ahead)
				shape["at"] = ValueOf( w[i + 1] );
				i += 2;
			}
			else if ( word == "at" && i + 2 < w.size() && w[i + 1].text == "look" )
			{
				statement["point"] = "look " + w[i + 2].text; // (M290: along the look, the level stops it)
				i += 3;
			}
			else if ( word == "at" && i + 1 < w.size() && ( w[i + 1].text == "hit" || w[i + 1].text == "target" ) )
			{
				statement["point"] = std::string( "hit" ); // (where the cast holds)
				i += 2;
			}
			else if ( word == "at" && i + 1 < w.size() && w[i + 1].quoted == false )
			{
				shape["at_node"] = w[i + 1].text; // (a marker drawn in the scene: its place)
				i += 2;
			}
			else
			{
				ok = false;
			}
		}
		if ( ok == false )
		{
			Fail( number, usage );
			return false;
		}
		if ( verb == "grab" && ( statement.Get( "self", false ).AsBool() || statement.Get( "most", 1 ).AsInt() > 8 ) )
		{
			Fail( number, "a grab holds 8 at most, and not the player itself" );
			return false;
		}
		statement["kind"] = "find";
		statement["grab"] = verb == "grab";
		if ( verb == "grab" && statement.Has( "most" ) == false )
		{
			statement["most"] = 8;
		}
		statement["shape"] = shape;
		statement["who"] = players == bodies ? "all" : players ? "players" : "bodies";
		statement["statements"] = List();
		return true;
	}

	// cast SHAPE range R [speed V] [holds level|bodies|players ...] [along look|facing]; SHAPE: ray, sphere
	// R, or a node's path (one convex shape, around its origin)
	bool Cast( const std::vector<Token>& w, int number, Dict& statement )
	{
		const char* usage = "cast SHAPE range R [speed V] [holds level bodies players] [along look|facing], SHAPE ray, sphere R, or a node's path";
		size_t i = 1;
		Dict shape;
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
			Fail( number, usage );
			return false;
		}
		Strings holds;
		bool ok = true;
		for ( ; i < w.size() && ok; )
		{
			const std::string word = w[i].text;
			if ( ( word == "range" || word == "speed" ) && i + 1 < w.size() && Number( w[i + 1], a ) && a >= 0.0 )
			{
				statement[word] = a;
				i += 2;
			}
			else if ( word == "along" && i + 1 < w.size() && ( w[i + 1].text == "look" || w[i + 1].text == "facing" ) )
			{
				statement["along"] = w[i + 1].text;
				i += 2;
			}
			else if ( word == "holds" )
			{
				i += 1;
				for ( ; i < w.size() && ( w[i].text == "level" || w[i].text == "bodies" || w[i].text == "players" ); ++i )
				{
					holds.push_back( w[i].text );
				}
				ok = holds.empty() == false;
			}
			else
			{
				ok = false;
			}
		}
		if ( ok == false || statement.Has( "range" ) == false )
		{
			Fail( number, usage );
			return false;
		}
		statement["kind"] = "cast";
		statement["shape"] = shape;
		statement["holds"] = holds;
		statement["statements"] = List();
		return true;
	}

	// The lines under a find, a grab or a cast: what happens to each thing it found (or holds).
	void Inside( const std::vector<Node>& lines, Dict& find, bool castBlock )
	{
		std::vector<Frame> frames;
		InsideLines( lines, find, castBlock, frames );
	}

	void InsideLines( const std::vector<Node>& lines, Dict& find, bool castBlock, std::vector<Frame>& frames )
	{
		List inside = find["statements"].AsList();
		const bool grabbing = find.Get( "grab", false ).AsBool();
		const Node* lastIf = nullptr;
		for ( const Node& node : lines )
		{
			const Node* previous = lastIf;
			lastIf = nullptr;
			const int number = node.line.number;
			const std::string line = Resolved( m_l.tree, node.line.code, true );
			const std::vector<Token> w = SplitLine( line );
			const std::string verb = w[0].text;
			Dict statement;
			statement["line"] = number;
			const std::string placed = verb == "move" && w.size() > 1 && w[1].text == "for" ? std::string( "move for" ) : verb;
			const std::string misplaced = Misplaced( placed, castBlock ? kInCast : grabbing ? kInGrab : kInFind, false );
			if ( misplaced.empty() == false )
			{
				Fail( number, misplaced );
				continue;
			}
			statement["verb"] = placed;
			// (M290) An if under a find: decided for each thing it found.
			if ( verb == "if" || verb == "else" )
			{
				if ( castBlock )
				{
					Fail( number, "an if is not in a cast" );
					continue;
				}
				if ( verb == "if" )
				{
					if ( w.size() < 2 )
					{
						Fail( number, "if EXPRESSION" );
						continue;
					}
					frames.push_back( { RestFrom( line, w[1] ), false } );
					InsideLines( node.under, find, castBlock, frames );
					frames.pop_back();
					lastIf = &node;
					continue;
				}
				if ( w.size() != 1 || previous == nullptr )
				{
					Fail( number, "an else comes right after an if's lines, as far in as the if" );
					continue;
				}
				const std::string condition = Resolved( m_l.tree, previous->line.code, true );
				frames.push_back( { RestFrom( condition, SplitLine( condition )[1] ), true } );
				InsideLines( node.under, find, castBlock, frames );
				frames.pop_back();
				continue;
			}
			if ( node.under.empty() == false )
			{
				Fail( node.under[0].line.number, "nothing goes under \"" + verb + "\": only an if and an else have lines under them here" );
			}
			statement["only"] = Conjunction( frames );
			if ( verb == "change" )
			{
				// (Of the motion's owner, once for each thing found: a count of what was hit.)
				if ( w.size() < 4 )
				{
					Fail( number, "change FIELD = VALUE, += or -=" );
					continue;
				}
				statement["kind"] = "change";
				statement["text"] = RestFrom( line, w[1] );
			}
			else if ( verb == "apply" && w.size() >= 4 && w[2].text == "for" )
			{
				// (On each player found, M288; on the player a cast holds, M290. "on target" says so.)
				statement["kind"] = "apply";
				statement["text"] = w[1].text;
				std::string seconds = RestFrom( line, w[3] );
				if ( w.size() >= 6 && w[w.size() - 2].text == "on" )
				{
					if ( w[w.size() - 1].text != "target" )
					{
						Fail( number, "apply STATUS for S [on target]: in a find or a cast, on what it found or holds" );
						continue;
					}
					seconds = Strip( seconds.substr( 0, seconds.rfind( " on " ) ) );
				}
				statement["value"] = seconds;
			}
			else if ( verb == "emit" && w.size() == 2 )
			{
				if ( castBlock && inside.size() > 0 )
				{
					bool emitted = false;
					for ( size_t k = 0; k < inside.size(); ++k )
					{
						emitted = emitted || inside[k].AsDict().Get( "kind", "" ).AsString() == "emit";
					}
					if ( emitted )
					{
						Fail( number, "a cast says one event when it takes hold" );
						continue;
					}
				}
				statement["kind"] = "emit";
				statement["text"] = w[1].text;
			}
			else
			{
				Dict part;
				std::string why;
				if ( ParsePart( w, part, why ) == false )
				{
					Fail( number, why );
					continue;
				}
				Dict props = part["properties"].AsDict();
				if ( ( verb == "impulse" || verb == "throw" || verb == "force" || verb == "steer" ) && props.Has( "target" ) == false )
				{
					props["target"] = castBlock ? 1 : 3; // (what it holds, or found, unless it says)
				}
				ResolveTarget( props, castBlock == false, castBlock );
				statement["kind"] = "part";
				statement["part"] = part;
			}
			inside.push_back( statement );
		}
	}

	void Finish()
	{
		if ( m_bad )
		{
			return;
		}
		const int line = m_block.line.number;
		if ( m_settings.Get( "owner", "player" ).AsString() == "item" )
		{
			// An item's motion (on item) happens in one tick: what it may have is checked as a whole,
			// since "on item" may come after its first lines.
			const std::string once = "an item's motion (on item) happens in one tick, on an event said at it: ";
			if ( m_steps.size() > 1 )
			{
				m_l.Fail( int( m_steps[1].AsDict().Get( "line", line ).AsInt() ), once + "no wait or timeout" );
				return;
			}
			for ( const char* setting : { "cooldown", "uses", "lasts", "until", "conditions" } )
			{
				if ( m_settings.Has( setting ) )
				{
					m_l.Fail( line, once + "no " + ( std::string( setting ) == "conditions" ? std::string( "if on its first line (an if block is fine)" ) : std::string( setting ) ) );
					return;
				}
			}
			List list = m_steps[0].AsDict().Get( "statements", List() ).AsList();
			for ( size_t k = 0; k < list.size(); ++k )
			{
				Dict statement = list[k].AsDict();
				const std::string misplaced = Misplaced( statement.Get( "verb", "" ).AsString(), kInStep, true );
				if ( misplaced.empty() == false )
				{
					m_l.Fail( int( statement.Get( "line", line ).AsInt() ), misplaced );
					return;
				}
			}
		}
		else
		{
			for ( size_t s = 0; s < m_steps.size(); ++s )
			{
				List list = m_steps[s].AsDict().Get( "statements", List() ).AsList();
				for ( size_t k = 0; k < list.size(); ++k )
				{
					Dict statement = list[k].AsDict();
					if ( statement.Get( "verb", "" ).AsString() == "remove" )
					{
						m_l.Fail( int( statement.Get( "line", line ).AsInt() ), Misplaced( "remove", kInStep, false ) );
						return;
					}
				}
			}
		}
		Rule made;
		made.line = line;
		made.type = "motion";
		made.properties = m_settings;
		made.properties["steps"] = m_steps;
		m_l.out.push_back( made );
	}
};

// A reaction block (react on EVENT [if EXPRESSION] | react while EXPRESSION): one CbReaction for each
// action under it, with the block's settings and the scopes it is under.
class ReactionLowering
{
public:
	ReactionLowering( const Lowering& lowering, const Node& block )
		: m_l( lowering )
		, m_block( block )
	{
	}

	void Lower()
	{
		if ( Head() == false )
		{
			return;
		}
		std::vector<const Node*> scopes;
		Lines( m_block.under, scopes );
		if ( m_actions == 0 )
		{
			m_l.Fail( m_block.line.number, "the reaction does nothing: give it an action (sound, create, play, set, call, shake, flash)" );
		}
	}

private:
	const Lowering& m_l;
	const Node& m_block;
	Dict m_head;
	struct Is
	{
		std::string subject;
		std::string what;
	};
	std::vector<Is> m_is;
	std::vector<const Node*> m_settings; // cooldown, server only
	bool m_begun = false;
	int m_actions = 0;

	bool Head()
	{
		const int head = m_block.line.number;
		const std::string& code = m_block.line.code;
		const std::vector<Word> w = WordsOf( code );
		std::string condition;
		const bool on = w.size() >= 3 && w[1].text == "on";
		if ( on )
		{
			if ( w.size() > 3 && ( w[3].text != "if" || w.size() < 5 ) )
			{
				m_l.Fail( head, "react on EVENT [if EXPRESSION]" );
				return false;
			}
			condition = From( code, w, 4 );
		}
		else if ( w.size() >= 3 && w[1].text == "while" )
		{
			condition = From( code, w, 2 );
		}
		else
		{
			m_l.Fail( head, "react on EVENT [if EXPRESSION], or react while EXPRESSION" );
			return false;
		}
		// "actor is player and ...": what its subject must be.
		for ( ;; )
		{
			const std::vector<Word> c = WordsOf( condition );
			if ( c.size() < 3 || c[1].text != "is" )
			{
				break;
			}
			m_is.push_back( Is{ c[0].text, c[2].text } );
			if ( c.size() == 3 )
			{
				condition.clear();
				break;
			}
			if ( c[3].text != "and" || c.size() < 5 )
			{
				m_l.Fail( head, "after " + c[0].text + " is " + c[2].text + ": and EXPRESSION" );
				return false;
			}
			condition = From( condition, c, 4 );
		}
		if ( on == false && condition.empty() )
		{
			m_l.Fail( head, "react while EXPRESSION" );
			return false;
		}
		m_head["when"] = on ? kReactionOnEvent : kReactionWhile;
		if ( on )
		{
			m_head["event"] = w[2].text;
		}
		if ( condition.empty() == false )
		{
			m_head["conditions"] = Strings{ Strip( EngineExpression( m_l.tree, condition, false ) ) };
		}
		return true;
	}

	void Lines( const std::vector<Node>& lines, std::vector<const Node*>& scopes )
	{
		for ( const Node& node : lines )
		{
			const int number = node.line.number;
			const std::vector<Word> v = WordsOf( node.line.code );
			const std::string verb = v[0].text;
			if ( verb == "cooldown" || verb == "server" )
			{
				Dict scratch;
				std::string why;
				if ( m_begun || scopes.empty() == false )
				{
					m_l.Fail( number, "\"" + verb + "\" is the reaction's: before its scopes and actions" );
				}
				else if ( Into( node, scratch, why ) == false )
				{
					m_l.Fail( number, why );
				}
				else
				{
					m_settings.push_back( &node );
				}
				continue;
			}
			m_begun = true;
			if ( verb == "as" || verb == "at" || verb == "after" || verb == "chance" )
			{
				bool twice = false;
				for ( const Node* s : scopes )
				{
					if ( WordsOf( s->line.code )[0].text == verb )
					{
						m_l.Fail( number, "an " + verb + " under an " + verb + " (line " + std::to_string( s->line.number ) + "): one holds at a time" );
						twice = true;
					}
				}
				Dict scratch;
				std::string why;
				if ( Into( node, scratch, why ) == false )
				{
					m_l.Fail( number, why );
					m_actions += 1; // (what is under it is not read: it is not a reaction that does nothing)
					continue;
				}
				if ( node.under.empty() )
				{
					m_l.Fail( number, node.line.code + ": nothing is under it (indent the lines it is for)" );
					continue;
				}
				if ( twice )
				{
					m_actions += 1;
					continue;
				}
				scopes.push_back( &node );
				Lines( node.under, scopes );
				scopes.pop_back();
				continue;
			}
			if ( node.under.empty() == false )
			{
				m_l.Fail( node.under[0].line.number, "nothing goes under \"" + verb + "\": only as, at, after and chance have lines under them" );
			}
			Action( node, scopes );
		}
	}

	// A setting of the block (cooldown S, server only) or a scope (as, at, after, chance) into the
	// properties of the actions under it. False, with why, when it cannot be read.
	static bool Into( const Node& node, Dict& current, std::string& why )
	{
		const std::vector<Token> w = SplitLine( node.line.code );
		const std::string verb = w[0].text;
		double value = 0.0;
		if ( verb == "server" )
		{
			if ( w.size() != 2 || w[1].text != "only" )
			{
				why = "server only: it waits for the server's cue (a press of the viewer's is not guessed)";
				return false;
			}
			current["wait_for_server"] = true;
			return true;
		}
		if ( verb == "as" )
		{
			if ( w.size() != 2 )
			{
				why = "as holder, actor, target, me, local, world or a path: one word";
				return false;
			}
			current["subject"] = EnginePath( w[1].text );
			return true;
		}
		if ( verb == "cooldown" || verb == "after" || verb == "chance" )
		{
			if ( w.size() != 2 || Number( w[1], value ) == false )
			{
				why = verb + " S: a number";
				return false;
			}
			current[verb == "after" ? "delay" : verb] = value;
			return true;
		}
		// at point | end | beam [from PATH] | here | PATH [attached], then [offset X Y Z]. One way to say
		// where (M296, the owner): `attached` makes what is created ride along with the place, a child of
		// it (until M296 `create ... under PATH`, and `at follow` for the subject).
		std::vector<Token> at = w;
		if ( at.size() >= 2 && at[1].text == "follow" )
		{
			why = "at follow went (M296): at holder attached (or the subject's own path) makes it ride along";
			return false;
		}
		const bool attached = at.size() >= 3 && at[2].text == "attached";
		if ( attached )
		{
			const std::string& place = at[1].text;
			if ( place == "point" || place == "end" || place == "beam" || place == "here" || at.size() != 3 )
			{
				why = "at PATH attached: a node (or holder, me, actor, target, local) it rides along with, nothing after it";
				return false;
			}
			current.Erase( "place_node" );
			current.Erase( "offset" );
			current["place"] = 0;
			current["scene_parent"] = EnginePath( place );
			return true;
		}
		for ( size_t k = 1; k < at.size(); ++k )
		{
			const std::string& word = at[k].text;
			const bool place = k == 1 && word != "point" && word != "end" && word != "beam";
			if ( place || ( k > 1 && at[k - 1].text == "from" ) )
			{
				at[k].text = EnginePath( word );
			}
		}
		size_t next = 0;
		current.Erase( "place_node" );
		current.Erase( "offset" );
		if ( at.size() == 2 && at[1].text == "here" )
		{
			current["place"] = 0; // where the sheet's node is (under its parent)
			return true;
		}
		if ( at.size() < 2 )
		{
			why = "at point, end, beam from PATH, or a path [attached]";
			return false;
		}
		if ( ParseAt( at, 1, at.size(), current, next, why ) == false || next != at.size() )
		{
			why = why.empty() ? std::string( "at PLACE, then: from PATH, offset X Y Z" ) : why;
			return false;
		}
		return true;
	}

	// A reaction's options after its own words: name and value pairs, each a property.
	bool Options( const std::vector<Token>& w, const std::string& line, size_t from, Dict& into, const std::string& verb,
				  const std::vector<std::pair<const char*, const char*>>& names, int number )
	{
		for ( size_t i = from; i < w.size(); i += 2 )
		{
			const std::string name = w[i].text;
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
				m_l.Fail( number, "\"" + name + "\" is not something " + verb + " takes" );
				return false;
			}
			const std::string key = property;
			if ( key == "bus" || key == "animation_off" || key == "scene_parent" )
			{
				into[key] = key == "scene_parent" ? Value( EnginePath( w[i + 1].text ) ) : Value( w[i + 1].text ); // (words, not numbers)
			}
			else if ( Number( w[i + 1], value ) )
			{
				into[key] = value;
			}
			else
			{
				m_l.Fail( number, name + " wants a number, not \"" + w[i + 1].text + "\"" );
				return false;
			}
		}
		return true;
	}

	void Action( const Node& node, const std::vector<const Node*>& scopes )
	{
		const int number = node.line.number;
		const std::string& code = node.line.code;
		const std::vector<Token> w = SplitLine( code );
		const std::string verb = w[0].text;
		m_actions += 1; // (one it has, read or not: the reaction is not one that does nothing)
		// What it is under: the block's settings, then its scopes, outermost first.
		Dict current;
		std::string subject;
		for ( const Node* s : scopes )
		{
			const std::vector<Word> sw = WordsOf( s->line.code );
			subject = sw[0].text == "as" ? sw[1].text : subject;
		}
		bool ok = true;
		for ( const Is& one : m_is )
		{
			if ( one.subject != subject )
			{
				m_l.Fail( number, "\"" + one.subject + " is " + one.what + "\" asks what its subject is: this line must be under as " + one.subject );
				ok = false;
			}
		}
		if ( ok == false )
		{
			return;
		}
		for ( const Is& one : m_is )
		{
			const bool kind = one.what == "any" || one.what == "player" || one.what == "prop" || one.what == "static" || one.what == "ragdoll" || one.what == "item";
			current[kind ? "subject_kind" : "subject_template"] = one.what;
		}
		std::string unused;
		for ( const Node* s : m_settings )
		{
			Into( *s, current, unused );
		}
		for ( const Node* s : scopes )
		{
			Into( *s, current, unused );
		}
		Dict p;
		double value = 0.0;
		if ( verb == "sound" )
		{
			if ( w.size() < 2 )
			{
				m_l.Fail( number, "sound NAME [volume DB] [pitch P] [jitter J] [range M] [bus NAME] [volume_by EXPRESSION]" );
				return;
			}
			p["sound"] = SoundPath( w[1].text );
			const std::vector<Word> words = WordsOf( code );
			const std::string rest = EngineExpression( m_l.tree, From( code, words, 2 ), false );
			if ( Options( SplitLine( rest ), rest, 0, p, verb,
						  { { "volume", "volume_db" }, { "pitch", "pitch_scale" }, { "jitter", "pitch_jitter" }, { "range", "max_distance" }, { "bus", "bus" } },
						  number ) == false )
			{
				return;
			}
		}
		else if ( verb == "create" )
		{
			if ( w.size() < 2 )
			{
				m_l.Fail( number, "create NAME [for S]" );
				return;
			}
			p["scene"] = EffectPath( w[1].text );
			if ( std::any_of( w.begin() + 2, w.end(), []( const Token& t ) { return t.text == "under" && t.quoted == false; } ) )
			{
				m_l.Fail( number, "create ... under went (M296): write at PATH attached above it (one way to say where)" );
				return;
			}
			if ( Options( w, code, 2, p, verb, { { "for", "scene_lifetime" } }, number ) == false )
			{
				return;
			}
		}
		else if ( verb == "play" )
		{
			if ( w.size() < 3 )
			{
				m_l.Fail( number, "play PATH ANIMATION [off ANIMATION]" );
				return;
			}
			p["animation_player"] = EnginePath( w[1].text );
			p["animation"] = w[2].text;
			if ( Options( w, code, 3, p, verb, { { "off", "animation_off" } }, number ) == false )
			{
				return;
			}
		}
		else if ( verb == "set" )
		{
			if ( w.size() < 4 )
			{
				m_l.Fail( number, "set PATH PROPERTY VALUE [blend S], or set PATH PROPERTY = EXPRESSION" );
				return;
			}
			p["target"] = EnginePath( w[1].text );
			p["property"] = w[2].text;
			if ( w[3].text == "=" && w[3].quoted == false )
			{
				if ( w.size() < 5 )
				{
					m_l.Fail( number, "set PATH PROPERTY = EXPRESSION" );
					return;
				}
				const std::vector<Word> words = WordsOf( code );
				p["value_expression"] = Strip( EngineExpression( m_l.tree, From( code, words, 4 ), false ) );
			}
			else
			{
				p["value"] = ValueOf( w[3] );
				if ( Options( w, code, 4, p, verb, { { "blend", "blend_time" } }, number ) == false )
				{
					return;
				}
			}
		}
		else if ( verb == "call" )
		{
			if ( w.size() < 3 )
			{
				m_l.Fail( number, "call PATH METHOD [ARGUMENT ...]" );
				return;
			}
			p["target"] = EnginePath( w[1].text );
			p["method"] = w[2].text;
			List args;
			for ( size_t i = 3; i < w.size(); ++i )
			{
				args.push_back( ValueOf( w[i] ) );
			}
			p["method_args"] = args;
		}
		else if ( verb == "shake" )
		{
			if ( w.size() < 2 || Number( w[1], value ) == false )
			{
				m_l.Fail( number, "shake AMOUNT [for S]: 0 to 1" );
				return;
			}
			p["shake"] = value;
			if ( Options( w, code, 2, p, verb, { { "for", "shake_time" } }, number ) == false )
			{
				return;
			}
		}
		else if ( verb == "flash" )
		{
			const Value color = w.size() >= 2 ? ValueOf( w[1] ) : Value();
			if ( color.type() != Value::Type::Color )
			{
				m_l.Fail( number, "flash rgba(r, g, b, a) [for S]" );
				return;
			}
			p["flash_color"] = color;
			if ( Options( w, code, 2, p, verb, { { "for", "flash_time" } }, number ) == false )
			{
				return;
			}
		}
		else
		{
			m_l.Fail( number, "\"" + verb + "\" is not something a reaction does: sound, create, play, set, call, shake or flash" );
			return;
		}
		Dict taken = current.Duplicate();
		taken.Merge( p, true );
		Rule made;
		made.line = number;
		made.head = m_block.line.number;
		made.properties = m_head.Duplicate();
		made.properties.Merge( taken, true );
		m_l.out.push_back( made );
	}
};

// predict ACTION as CUE [if EXPRESSION]: the viewer's own press, shown as the cue the server will
// answer with (a CbPrediction).
void LowerPredict( const Lowering& l, const Node& block )
{
	const int number = block.line.number;
	const std::vector<Word> words = WordsOf( block.line.code );
	std::string line = block.line.code;
	if ( words.size() > 5 && words[4].text == "if" )
	{
		line = line.substr( 0, words[5].start ) + EngineExpression( l.tree, From( line, words, 5 ), false );
	}
	const std::vector<Token> w = SplitLine( line );
	Dict settings;
	bool bad = false;
	if ( w.size() < 4 || w[2].text != "as" )
	{
		l.Fail( number, "predict ACTION as CUE [if EXPRESSION]" );
		bad = true;
	}
	else
	{
		settings["action"] = w[1].text;
		settings["cue"] = w[3].text;
		if ( w.size() >= 5 )
		{
			if ( w[4].text != "if" || w.size() < 6 )
			{
				l.Fail( number, "after the cue: if EXPRESSION" );
				bad = true;
			}
			else
			{
				settings["conditions"] = Strings{ RestFrom( line, w[5] ) };
			}
		}
	}
	Strings changes;
	for ( const Node& node : block.under )
	{
		const std::string body = Resolved( l.tree, node.line.code, false );
		const std::vector<Token> b = SplitLine( body );
		const std::string verb = b[0].text;
		double seconds = 0.0;
		if ( node.under.empty() == false )
		{
			l.Fail( node.under[0].line.number, "nothing goes under a prediction's lines" );
			bad = true;
		}
		if ( verb == "cooldown" && b.size() == 2 && Number( b[1], seconds ) )
		{
			settings["cooldown"] = seconds;
		}
		else if ( verb == "held" && b.size() == 1 )
		{
			settings["while_held"] = true;
		}
		else if ( verb == "change" && b.size() >= 4 )
		{
			changes.push_back( RestFrom( body, b[1] ) );
		}
		else if ( verb == "stance" && ( b.size() == 2 || ( b.size() == 4 && b[2].text == "on" ) ) )
		{
			settings["stance"] = b[1].text;
			if ( b.size() == 4 )
			{
				settings["stance_layer"] = b[3].text;
			}
		}
		else
		{
			l.Fail( node.line.number, "a prediction takes cooldown S, held, change FIELD OP NUMBER, stance NAME [on LAYER]" );
			bad = true;
		}
	}
	if ( bad )
	{
		return;
	}
	Rule made;
	made.line = number;
	made.prediction = true;
	made.type = "CbPrediction";
	made.properties = settings;
	if ( changes.empty() == false )
	{
		made.properties["changes"] = changes;
	}
	l.out.push_back( made );
}

// hud PATH: one control's lines (a CbFieldBinding each): show if, text, bind.
void LowerHud( const Lowering& l, const Node& block )
{
	const std::vector<Word> head = WordsOf( block.line.code );
	if ( head.size() != 2 )
	{
		l.Fail( block.line.number, "hud PATH: the control its lines are about" );
		return;
	}
	const std::string path = head[1].text;
	if ( block.under.empty() )
	{
		l.Fail( block.line.number, "hud " + path + ": nothing is under it" );
		return;
	}
	for ( const Node& node : block.under )
	{
		const int number = node.line.number;
		const std::vector<Word> v = WordsOf( node.line.code );
		const std::string verb = v[0].text;
		if ( node.under.empty() == false )
		{
			l.Fail( node.under[0].line.number, "nothing goes under a hud's lines" );
		}
		const char* usage = "a hud's lines are show if EXPRESSION, text \"FORMAT\" [into PROPERTY] [if EXPRESSION], bind PROPERTY = EXPRESSION [if EXPRESSION]";
		if ( ( verb != "show" && verb != "text" && verb != "bind" ) || v.size() < 2 )
		{
			l.Fail( number, usage );
			continue;
		}
		// The line after its verb, its expressions in the engine's spelling; "... if EXPRESSION": the
		// condition, to the end of the line.
		const std::string line = EngineExpression( l.tree, From( node.line.code, v, 1 ), false );
		const std::vector<Token> w = SplitLine( line );
		size_t cut = w.size();
		for ( size_t k = 0; k < w.size(); ++k )
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
		Dict& p = made.properties;
		if ( cut < w.size() )
		{
			if ( cut + 1 >= w.size() )
			{
				l.Fail( number, "if what? if EXPRESSION" );
				continue;
			}
			p["conditions"] = Strings{ RestFrom( line, w[cut + 1] ) };
		}
		std::string why;
		if ( verb == "show" )
		{
			if ( cut != 0 || p.Has( "conditions" ) == false )
			{
				why = "show if EXPRESSION";
			}
		}
		else if ( verb == "text" )
		{
			p["hides"] = false;
			if ( cut < 1 )
			{
				why = "text \"FORMAT\" [into PROPERTY] [if EXPRESSION]";
			}
			else
			{
				// (M296) A motion's clock in a format, as in an expression: "{cooldown Strike}" is
				// {cooldown.Strike}; so ready, uses and remaining.
				std::string format = w[0].text;
				for ( size_t open = format.find( '{' ); open != std::string::npos; open = format.find( '{', open + 1 ) )
				{
					const size_t close = format.find( '}', open );
					const std::vector<Word> inside = close == std::string::npos ? std::vector<Word>{} : WordsOf( format.substr( open + 1, close - open - 1 ) );
					if ( inside.size() == 2 && ( inside[0].text == "ready" || inside[0].text == "cooldown" || inside[0].text == "uses" || inside[0].text == "remaining" ) )
					{
						format.replace( open + 1, close - open - 1, inside[0].text + "." + inside[1].text );
					}
				}
				p["text_format"] = format;
				if ( cut == 3 && w[1].text == "into" )
				{
					p["property"] = w[2].text;
				}
				else if ( cut != 1 )
				{
					why = "after the format: into PROPERTY, or if EXPRESSION";
				}
			}
		}
		else
		{
			p["hides"] = false;
			if ( cut < 3 || w[1].text != "=" )
			{
				why = "bind PROPERTY = EXPRESSION [if EXPRESSION]";
			}
			else
			{
				p["property"] = w[0].text;
				// (The expression, up to "if" or the end of the line.)
				p["field"] = cut < w.size() ? Strip( line.substr( w[2].start, w[cut].start - w[2].start ) ) : RestFrom( line, w[2] );
			}
		}
		if ( why.empty() == false )
		{
			l.Fail( number, why );
			continue;
		}
		p["target"] = path;
		l.out.push_back( made );
	}
}

// The number a mistake is said at ("line N: ..."), for putting them in order.
int MistakeLine( const std::string& error )
{
	const size_t colon = error.find( ':' );
	return error.rfind( "line ", 0 ) == 0 && colon != std::string::npos && IsValidInt( error.substr( 5, colon - 5 ) ) ? int( ToInt( error.substr( 5, colon - 5 ) ) ) : 0;
}
} // namespace

void SetMoveParameterNames( const Strings& names )
{
	MoveParameters() = names;
}

bool IsPathProperty( const std::string& name )
{
	return name == "subject" || name == "place_node" || name == "target" || name == "scene_parent" || name == "animation_player";
}

bool ParseChange( const std::string& text, Change& out, std::string* error )
{
	auto fail = [&]( const std::string& why ) {
		if ( error != nullptr )
		{
			*error = why;
		}
		return false;
	};
	const std::string rest = Strip( text );
	const size_t at = rest.find( '=' );
	if ( at == std::string::npos || at == 0 )
	{
		return fail( "write it as a field, an operator and a number: pistol.ammo -= 1 (operators: -=, +=, =)" );
	}
	out = Change();
	const char before = rest[at - 1];
	std::string name = rest.substr( 0, at );
	if ( before == '-' || before == '+' )
	{
		out.op = before;
		name = rest.substr( 0, at - 1 );
	}
	out.field = Strip( name );
	const std::string number = Strip( rest.substr( at + 1 ) );
	if ( out.field.empty() || Contains( out.field, " " ) )
	{
		return fail( "name one field before the operator" );
	}
	if ( IsValidFloat( number ) == false )
	{
		return fail( "\"" + number + "\" is not a number" );
	}
	out.value = ToFloat( number );
	return true;
}

std::vector<Rule> ParseRules( const std::string& text, Strings& errors )
{
	bool any = false;
	for ( const std::string& line : LinesOf( text ) )
	{
		any = any || CodeOf( line ).empty() == false;
	}
	if ( any == false )
	{
		return {};
	}
	if ( SheetVersion( text ) != 1 )
	{
		errors.push_back( "line 1: " + NotVersion1( text ) );
		return {};
	}
	Strings found;
	const Tree tree = ReadTree( text, found );
	std::vector<Rule> out;
	const Lowering lowering{ tree, found, out };
	for ( const Node& block : tree.blocks )
	{
		const std::string verb = block.Verb();
		if ( verb == "motion" || verb == "use" )
		{
			MotionLowering( lowering, block ).Lower();
		}
		else if ( verb == "react" )
		{
			ReactionLowering( lowering, block ).Lower();
		}
		else if ( verb == "predict" )
		{
			LowerPredict( lowering, block );
		}
		else
		{
			LowerHud( lowering, block );
		}
	}
	// (In the order of their lines.)
	std::stable_sort( found.begin(), found.end(), []( const std::string& a, const std::string& b ) { return MistakeLine( a ) < MistakeLine( b ); } );
	errors.insert( errors.end(), found.begin(), found.end() );
	return out;
}

} // namespace cb::sheet
