#include "sheet_rules.h"

#include "sheet_text.h"

#include <utility>
#include <vector>

// Moved from src/godot/cue/cue_rules.cpp in M283, line for line: what the parser does is what it
// did (the golden files check it), only the types are plain C++.

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

// How far in a line's text starts: a tab is four spaces.
int Column( const std::string& line )
{
	int column = 0;
	for ( char c : line )
	{
		if ( c == ' ' )
		{
			column += 1;
		}
		else if ( c == '\t' )
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
// (docs/sheets.md, "Where a line may stand") is the same table.
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
		return "\"" + verb + "\" is an item's motion's (owner item)";
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
	// The words a part shares: on, along, direction.
	auto common = [&]( const std::string& word, size_t& k ) {
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
	if ( verb == "impulse" || verb == "force" )
	{
		// How much: a number, or an expression up to the first word that is an option (read when it
		// acts: "9 * curve(Falloff, reach)", "2 * dash.power").
		auto forceStops = []( const std::string& word ) {
			return word == "on" || word == "along" || word == "direction" || word == "replace" || word == "kind" || word == "top" ||
				   word == "ramp" || word == "react" || word == "turn";
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
		part["class"] = verb == "impulse" ? "CbImpulse" : "CbForce";
		{
			// (The words before the first option word, found above.)
			Amount main;
			main.numeric = options == 2 && Number( w[1], main.number );
			for ( size_t j = 1; j < options; ++j )
			{
				main.text += ( j > 1 ? " " : "" ) + w[j].text;
			}
			// (An impulse's speed is its line's third field, a force's strength its fifth.)
			amountInto( main, verb == "impulse" ? "speed" : "strength", verb == "impulse" ? 2 : 4 );
		}
		for ( i = options; i < w.size() && why.empty(); )
		{
			const std::string word = w[i].text;
			if ( common( word, i ) )
			{
				continue;
			}
			if ( verb == "impulse" && word == "replace" && i + 1 < w.size() )
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
			else if ( verb == "impulse" && word == "kind" && i + 1 < w.size() )
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
				const int kind = k == "acceleration" ? 0 : k == "force" ? 1 : k == "velocity" ? 2 : -1;
				if ( kind < 0 )
				{
					why = "kind acceleration, force or velocity";
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
		why = "\"" + verb + "\" is not one of: change, cooldown, emit, impulse, force, link, move, launch";
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

struct Building
{
	int line = 0;
	Dict settings;			// when it is (on, while, its conditions); a prediction: all of it
	Dict current;			// the settings read so far: each action below takes them as they are
	int pendingSetting = 0; // the line of a setting no action has come after yet
	std::vector<Rule> actions;
	bool prediction = false;
	// A motion block (for the bake: run by the simulation). Its steps; the ifs open around the line
	// being read (their column, their condition, whether it is their else now); its lines' columns.
	bool motion = false;
	List steps;
	struct Frame
	{
		int column = 0;
		std::string condition;
		bool otherwise = false;
	};
	std::vector<Frame> frames;
	int headColumn = 0;
	int bodyColumn = -1;
	// The find being read (its column; -1: none): the lines further in are what happens to each thing
	// it found.
	int findColumn = -1;
	Dict find;
	bool castBlock = false; // the open block is a cast (else a find)
	bool cast = false;		// the motion has one
	Strings changes;		// a prediction's
	bool bad = false;
};

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

// Version 0 (sheet_v1.cpp reads version 1 by lowering it to this).
std::vector<Rule> ParseVersion0( const std::string& text, Strings& errors )
{
	std::vector<Rule> out;
	Building rule;
	bool open = false;
	auto fail = [&]( int line, const std::string& why ) {
		errors.push_back( "line " + Num64( line ) + ": " + why );
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
			errors.push_back( "line " + Num64( rule.pendingSetting ) + ": a setting after the last action does nothing (a setting is for the actions below it)" );
			return;
		}
		if ( rule.motion )
		{
			// An item's motion (owner item) happens in one tick: what it may have is checked as a whole,
			// since "owner item" may come after its first lines.
			if ( rule.settings.Get( "owner", "player" ).AsString() == "item" )
			{
				const std::string once = "an item's motion (owner item) happens in one tick, on an event said at it: ";
				if ( rule.steps.size() > 1 )
				{
					errors.push_back( "line " + Num64( rule.steps[1].AsDict().Get( "line", rule.line ).AsInt() ) + ": " + once + "no then, after or timeout" );
					return;
				}
				for ( const char* setting : { "cooldown", "uses", "lasts", "until", "conditions" } )
				{
					if ( rule.settings.Has( setting ) )
					{
						errors.push_back( "line " + Num64( rule.line ) + ": " + once + "no " +
										  ( std::string( setting ) == "conditions" ? std::string( "if on its first line (an if block is fine)" ) : std::string( setting ) ) );
						return;
					}
				}
				List list = rule.steps[0].AsDict().Get( "statements", List() ).AsList();
				for ( size_t k = 0; k < list.size(); ++k )
				{
					Dict statement = list[k].AsDict();
					const std::string misplaced = Misplaced( statement.Get( "verb", "" ).AsString(), kInStep, true );
					if ( misplaced.empty() == false )
					{
						errors.push_back( "line " + Num64( statement.Get( "line", rule.line ).AsInt() ) + ": " + misplaced );
						return;
					}
				}
			}
			else
			{
				for ( size_t s = 0; s < rule.steps.size(); ++s )
				{
					List list = rule.steps[s].AsDict().Get( "statements", List() ).AsList();
					for ( size_t k = 0; k < list.size(); ++k )
					{
						Dict statement = list[k].AsDict();
						if ( statement.Get( "verb", "" ).AsString() == "remove" )
						{
							errors.push_back( "line " + Num64( statement.Get( "line", rule.line ).AsInt() ) + ": " + Misplaced( "remove", kInStep, false ) );
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
			if ( rule.changes.empty() == false )
			{
				made.properties["changes"] = rule.changes;
			}
			out.push_back( made );
			return;
		}
		if ( rule.actions.empty() )
		{
			errors.push_back( "line " + Num64( rule.line ) + ": the rule does nothing: give it an action (sound, scene, play, set, call, shake, flash)" );
			return;
		}
		for ( Rule& action : rule.actions )
		{
			Rule made;
			made.line = action.line;
			made.head = rule.line;
			made.properties = rule.settings.Duplicate();
			made.properties.Merge( action.properties, true );
			out.push_back( made );
		}
	};
	const std::vector<std::string> lines = Split( Replace( text, "\r", "" ), "\n" );
	for ( size_t index = 0; index < lines.size(); ++index )
	{
		const int number = int( index + 1 );
		const std::string& line = lines[index];
		std::vector<Token> w = SplitLine( line );
		if ( w.empty() )
		{
			continue;
		}
		const std::string verb = w[0].text;
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
			Dict& p = made.properties;
			if ( cut < w.size() )
			{
				if ( cut + 1 >= w.size() )
				{
					errors.push_back( "line " + Num64( number ) + ": if what? if <expression>" );
					continue;
				}
				p["conditions"] = Strings{ RestFrom( line, w[cut + 1] ) };
			}
			std::string why;
			if ( w.size() < 2 )
			{
				why = verb + " what? " +
					  ( verb == "show"	 ? "show <node> if <expression>"
						: verb == "text" ? "text <node> \"format\" [into <property>] [if <expression>]"
										 : "bind <node> <property> = <expression> [if <expression>]" );
			}
			else if ( verb == "show" )
			{
				if ( cut != 2 || p.Has( "conditions" ) == false )
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
					std::string expression = RestFrom( line, w[4] );
					if ( cut < w.size() )
					{
						expression = Strip( line.substr( w[4].start, w[cut].start - w[4].start ) );
					}
					p["field"] = expression;
				}
			}
			if ( why.empty() == false )
			{
				errors.push_back( "line " + Num64( number ) + ": " + why );
				continue;
			}
			p["target"] = w[1].text;
			out.push_back( made );
			continue;
		}
		// --- a motion block: for the bake, run by the simulation (docs/sheets.md)
		if ( verb == "motion" )
		{
			finish();
			rule = Building{};
			rule.line = number;
			rule.motion = true;
			rule.headColumn = Column( line );
			open = true;
			Dict first;
			first["statements"] = List();
			rule.steps.push_back( first );
			// motion NAME on use | on press ACTION | on EVENT | while EXPR | when EXPR, then [if EXPR]
			const char* usage = "motion NAME on use, on press ACTION, on EVENT, while EXPRESSION or when EXPRESSION, then [if EXPRESSION]";
			if ( w.size() < 3 || Contains( w[1].text, "/" ) || Contains( w[1].text, "." ) || w[1].quoted )
			{
				fail( number, usage );
				continue;
			}
			rule.settings["name"] = w[1].text;
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
				std::string expression = Strip( line.substr( w[3].start, ( cut < w.size() ? w[cut].start : line.size() ) - w[3].start ) );
				const size_t hash = expression.find( '#' );
				if ( hash != std::string::npos )
				{
					expression = Strip( expression.substr( 0, hash ) );
				}
				rule.settings["expression"] = expression;
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
					fail( number, how == "while" ? std::string( "a while's expression is its condition: no if" ) : std::string( "if what? if <expression>" ) );
					continue;
				}
				rule.settings["conditions"] = Strings{ RestFrom( line, w[cut + 1] ) };
			}
			continue;
		}
		if ( open && rule.motion && verb != "on" && verb != "while" && verb != "predict" && verb != "use" && verb != "show" && verb != "text" &&
			 verb != "bind" )
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
				List inside = rule.find["statements"].AsList();
				Dict statement;
				statement["line"] = number;
				const std::string placed = verb == "move" && w.size() > 1 && w[1].text == "for" ? std::string( "move for" ) : verb;
				const std::string misplaced = Misplaced( placed, rule.castBlock ? kInCast : kInFind, false );
				if ( misplaced.empty() == false )
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
						for ( size_t k = 0; k < inside.size(); ++k )
						{
							emitted = emitted || inside[k].AsDict().Get( "kind", "" ).AsString() == "emit";
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
					Dict part;
					std::string why;
					if ( ParsePart( w, part, why ) == false )
					{
						fail( number, why );
						continue;
					}
					Dict props = part["properties"].AsDict();
					if ( ( verb == "impulse" || ( rule.castBlock && verb == "force" ) ) && props.Has( "target" ) == false )
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
			while ( rule.frames.empty() == false && ( verb == "else" ? rule.frames.back().column > column : rule.frames.back().column >= column ) )
			{
				rule.frames.pop_back();
			}
			const bool top = column <= rule.bodyColumn && rule.frames.empty();
			Dict step = rule.steps[rule.steps.size() - 1].AsDict();
			List statements = step["statements"].AsList();
			// What the line is under: every open if (its else: not it), as one condition.
			auto guard = [&]() {
				std::string all;
				for ( const auto& frame : rule.frames )
				{
					const std::string one = frame.otherwise ? "not ( " + frame.condition + " )" : "( " + frame.condition + " )";
					all += all.empty() ? one : " and " + one;
				}
				return all;
			};
			auto add = [&]( Dict statement ) {
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
				for ( size_t k = 0; k < statements.size(); ++k )
				{
					again = again || statements[k].AsDict().Get( "kind", "" ).AsString() == "every";
				}
				if ( step.Get( "timeout", false ).AsBool() || again )
				{
					fail( number, Misplaced( "every", again ? kInEvery : kInTimeout, false ) );
					continue;
				}
				if ( top == false || w.size() != 2 || Number( w[1], seconds ) == false || seconds <= 0.0 )
				{
					fail( number, "every S: as far in as the motion's first line, not under an if; what follows in its step happens every S seconds" );
					continue;
				}
				Dict every;
				every["kind"] = "every";
				every["verb"] = "every";
				every["seconds"] = seconds;
				every["line"] = number;
				every["guard"] = std::string();
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
				Dict next;
				next["statements"] = List();
				next["line"] = number;
				if ( verb == "timeout" )
				{
					if ( w.size() != 1 || step.Get( "within", 0.0 ).AsFloat() <= 0.0 || step.Get( "timeout", false ).AsBool() || step.Get( "or", false ).AsBool() )
					{
						fail( number, "a timeout comes right after a then when ... within S (a wait ... or S goes on: it has none)" );
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
					// then when EXPRESSION [within S | or S]
					size_t end = w.size();
					if ( w.size() >= 5 && ( w[w.size() - 2].text == "within" || w[w.size() - 2].text == "or" ) && IsValidFloat( w[w.size() - 1].text ) )
					{
						if ( Number( w[w.size() - 1], value ) == false || value <= 0.0 )
						{
							fail( number, "within <seconds>" );
							continue;
						}
						next["within"] = value;
						if ( w[w.size() - 2].text == "or" )
						{
							next["or"] = true; // (it goes on after S without it)
						}
						end = w.size() - 2;
					}
					if ( w.size() < 3 || w[1].text != "when" || end < 3 )
					{
						fail( number, "then when <expression> [within S]" );
						continue;
					}
					next["wait"] = Strip( line.substr( w[2].start, w[end - 1].start + w[end - 1].text.size() - w[2].start ) );
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
					Strings until = rule.settings.Get( "until", Strings() ).AsStrings();
					until.push_back( RestFrom( line, w[1] ) );
					rule.settings["until"] = until;
				}
				else if ( ( verb == "cooldown" || verb == "lasts" ) && w.size() == 2 && Number( w[1], value ) && value >= 0.0 )
				{
					rule.settings[verb] = value;
				}
				else if ( verb == "uses" && w.size() >= 2 && IsValidInt( w[1].text ) && ToInt( w[1].text ) > 0 &&
						  ( w.size() == 2 || ( w.size() == 4 && w[2].text == "refill" && w[3].text == "ground" ) ||
							( w.size() == 5 && w[2].text == "refill" && w[3].text == "after" && Number( w[4], value ) && value > 0.0 ) ) )
				{
					rule.settings["uses"] = ToInt( w[1].text );
					rule.settings["refill"] = w.size() == 4 ? Value( "ground" ) : Value( w.size() == 5 ? value : 0.0 );
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
					fail( number, verb == "until"	 ? std::string( "until <expression>" )
								  : verb == "uses"	 ? std::string( "uses N, uses N refill ground, or uses N refill after S" )
								  : verb == "item"	 ? std::string( "item hand (the default: whoever has it out) or item carried (in the hand or put away)" )
								  : verb == "owner" ? std::string( "owner item (the item runs it, lying in the world) or owner player (the default)" )
													 : verb + " <seconds>" );
				}
				continue;
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
				const bool ownerItem = rule.settings.Get( "owner", "player" ).AsString() == "item";
				// (An item's own line, before "owner item" was said, is checked when the motion ends.)
				const std::string misplaced = placed == "remove" ? std::string() : Misplaced( placed, place, ownerItem );
				if ( misplaced.empty() == false )
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
					fail( number, usage );
					continue;
				}
				bool players = false, bodies = false, ok = true;
				for ( ; i < w.size() && ok; )
				{
					const std::string word = w[i].text;
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
						// at (x, y, z): moved from where it is placed, in the player's frame (metres right,
						// up and ahead)
						shape["at"] = ValueOf( w[i + 1] );
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
				statement["statements"] = List();
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
				const char* usage = "cast SHAPE range R [speed V] [holds level bodies players] [along look|facing], SHAPE ray, sphere R, or a node's path";
				if ( rule.steps.size() > 1 || rule.frames.empty() == false || rule.cast )
				{
					fail( number, rule.cast ? "a motion casts once" : "a cast is thrown when the motion starts: in its first step, not under an if" );
					continue;
				}
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
					fail( number, usage );
					continue;
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
					fail( number, usage );
					continue;
				}
				statement["kind"] = "cast";
				statement["shape"] = shape;
				statement["holds"] = holds;
				statement["statements"] = List();
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
				bool ok = w.size() >= 3;
				auto breakStops = []( const std::string& next ) { return next == "radius" || next == "speed" || next == "seconds"; };
				for ( size_t k = 1; ok && k < w.size(); )
				{
					const std::string word = w[k].text;
					k += 1;
					Amount amount;
					ok = breakStops( word ) && TakeAmount( w, k, breakStops, amount );
					statement[word] = amount.text; // (a number, or an expression: the bake writes it as it is)
				}
				if ( ok == false || statement.Has( "radius" ) == false )
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
				// emit EVENT [value EXPRESSION] (M287: the event's value, read when it happens)
				if ( w.size() != 2 && ( w.size() < 4 || w[2].text != "value" ) )
				{
					fail( number, "emit <event> [value <number or expression>]" );
					continue;
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
					fail( number, "end (alone on its line)" );
					continue;
				}
				statement["kind"] = "end";
			}
			else
			{
				Dict part;
				std::string why;
				if ( ParsePart( w, part, why ) == false )
				{
					fail( number, BeginsWith( why, "\"" + verb + "\" is not one of" )
									  ? "\"" + verb +
											"\" is not a line of a motion: if, else, then when, after, timeout, cooldown, uses, lasts, until, item, owner, change, emit, end, every, find, cast, impulse, force, link, move, launch, follow, break, remove"
									  : why );
					continue;
				}
				Dict props = part["properties"].AsDict();
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
			Dict first;
			first["statements"] = List();
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
				rule.settings["conditions"] = Strings{ RestFrom( line, w[2] ) };
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
				rule.settings["conditions"] = Strings{ RestFrom( line, w[5] ) };
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
				rule.settings["when"] = kReactionOnEvent;
				rule.settings["event"] = w[1].text;
				if ( w.size() >= 3 )
				{
					if ( w[2].text != "if" || w.size() < 4 )
					{
						fail( number, "after the event: if <expression>" );
						continue;
					}
					rule.settings["conditions"] = Strings{ RestFrom( line, w[3] ) };
				}
			}
			else
			{
				if ( w.size() < 2 )
				{
					fail( number, "while what? while <expression>" );
					continue;
				}
				rule.settings["when"] = kReactionWhile;
				rule.settings["conditions"] = Strings{ RestFrom( line, w[1] ) };
			}
			continue;
		}
		if ( open == false )
		{
			errors.push_back( "line " + Num64( number ) + ": \"" + verb + "\" outside a rule: start one with on <event> or while <expression>" );
			continue;
		}
		// --- what the rest of the line says, word by word: name value pairs after the action's own
		auto options = [&]( size_t from, Dict& into, const std::vector<std::pair<const char*, const char*>>& names ) {
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
					fail( number, "\"" + name + "\" is not something " + verb + " takes" );
					return false;
				}
				const std::string key = property;
				if ( key == "bus" || key == "animation_off" || key == "scene_parent" )
				{
					into[key] = w[i + 1].text; // (words, not numbers)
				}
				else if ( Number( w[i + 1], value ) )
				{
					into[key] = value;
				}
				else
				{
					fail( number, name + " wants a number, not \"" + w[i + 1].text + "\"" );
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
				fail( number, verb + " takes one word" );
				continue;
			}
			rule.current[verb == "subject" ? "subject" : verb == "kind" ? "subject_kind" : "subject_template"] = w[1].text;
			rule.pendingSetting = number;
			continue;
		}
		if ( verb == "side" )
		{
			const std::string side = w.size() == 2 ? w[1].text : std::string();
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
				fail( number, verb + " takes a number" );
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
			std::string why;
			// (A new place: what the last one said about a node or an offset goes with it.)
			rule.current.Erase( "place_node" );
			rule.current.Erase( "offset" );
			if ( w.size() == 2 && w[1].text == "here" )
			{
				rule.current["place"] = 0; // where the sheet's node is (under its parent)
			}
			else if ( ParseAt( w, 1, w.size(), rule.current, next, why ) == false || next != w.size() )
			{
				fail( number, why.empty() ? std::string( "after where: from PATH, offset X Y Z" ) : why );
				continue;
			}
			rule.pendingSetting = number;
			continue;
		}
		// --- actions
		Rule action;
		action.line = number;
		Dict& p = action.properties;
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
				p["value"] = ValueOf( w[3] );
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
			List args;
			for ( size_t i = 3; i < w.size(); ++i )
			{
				args.push_back( ValueOf( w[i] ) );
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
			const Value color = w.size() >= 2 ? ValueOf( w[1] ) : Value();
			if ( color.type() != Value::Type::Color )
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
		Dict taken = rule.current.Duplicate();
		taken.Merge( action.properties, true );
		action.properties = taken;
		rule.pendingSetting = 0;
		rule.actions.push_back( action );
	}
	finish();
	return out;
}

} // namespace cb::sheet
