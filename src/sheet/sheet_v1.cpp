#include "sheet_v1.h"

#include "sheet_dump.h"
#include "sheet_text.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace cb::sheet
{

namespace
{

// --- Lines and their words ------------------------------------------------------------------------

std::vector<std::string> LinesOf( const std::string& text )
{
	std::vector<std::string> lines = Split( Replace( text, "\r", "" ), "\n" );
	if ( lines.empty() == false && lines.back().empty() )
	{
		lines.pop_back(); // (the newline that ends the file)
	}
	return lines;
}

// Where a line's comment starts (outside quotes), or its length.
size_t CommentAt( const std::string& line )
{
	bool quoted = false;
	for ( size_t i = 0; i < line.size(); ++i )
	{
		if ( line[i] == '"' )
		{
			quoted = quoted == false;
		}
		else if ( line[i] == '#' && quoted == false )
		{
			return i;
		}
	}
	return line.size();
}

// The line without its comment and the spaces around it.
std::string CodeOf( const std::string& line )
{
	return Strip( line.substr( 0, CommentAt( line ) ) );
}

bool Blank( const std::string& line )
{
	return CodeOf( line ).empty();
}

// How far in a line starts: a tab is four columns (as version 0 counts).
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

std::string Tabs( int depth )
{
	return std::string( size_t( depth > 0 ? depth : 0 ), '\t' );
}

// A word of a line's code: "rgb(1, 0, 0)" and "(1, 2, 3)" are one, and so is "a quoted text".
struct Word
{
	std::string text; // (a quoted word with its quotes)
	size_t start = 0;
	size_t end = 0;
};

std::vector<Word> WordsOf( const std::string& code )
{
	std::vector<Word> out;
	size_t i = 0;
	const size_t n = code.size();
	while ( i < n )
	{
		if ( code[i] == ' ' || code[i] == '\t' )
		{
			++i;
			continue;
		}
		Word word;
		word.start = i;
		if ( code[i] == '"' )
		{
			size_t close = code.find( '"', i + 1 );
			i = close == std::string::npos ? n : close + 1;
		}
		else
		{
			int depth = 0;
			while ( i < n && ( depth > 0 || ( code[i] != ' ' && code[i] != '\t' ) ) )
			{
				depth += code[i] == '(' ? 1 : code[i] == ')' ? -1 : 0;
				++i;
			}
		}
		word.end = i;
		word.text = code.substr( word.start, word.end - word.start );
		out.push_back( word );
	}
	return out;
}

// The code from a word on.
std::string From( const std::string& code, const std::vector<Word>& w, size_t k )
{
	return k < w.size() ? code.substr( w[k].start ) : std::string();
}

// --- Anchors: version 1's words for version 0's signs ---------------------------------------------

struct Anchor
{
	const char* word; // version 1
	const char* sign; // version 0
};

// (Longest sign first: ^^ before ^.)
const Anchor kAnchors[] = { { "holder", "^^" }, { "me", "^" }, { "actor", "$at" }, { "target", "$other" }, { "local", "$local" }, { "world", "$world" } };

const char* SignOf( const std::string& word )
{
	for ( const Anchor& a : kAnchors )
	{
		if ( word == a.word )
		{
			return a.sign;
		}
	}
	return nullptr;
}

// A path's anchor in version 0's sign: holder/Head is ^^/Head, local@pickup.target $local@pickup.target.
std::string PathToV0( const std::string& path )
{
	const size_t cut = path.find_first_of( "/@" );
	const std::string head = path.substr( 0, cut );
	const char* sign = SignOf( head );
	return sign != nullptr ? sign + ( cut == std::string::npos ? std::string() : path.substr( cut ) ) : path;
}

std::string PathToV1( const std::string& path )
{
	for ( const Anchor& a : kAnchors )
	{
		const std::string sign = a.sign;
		if ( path.rfind( sign, 0 ) == 0 && ( path.size() == sign.size() || path[sign.size()] == '/' || path[sign.size()] == '@' ) )
		{
			return a.word + path.substr( sign.size() );
		}
	}
	return path;
}

// --- Expressions: version 1's words for version 0's signs ------------------------------------------
//
//   known gun.ammo              ?gun.ammo
//   combat.health of target     $other:combat.health       (of me, holder, actor, target, local, world)
//   local                       is_local
//   active Slam                 motion.Slam
//
// Word for word, inside an expression, with what is quoted and what is a comment left as it is: the
// two directions are each other's undoing, so a converted sheet reads back to what it was.

struct Piece
{
	bool word = false;
	std::string text;
};

bool NameStart( char c )
{
	return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_';
}

bool NameChar( char c )
{
	return NameStart( c ) || ( c >= '0' && c <= '9' ) || c == '.';
}

std::vector<Piece> PiecesOf( const std::string& text, bool signs )
{
	std::vector<Piece> out;
	size_t i = 0;
	const size_t n = text.size();
	auto other = [&]( const std::string& s ) {
		if ( out.empty() || out.back().word )
		{
			out.push_back( Piece{ false, std::string() } );
		}
		out.back().text += s;
	};
	while ( i < n )
	{
		const char c = text[i];
		if ( c == '"' )
		{
			size_t close = text.find( '"', i + 1 );
			close = close == std::string::npos ? n : close + 1;
			other( text.substr( i, close - i ) );
			i = close;
			continue;
		}
		if ( c == '#' )
		{
			other( text.substr( i ) );
			break;
		}
		const bool starts = NameChar( c ) || ( signs && ( c == '?' || c == '$' || c == '^' ) );
		if ( starts )
		{
			size_t end = i;
			while ( end < n && ( NameChar( text[end] ) || ( signs && ( text[end] == '?' || text[end] == '$' || text[end] == '^' || text[end] == ':' ) ) ) )
			{
				++end;
			}
			out.push_back( Piece{ true, text.substr( i, end - i ) } );
			i = end;
			continue;
		}
		other( std::string( 1, c ) );
		++i;
	}
	return out;
}

bool Spaces( const Piece& p )
{
	return p.word == false && p.text.empty() == false && p.text.find_first_not_of( " \t" ) == std::string::npos;
}

// The params of the sheet being lowered (its word for a number: param gun.magazine 12), by name.
thread_local const std::map<std::string, std::string>* t_params = nullptr;
// Its item's own fields (field ... on item), which a motion's lines read and change as the item's
// ("item:gun.rounds", M287); only in a motion block.
thread_local const std::set<std::string>* t_itemFields = nullptr;
thread_local bool t_inMotion = false;

std::string ExprToV0( const std::string& text )
{
	const std::vector<Piece> p = PiecesOf( text, false );
	std::string out;
	for ( size_t i = 0; i < p.size(); ++i )
	{
		if ( p[i].word == false )
		{
			out += p[i].text;
			continue;
		}
		const std::string& w = p[i].text;
		if ( t_params != nullptr )
		{
			auto param = t_params->find( w );
			if ( param != t_params->end() )
			{
				out += param->second;
				continue;
			}
		}
		auto wordAt = [&]( size_t k ) -> const std::string* { return k < p.size() && p[k].word ? &p[k].text : nullptr; };
		auto spaceAt = [&]( size_t k ) { return k < p.size() && Spaces( p[k] ); };
		// NAME of ANCHOR
		if ( NameStart( w[0] ) && spaceAt( i + 1 ) && wordAt( i + 2 ) != nullptr && *wordAt( i + 2 ) == "of" && spaceAt( i + 3 ) && wordAt( i + 4 ) != nullptr &&
			 SignOf( *wordAt( i + 4 ) ) != nullptr )
		{
			out += std::string( SignOf( *wordAt( i + 4 ) ) ) + ":" + w;
			i += 4;
			continue;
		}
		if ( ( w == "known" || w == "active" || w == "ready" || w == "cooldown" || w == "uses" ) && spaceAt( i + 1 ) && wordAt( i + 2 ) != nullptr &&
			 NameStart( ( *wordAt( i + 2 ) )[0] ) )
		{
			// (ready, cooldown, uses: a motion's state, M287.)
			out += w == "known" ? "?" : w == "active" ? "motion." : w + ".";
			i += 1; // (the spaces: the name is read next)
			continue;
		}
		if ( t_inMotion && t_itemFields != nullptr && t_itemFields->count( w ) != 0 )
		{
			out += "item:" + w;
			continue;
		}
		const bool path = i + 1 < p.size() && p[i + 1].word == false && ( p[i + 1].text[0] == '/' || p[i + 1].text[0] == '@' );
		if ( w == "local" && path == false )
		{
			out += "is_local";
			continue;
		}
		out += w;
	}
	return out;
}

std::string ExprToV1( const std::string& text )
{
	const std::vector<Piece> p = PiecesOf( text, true );
	std::string out;
	std::function<std::string( const std::string& )> word = [&]( const std::string& w ) -> std::string {
		if ( w.size() > 1 && w[0] == '?' )
		{
			return "known " + word( w.substr( 1 ) );
		}
		for ( const Anchor& a : kAnchors )
		{
			const std::string sign = std::string( a.sign ) + ":";
			if ( w.rfind( sign, 0 ) == 0 && w.size() > sign.size() && NameStart( w[sign.size()] ) )
			{
				return w.substr( sign.size() ) + " of " + a.word;
			}
		}
		if ( w == "is_local" )
		{
			return "local";
		}
		if ( w.rfind( "motion.", 0 ) == 0 && w.size() > 7 && NameStart( w[7] ) )
		{
			return "active " + w.substr( 7 );
		}
		return w;
	};
	for ( const Piece& piece : p )
	{
		out += piece.word ? word( piece.text ) : piece.text;
	}
	return out;
}

// --- Assets: short names for the sheet's own project's files ---------------------------------------

bool EndsWithAny( const std::string& s, std::initializer_list<const char*> ends )
{
	for ( const char* e : ends )
	{
		if ( EndsWith( s, e ) )
		{
			return true;
		}
	}
	return false;
}

const char* const kSounds = "res://assets/sfx/";
const char* const kEffects = "res://vfx/";

std::string SoundToV0( const std::string& name )
{
	if ( BeginsWith( name, "res://" ) )
	{
		return name;
	}
	return kSounds + name + ( EndsWithAny( name, { ".wav", ".ogg", ".mp3" } ) ? "" : ".wav" );
}

std::string SoundToV1( const std::string& path )
{
	if ( BeginsWith( path, kSounds ) && EndsWith( path, ".wav" ) && path.size() > std::string( kSounds ).size() + 4 )
	{
		return path.substr( std::string( kSounds ).size(), path.size() - std::string( kSounds ).size() - 4 );
	}
	return path;
}

std::string EffectToV0( const std::string& name )
{
	if ( BeginsWith( name, "res://" ) )
	{
		return name;
	}
	return kEffects + name + ( EndsWithAny( name, { ".tscn", ".scn" } ) ? "" : ".tscn" );
}

std::string EffectToV1( const std::string& path )
{
	if ( BeginsWith( path, kEffects ) && EndsWith( path, ".tscn" ) && path.size() > std::string( kEffects ).size() + 5 )
	{
		return path.substr( std::string( kEffects ).size(), path.size() - std::string( kEffects ).size() - 5 );
	}
	return path;
}

// --- Lowering: version 1 to version 0 -------------------------------------------------------------

struct Out
{
	std::vector<std::pair<std::string, int>> lines;
	void Add( const std::string& text, int from )
	{
		lines.push_back( { text, from } );
	}
};

std::string Number( int n )
{
	return std::to_string( n );
}

bool IsKindWord( const std::string& word )
{
	return word == "any" || word == "player" || word == "prop" || word == "static" || word == "ragdoll" || word == "item";
}

// An action of a reaction, in version 0's words.
bool ActionToV0( const std::string& code, std::string& out, std::string& why )
{
	const std::vector<Word> w = WordsOf( code );
	const std::string verb = w[0].text;
	auto options = [&]( size_t from, std::initializer_list<std::pair<const char*, const char*>> renames, bool paths ) {
		std::string s;
		for ( size_t k = from; k < w.size(); ++k )
		{
			std::string word = w[k].text;
			for ( const auto& [v1, v0] : renames )
			{
				word = word == v1 ? std::string( v0 ) : word;
			}
			// (A path after under is one; the value after the rest is as it is.)
			if ( paths && k > from && w[k - 1].text == "under" )
			{
				word = PathToV0( word );
			}
			s += " " + word;
		}
		return s;
	};
	if ( verb == "sound" )
	{
		if ( w.size() < 2 )
		{
			why = "sound NAME [volume DB] [pitch P] [jitter J] [range M] [bus NAME] [volume_by EXPRESSION]";
			return false;
		}
		out = "sound " + SoundToV0( w[1].text ) + ( w.size() > 2 ? " " + ExprToV0( From( code, w, 2 ) ) : std::string() );
		return true;
	}
	if ( verb == "create" )
	{
		if ( w.size() < 2 )
		{
			why = "create NAME [under PATH] [for S]";
			return false;
		}
		out = "scene " + EffectToV0( w[1].text ) + options( 2, { { "for", "life" } }, true );
		return true;
	}
	if ( verb == "play" || verb == "call" || verb == "set" )
	{
		if ( w.size() < 2 )
		{
			why = verb == "play" ? "play PATH ANIMATION [off ANIMATION]" : verb == "call" ? "call PATH METHOD [ARGUMENT ...]" : "set PATH PROPERTY VALUE [blend S], or set PATH PROPERTY = EXPRESSION";
			return false;
		}
		std::string rest = From( code, w, 2 );
		if ( verb == "set" && w.size() > 4 && w[3].text == "=" )
		{
			rest = code.substr( w[2].start, w[3].end - w[2].start ) + " " + ExprToV0( From( code, w, 4 ) );
		}
		out = verb + " " + PathToV0( w[1].text ) + ( rest.empty() ? std::string() : " " + rest );
		return true;
	}
	if ( verb == "shake" || verb == "flash" )
	{
		out = verb + options( 1, { { "for", "time" } }, false );
		return true;
	}
	why = "\"" + verb + "\" is not something a reaction does: sound, create, play, set, call, shake or flash";
	return false;
}

// A scope's line in version 0's words: the setting that holds for the actions under it.
std::string ScopeToV0( const std::string& code )
{
	const std::vector<Word> w = WordsOf( code );
	const std::string verb = w[0].text;
	if ( verb == "as" )
	{
		return "subject " + ( w.size() > 1 ? PathToV0( w[1].text ) : std::string() ) + ( w.size() > 2 ? " " + From( code, w, 2 ) : std::string() );
	}
	if ( verb == "after" )
	{
		return "delay " + From( code, w, 1 );
	}
	if ( verb == "at" )
	{
		std::string s = "at";
		for ( size_t k = 1; k < w.size(); ++k )
		{
			const std::string& word = w[k].text;
			const bool place = k == 1 && word != "point" && word != "end" && word != "beam" && word != "follow";
			s += " " + ( place || ( k > 1 && w[k - 1].text == "from" ) ? PathToV0( word ) : word );
		}
		return s;
	}
	return code; // chance
}

void LowerMotion( const std::vector<std::string>& lines, size_t first, size_t end, Out& out, Strings& errors )
{
	out.Add( ExprToV0( CodeOf( lines[first] ) ), int( first + 1 ) );
	// One line of the body, in version 0's words.
	auto lower = [&]( size_t k, std::string& text ) {
		const std::string& line = lines[k];
		const std::string code = CodeOf( line );
		const std::string indent = line.substr( 0, line.find_first_not_of( " \t" ) );
		const std::vector<Word> w = WordsOf( code );
		const std::string verb = w[0].text;
		const int number = int( k + 1 );
		auto fail = [&]( const std::string& why ) {
			errors.push_back( "line " + Number( number ) + ": " + why );
			return false;
		};
		if ( verb == "for" )
		{
			if ( w.size() != 2 || ( w[1].text != "holder" && w[1].text != "carrier" ) )
			{
				return fail( "for holder (the default: whoever has the item out) or for carrier (in the hand or put away)" );
			}
			text = indent + "item " + ( w[1].text == "holder" ? "hand" : "carried" );
			return true;
		}
		if ( verb == "on" )
		{
			if ( w.size() != 2 || w[1].text != "item" )
			{
				return fail( "on item: the item runs the motion, where it lies" );
			}
			text = indent + "owner item";
			return true;
		}
		if ( verb == "then" || verb == "item" || verb == "owner" || ( verb == "after" && w.size() == 2 ) )
		{
			return fail( "\"" + verb + "\" is version 0's: wait for EXPRESSION, wait S, for holder, for carrier, on item" );
		}
		text = indent + ExprToV0( code );
		return true;
	};
	for ( size_t k = first + 1; k < end; ++k )
	{
		if ( Blank( lines[k] ) )
		{
			continue;
		}
		const std::string& line = lines[k];
		const std::string code = CodeOf( line );
		const std::vector<Word> w = WordsOf( code );
		const int number = int( k + 1 );
		if ( w[0].text != "wait" )
		{
			std::string text;
			if ( lower( k, text ) )
			{
				out.Add( text, number );
			}
			continue;
		}
		// wait S | wait for EXPRESSION [within S | or S]
		const std::string indent = line.substr( 0, line.find_first_not_of( " \t" ) );
		if ( w.size() == 2 && w[1].text != "for" )
		{
			out.Add( indent + "after " + w[1].text, number );
			continue;
		}
		if ( w.size() < 3 || w[1].text != "for" )
		{
			errors.push_back( "line " + Number( number ) + ": wait S, or wait for EXPRESSION [within S | or S]" );
			continue;
		}
		size_t last = w.size();
		std::string limit;
		std::string how;
		if ( w.size() >= 5 && ( w[w.size() - 2].text == "within" || w[w.size() - 2].text == "or" ) && IsValidFloat( w[w.size() - 1].text ) )
		{
			how = w[w.size() - 2].text;
			limit = w[w.size() - 1].text;
			last = w.size() - 2;
		}
		const std::string expression = code.substr( w[2].start, w[last - 1].end - w[2].start );
		// (wait for X or S: the step after it starts once X holds, or after S without it: M287.)
		out.Add( indent + "then when " + ExprToV0( expression ) + ( limit.empty() ? std::string() : " " + how + " " + limit ), number );
	}
}

void LowerReact( const std::vector<std::string>& lines, size_t first, size_t end, Out& out, Strings& errors )
{
	const int head = int( first + 1 );
	const std::string code = CodeOf( lines[first] );
	const std::vector<Word> w = WordsOf( code );
	auto fail = [&]( int number, const std::string& why ) { errors.push_back( "line " + Number( number ) + ": " + why ); };
	std::string event;
	std::string condition;
	const bool on = w.size() >= 3 && w[1].text == "on";
	if ( on )
	{
		event = w[2].text;
		if ( w.size() > 3 )
		{
			if ( w[3].text != "if" || w.size() < 5 )
			{
				fail( head, "react on EVENT [if EXPRESSION]" );
				return;
			}
			condition = From( code, w, 4 );
		}
	}
	else if ( w.size() >= 3 && w[1].text == "while" )
	{
		condition = From( code, w, 2 );
	}
	else
	{
		fail( head, "react on EVENT [if EXPRESSION], or react while EXPRESSION" );
		return;
	}
	// "actor is player and ...": what its subject must be, which version 0 says as a setting.
	struct Is
	{
		std::string subject;
		std::string what;
	};
	std::vector<Is> is;
	for ( ;; )
	{
		const std::vector<Word> c = WordsOf( condition );
		if ( c.size() < 3 || c[1].text != "is" )
		{
			break;
		}
		is.push_back( Is{ c[0].text, c[2].text } );
		if ( c.size() == 3 )
		{
			condition.clear();
			break;
		}
		if ( c[3].text != "and" || c.size() < 5 )
		{
			fail( head, "after " + c[0].text + " is " + c[2].text + ": and EXPRESSION" );
			return;
		}
		condition = From( condition, c, 4 );
	}
	if ( on == false && condition.empty() )
	{
		fail( head, "react while EXPRESSION" );
		return;
	}
	const std::string start = on ? "on " + event + ( condition.empty() ? std::string() : " if " + ExprToV0( condition ) ) : "while " + ExprToV0( condition );
	// The body: block settings, then scopes and actions, a scope's lines indented under it.
	struct Scope
	{
		int column = 0;
		std::string verb;
		std::string code;
		int line = 0;
		int under = 0; // lines under it
	};
	std::vector<Scope> scopes;
	std::vector<std::pair<std::string, int>> settings; // cooldown, server only
	bool begun = false;
	int actions = 0;
	auto close = [&]( int column ) {
		while ( scopes.empty() == false && scopes.back().column >= column )
		{
			if ( scopes.back().under == 0 )
			{
				fail( scopes.back().line, scopes.back().code + ": nothing is under it (indent the lines it is for)" );
			}
			scopes.pop_back();
		}
	};
	for ( size_t k = first + 1; k < end; ++k )
	{
		if ( Blank( lines[k] ) )
		{
			continue;
		}
		const int number = int( k + 1 );
		const int column = Column( lines[k] );
		const std::string line = CodeOf( lines[k] );
		const std::vector<Word> v = WordsOf( line );
		const std::string verb = v[0].text;
		close( column );
		if ( verb == "cooldown" || verb == "server" )
		{
			if ( begun || scopes.empty() == false )
			{
				fail( number, "\"" + verb + "\" is the reaction's: before its scopes and actions" );
				continue;
			}
			if ( verb == "server" && ( v.size() != 2 || v[1].text != "only" ) )
			{
				fail( number, "server only: it waits for the server's cue (a press of the viewer's is not guessed)" );
				continue;
			}
			settings.push_back( { verb == "server" ? std::string( "wait" ) : line, number } );
			continue;
		}
		begun = true;
		for ( Scope& s : scopes )
		{
			s.under += 1;
		}
		if ( verb == "as" || verb == "at" || verb == "after" || verb == "chance" )
		{
			for ( const Scope& s : scopes )
			{
				if ( s.verb == verb )
				{
					fail( number, "an " + verb + " under an " + verb + " (line " + Number( s.line ) + "): one holds at a time" );
				}
			}
			if ( v.size() < 2 )
			{
				fail( number, verb == "as" ? "as holder, actor, target, me, local, world or a path" : verb == "at" ? "at point, end, beam from PATH, follow or a path" : verb + " NUMBER" );
				continue;
			}
			scopes.push_back( Scope{ column, verb, line, number, 0 } );
			continue;
		}
		std::string action;
		std::string why;
		if ( ActionToV0( line, action, why ) == false )
		{
			fail( number, why );
			continue;
		}
		// The action as a rule of its own: its event, its settings, its scopes, it.
		std::string subject;
		for ( const Scope& s : scopes )
		{
			subject = s.verb == "as" ? WordsOf( s.code )[1].text : subject;
		}
		for ( const Is& one : is )
		{
			if ( one.subject != subject )
			{
				fail( number, "\"" + one.subject + " is " + one.what + "\" asks what its subject is: this line must be under as " + one.subject );
			}
		}
		out.Add( start, head );
		for ( const Is& one : is )
		{
			out.Add( std::string( "\t" ) + ( IsKindWord( one.what ) ? "kind " : "template " ) + one.what, head );
		}
		for ( const auto& [text, from] : settings )
		{
			out.Add( "\t" + text, from );
		}
		for ( const Scope& s : scopes )
		{
			out.Add( "\t" + ScopeToV0( s.code ), s.line );
		}
		out.Add( "\t" + action, number );
		actions += 1;
	}
	close( -1 );
	if ( actions == 0 )
	{
		fail( head, "the reaction does nothing: give it an action (sound, create, play, set, call, shake, flash)" );
	}
}

void LowerPredict( const std::vector<std::string>& lines, size_t first, size_t end, Out& out )
{
	const std::string code = CodeOf( lines[first] );
	const std::vector<Word> w = WordsOf( code );
	std::string head = code;
	if ( w.size() > 5 && w[4].text == "if" )
	{
		head = code.substr( 0, w[5].start ) + ExprToV0( From( code, w, 5 ) );
	}
	out.Add( head, int( first + 1 ) );
	for ( size_t k = first + 1; k < end; ++k )
	{
		if ( Blank( lines[k] ) == false )
		{
			out.Add( "\t" + ExprToV0( CodeOf( lines[k] ) ), int( k + 1 ) );
		}
	}
}

void LowerHud( const std::vector<std::string>& lines, size_t first, size_t end, Out& out, Strings& errors )
{
	const std::vector<Word> w = WordsOf( CodeOf( lines[first] ) );
	if ( w.size() != 2 )
	{
		errors.push_back( "line " + Number( int( first + 1 ) ) + ": hud PATH: the control its lines are about" );
		return;
	}
	const std::string path = w[1].text;
	int count = 0;
	for ( size_t k = first + 1; k < end; ++k )
	{
		if ( Blank( lines[k] ) )
		{
			continue;
		}
		const int number = int( k + 1 );
		const std::string code = CodeOf( lines[k] );
		const std::vector<Word> v = WordsOf( code );
		const std::string verb = v[0].text;
		count += 1;
		if ( verb == "show" && v.size() >= 3 && v[1].text == "if" )
		{
			out.Add( "show " + path + " if " + ExprToV0( From( code, v, 2 ) ), number );
		}
		else if ( verb == "text" || verb == "bind" )
		{
			out.Add( verb + " " + path + ( v.size() > 1 ? " " + ExprToV0( From( code, v, 1 ) ) : std::string() ), number );
		}
		else
		{
			errors.push_back( "line " + Number( number ) + ": a hud's lines are show if EXPRESSION, text \"FORMAT\" [into PROPERTY] [if EXPRESSION], bind PROPERTY = EXPRESSION [if EXPRESSION]" );
		}
	}
	if ( count == 0 )
	{
		errors.push_back( "line " + Number( int( first + 1 ) ) + ": hud " + path + ": nothing is under it" );
	}
}

// --- Units (D8): "0.2 s", "16 m/s", checked against the word they are for, and dropped ----------

enum class Unit
{
	None,
	Seconds,
	Metres,
	Speed,
	Degrees,
};

Unit UnitOf( const std::string& word )
{
	return word == "s" ? Unit::Seconds : word == "m" ? Unit::Metres : word == "m/s" ? Unit::Speed : word == "deg" ? Unit::Degrees : Unit::None;
}

const char* UnitName( Unit unit )
{
	return unit == Unit::Seconds ? "a time (s)" : unit == Unit::Metres ? "a distance (m)" : unit == Unit::Speed ? "a speed (m/s)" : "an angle (deg)";
}

// What a word's number is in: None when any unit may follow it (a movement parameter, a param).
Unit UnitFor( const std::string& word )
{
	static const char* const seconds[] = { "cooldown", "lasts", "wait", "within", "or", "every", "after", "for", "life", "time", "seconds", "ramp", "over", "delay" };
	static const char* const metres[] = { "range", "radius", "ahead", "length", "sphere", "capsule" };
	static const char* const speeds[] = { "speed", "impulse", "top", "lift", "reel" };
	for ( const char* w : seconds )
	{
		if ( word == w )
		{
			return Unit::Seconds;
		}
	}
	for ( const char* w : metres )
	{
		if ( word == w )
		{
			return Unit::Metres;
		}
	}
	for ( const char* w : speeds )
	{
		if ( word == w )
		{
			return Unit::Speed;
		}
	}
	return Unit::None;
}

// The line without its units: "cooldown 0.4 m" is a mistake (cooldown takes seconds).
std::string WithoutUnits( const std::string& line, int number, Strings& errors )
{
	const size_t hash = CommentAt( line );
	const std::string code = line.substr( 0, hash );
	const std::vector<Word> w = WordsOf( code );
	std::string out;
	size_t from = 0;
	for ( size_t k = 1; k < w.size(); ++k )
	{
		const Unit unit = UnitOf( w[k].text );
		if ( unit == Unit::None || IsValidFloat( w[k - 1].text ) == false )
		{
			continue;
		}
		// The word the number is for: the nearest before it that is not a number.
		std::string owner;
		for ( size_t j = k - 1; j-- > 0; )
		{
			if ( IsValidFloat( w[j].text ) == false )
			{
				owner = w[j].text;
				break;
			}
		}
		const Unit wanted = UnitFor( owner );
		if ( wanted != Unit::None && wanted != unit )
		{
			errors.push_back( "line " + Number( number ) + ": \"" + w[k - 1].text + " " + w[k].text + "\" is " + UnitName( unit ) + "; " + owner + " takes " +
							  UnitName( wanted ) );
		}
		out += code.substr( from, w[k - 1].end - from );
		from = w[k].end;
	}
	return out + code.substr( from ) + line.substr( hash );
}

bool IsDeclaration( const std::string& verb )
{
	return verb == "field" || verb == "event" || verb == "action" || verb == "param";
}

// --- Converting: version 0 to version 1 -----------------------------------------------------------

bool StartsBlock( const std::string& verb )
{
	return verb == "on" || verb == "while" || verb == "predict" || verb == "motion" || verb == "use" || verb == "show" || verb == "text" || verb == "bind";
}

struct Converting
{
	std::vector<std::string> out;
	std::string why;
	bool Fail( size_t index, const std::string& reason )
	{
		why = why.empty() ? "line " + Number( int( index + 1 ) ) + ": " + reason : why;
		return false;
	}
};

// A motion block's body line in version 1's words, its indentation as tabs.
std::string MotionLineToV1( const std::string& line, int headColumn )
{
	const std::string code = CodeOf( line );
	const size_t hash = CommentAt( line );
	const std::string comment = hash < line.size() ? " " + Strip( line.substr( hash ) ) : std::string();
	const std::vector<Word> w = WordsOf( code );
	const std::string indent = Tabs( ( Column( line ) - headColumn ) / 4 );
	const std::string verb = w[0].text;
	std::string text;
	if ( verb == "then" && w.size() >= 3 && w[1].text == "when" )
	{
		text = "wait for " + ExprToV1( From( code, w, 2 ) );
	}
	else if ( verb == "after" && w.size() == 2 )
	{
		text = "wait " + w[1].text;
	}
	else if ( verb == "item" && w.size() == 2 )
	{
		text = std::string( "for " ) + ( w[1].text == "hand" ? "holder" : "carrier" );
	}
	else if ( verb == "owner" && w.size() == 2 && w[1].text == "item" )
	{
		text = "on item";
	}
	else
	{
		text = ExprToV1( code );
	}
	return indent + text + comment;
}

bool ConvertMotion( const std::vector<std::string>& lines, size_t first, size_t end, Converting& c )
{
	// Line for line: a "then when X within S" with a timeout stays one ("wait for X or S" goes on
	// to the next step, which a timeout does not: M287).
	const int headColumn = Column( lines[first] );
	c.out.push_back( ExprToV1( CodeOf( lines[first] ) ) + ( CommentAt( lines[first] ) < lines[first].size() ? " " + Strip( lines[first].substr( CommentAt( lines[first] ) ) ) : std::string() ) );
	for ( size_t k = first + 1; k < end; ++k )
	{
		if ( Strip( lines[k] ).empty() )
		{
			c.out.push_back( std::string() );
		}
		else if ( Blank( lines[k] ) )
		{
			c.out.push_back( Tabs( std::max( 1, ( Column( lines[k] ) - headColumn ) / 4 ) ) + Strip( lines[k] ) );
		}
		else
		{
			c.out.push_back( MotionLineToV1( lines[k], headColumn ) );
		}
	}
	return true;
}

bool ConvertPredict( const std::vector<std::string>& lines, size_t first, size_t end, Converting& c )
{
	const std::string code = CodeOf( lines[first] );
	const std::vector<Word> w = WordsOf( code );
	std::string head = code;
	if ( w.size() > 5 && w[4].text == "if" )
	{
		head = code.substr( 0, w[5].start ) + ExprToV1( From( code, w, 5 ) );
	}
	c.out.push_back( head );
	for ( size_t k = first + 1; k < end; ++k )
	{
		c.out.push_back( Strip( lines[k] ).empty() ? std::string() : Blank( lines[k] ) ? "\t" + Strip( lines[k] ) : "\t" + ExprToV1( Strip( lines[k] ) ) );
	}
	return true;
}

// A reaction rule: each action with the settings it was given, as react blocks with scopes.
bool ConvertReaction( const std::vector<std::string>& lines, size_t first, size_t end, Converting& c )
{
	const std::string code = CodeOf( lines[first] );
	const std::vector<Word> w = WordsOf( code );
	const bool on = w[0].text == "on";
	std::string event = on && w.size() >= 2 ? w[1].text : std::string();
	std::string condition = on ? ( w.size() >= 4 && w[2].text == "if" ? From( code, w, 3 ) : std::string() ) : From( code, w, 1 );
	if ( on && w.size() >= 3 && ( w[2].text != "if" || w.size() < 4 ) )
	{
		return c.Fail( first, "a rule's first line is on EVENT [if EXPRESSION]" );
	}
	// Each setting, as the line that said it last.
	struct Action
	{
		std::map<std::string, std::string> settings;
		std::string code;
		std::vector<std::string> comments; // the comment lines above it
	};
	std::vector<Action> actions;
	std::map<std::string, std::string> current;
	std::vector<std::string> comments;
	for ( size_t k = first + 1; k < end; ++k )
	{
		if ( Blank( lines[k] ) )
		{
			if ( Strip( lines[k] ).empty() == false )
			{
				comments.push_back( Strip( lines[k] ) );
			}
			continue;
		}
		const std::string line = CodeOf( lines[k] );
		if ( CommentAt( lines[k] ) < lines[k].size() )
		{
			comments.push_back( Strip( lines[k].substr( CommentAt( lines[k] ) ) ) );
		}
		const std::vector<Word> v = WordsOf( line );
		const std::string verb = v[0].text;
		if ( verb == "subject" || verb == "kind" || verb == "template" || verb == "delay" || verb == "chance" || verb == "cooldown" )
		{
			current[verb] = From( line, v, 1 );
		}
		else if ( verb == "wait" )
		{
			if ( v.size() != 1 )
			{
				return c.Fail( k, "wait off has no version 1 spelling (leave it out: no wait is the default)" );
			}
			current["wait"] = "";
		}
		else if ( verb == "at" )
		{
			if ( v.size() == 2 && v[1].text == "here" )
			{
				return c.Fail( k, "at here has no version 1 spelling (leave it out)" );
			}
			current["at"] = From( line, v, 1 );
		}
		else if ( verb == "side" )
		{
			return c.Fail( k, "side has no version 1 spelling yet" );
		}
		else if ( verb == "sound" || verb == "scene" || verb == "play" || verb == "set" || verb == "call" || verb == "shake" || verb == "flash" )
		{
			actions.push_back( Action{ current, line, comments } );
			comments.clear();
		}
		else
		{
			return c.Fail( k, "\"" + verb + "\": not a setting or an action of a rule" );
		}
	}
	auto toV1 = []( const std::string& action ) {
		const std::vector<Word> v = WordsOf( action );
		const std::string verb = v[0].text;
		if ( v.size() < 2 )
		{
			return action; // (a mistake either way: said as it was)
		}
		auto rest = [&]( size_t from, bool times, bool paths ) {
			std::string s;
			for ( size_t k = from; k < v.size(); ++k )
			{
				std::string word = v[k].text;
				if ( times && ( word == "time" || word == "life" ) )
				{
					word = "for";
				}
				if ( paths && k > from && v[k - 1].text == "under" )
				{
					word = PathToV1( word );
				}
				s += " " + word;
			}
			return s;
		};
		if ( verb == "sound" )
		{
			return "sound " + SoundToV1( v[1].text ) + ( v.size() > 2 ? " " + ExprToV1( From( action, v, 2 ) ) : std::string() );
		}
		if ( verb == "scene" )
		{
			return "create " + EffectToV1( v[1].text ) + rest( 2, true, true );
		}
		if ( verb == "shake" || verb == "flash" )
		{
			return verb + rest( 1, true, false );
		}
		std::string tail = From( action, v, 2 );
		if ( verb == "set" && v.size() > 4 && v[3].text == "=" )
		{
			tail = action.substr( v[2].start, v[3].end - v[2].start ) + " " + ExprToV1( From( action, v, 4 ) );
		}
		return verb + " " + PathToV1( v[1].text ) + ( tail.empty() ? std::string() : " " + tail );
	};
	auto scopeToV1 = []( const std::string& key, const std::string& value ) -> std::string {
		if ( key == "subject" )
		{
			return "as " + PathToV1( value );
		}
		if ( key == "delay" )
		{
			return "after " + value;
		}
		if ( key == "at" )
		{
			const std::vector<Word> v = WordsOf( value );
			std::string s = "at";
			for ( size_t k = 0; k < v.size(); ++k )
			{
				const std::string& word = v[k].text;
				const bool place = k == 0 && word != "point" && word != "end" && word != "beam" && word != "follow";
				s += " " + ( place || ( k > 0 && v[k - 1].text == "from" ) ? PathToV1( word ) : word );
			}
			return s;
		}
		return key + " " + value; // chance
	};
	const char* const kScopes[] = { "subject", "at", "delay", "chance" };
	// Blocks: actions in a row that share what a block says (its cooldown, server only, what its
	// subject must be, and then that subject).
	auto blockOf = [&]( const Action& a ) {
		auto get = [&]( const char* key ) {
			auto it = a.settings.find( key );
			return it == a.settings.end() ? std::string( "\x01" ) : it->second;
		};
		const bool is = a.settings.count( "kind" ) || a.settings.count( "template" );
		return get( "cooldown" ) + "|" + get( "wait" ) + "|" + get( "kind" ) + "|" + get( "template" ) + "|" + ( is ? get( "subject" ) : std::string() );
	};
	for ( size_t a = 0; a < actions.size(); )
	{
		size_t b = a;
		while ( b < actions.size() && blockOf( actions[b] ) == blockOf( actions[a] ) )
		{
			++b;
		}
		const Action& one = actions[a];
		std::string head = on ? "react on " + event : "react while";
		std::string is;
		if ( one.settings.count( "kind" ) || one.settings.count( "template" ) )
		{
			auto subject = one.settings.find( "subject" );
			if ( subject == one.settings.end() )
			{
				return c.Fail( first, "a kind or template without a subject has no version 1 spelling" );
			}
			const std::string who = PathToV1( subject->second );
			for ( const char* key : { "kind", "template" } )
			{
				if ( one.settings.count( key ) )
				{
					is += ( is.empty() ? "" : " and " ) + who + " is " + one.settings.at( key );
				}
			}
			const std::string plain = " " + condition + " ";
			if ( plain.find( " or " ) != std::string::npos || condition.find( "||" ) != std::string::npos )
			{
				return c.Fail( first, "a kind with an \"or\" in the rule's condition has no version 1 spelling" );
			}
		}
		const std::string cond = ExprToV1( condition );
		if ( on )
		{
			const std::string all = is.empty() ? cond : cond.empty() ? is : is + " and " + cond;
			head += all.empty() ? std::string() : " if " + all;
		}
		else
		{
			head += " " + ( is.empty() ? cond : is + " and " + cond );
		}
		c.out.push_back( head );
		if ( one.settings.count( "cooldown" ) )
		{
			c.out.push_back( "\tcooldown " + one.settings.at( "cooldown" ) );
		}
		if ( one.settings.count( "wait" ) )
		{
			c.out.push_back( "\tserver only" );
		}
		// Scopes: the open ones a prefix of what each action needs, in one order.
		std::vector<std::string> open;
		for ( size_t k = a; k < b; ++k )
		{
			std::vector<std::string> need;
			for ( const char* key : kScopes )
			{
				auto it = actions[k].settings.find( key );
				if ( it != actions[k].settings.end() )
				{
					need.push_back( scopeToV1( key, it->second ) );
				}
			}
			size_t keep = 0;
			while ( keep < open.size() && keep < need.size() && open[keep] == need[keep] )
			{
				++keep;
			}
			open.resize( keep );
			for ( const std::string& comment : actions[k].comments )
			{
				c.out.push_back( Tabs( int( keep ) + 1 ) + comment );
			}
			for ( size_t s = keep; s < need.size(); ++s )
			{
				c.out.push_back( Tabs( int( s ) + 1 ) + need[s] );
				open.push_back( need[s] );
			}
			c.out.push_back( Tabs( int( open.size() ) + 1 ) + toV1( actions[k].code ) );
		}
		if ( b < actions.size() )
		{
			c.out.push_back( std::string() );
		}
		a = b;
	}
	for ( const std::string& comment : comments )
	{
		c.out.push_back( "\t" + comment );
	}
	return true;
}

// HUD lines in a row, about the same control (comments between them too): one hud block. Where
// the next line after them is.
size_t ConvertHud( const std::vector<std::string>& lines, size_t first, Converting& c )
{
	const std::vector<Word> w = WordsOf( CodeOf( lines[first] ) );
	const std::string path = w.size() > 1 ? w[1].text : std::string();
	c.out.push_back( "hud " + path );
	size_t next = first;
	std::vector<std::string> pending;
	for ( size_t k = first; k < lines.size(); ++k )
	{
		if ( Blank( lines[k] ) )
		{
			if ( Strip( lines[k] ).empty() == false )
			{
				pending.push_back( "\t" + Strip( lines[k] ) );
			}
			continue;
		}
		const std::string code = CodeOf( lines[k] );
		const std::vector<Word> v = WordsOf( code );
		if ( ( v[0].text != "show" && v[0].text != "text" && v[0].text != "bind" ) || v.size() < 2 || v[1].text != path )
		{
			break;
		}
		c.out.insert( c.out.end(), pending.begin(), pending.end() );
		pending.clear();
		const std::string comment = CommentAt( lines[k] ) < lines[k].size() ? " " + Strip( lines[k].substr( CommentAt( lines[k] ) ) ) : std::string();
		c.out.push_back( "\t" + v[0].text + ( v.size() > 2 ? " " + ExprToV1( From( code, v, 2 ) ) : std::string() ) + comment );
		next = k + 1;
	}
	return next;
}

void RemapLines( Value& value, const std::vector<int>& lineOf, std::set<const void*>& seen )
{
	auto map = [&]( int64_t n ) { return n >= 1 && size_t( n ) <= lineOf.size() ? int64_t( lineOf[size_t( n - 1 )] ) : n; };
	if ( value.type() == Value::Type::List )
	{
		List list = value.AsList();
		if ( list.empty() || seen.insert( &list[0] ).second == false )
		{
			return;
		}
		for ( size_t i = 0; i < list.size(); ++i )
		{
			RemapLines( list[i], lineOf, seen );
		}
	}
	else if ( value.type() == Value::Type::Dict )
	{
		Dict dict = value.AsDict();
		if ( seen.insert( &dict.Entries() ).second == false )
		{
			return;
		}
		for ( const std::string& key : dict.Keys() )
		{
			Value& one = dict[key];
			if ( key == "line" && one.type() == Value::Type::Int )
			{
				one = Value( map( one.AsInt() ) );
			}
			else
			{
				RemapLines( one, lineOf, seen );
			}
		}
	}
}

} // namespace

// The version 0 parser (sheet_rules.cpp).
std::vector<Rule> ParseVersion0( const std::string& text, Strings& errors );

int SheetVersion( const std::string& text )
{
	for ( const std::string& line : LinesOf( text ) )
	{
		if ( Blank( line ) )
		{
			continue;
		}
		const std::vector<Word> w = WordsOf( CodeOf( line ) );
		if ( w[0].text != "cbsheet" )
		{
			return 0;
		}
		return w.size() == 2 && w[1].text == "1" ? 1 : -1;
	}
	return 0;
}

Lowered LowerVersion1( const std::string& text, Strings& errors )
{
	std::vector<std::string> lines = LinesOf( text );
	for ( size_t k = 0; k < lines.size(); ++k )
	{
		lines[k] = WithoutUnits( lines[k], int( k + 1 ), errors );
	}
	// The params, as the numbers they stand for in this sheet's expressions.
	Strings ignored;
	std::map<std::string, std::string> params;
	for ( const Declaration& d : ReadDeclarations( text, ignored ) )
	{
		if ( d.kind == Declaration::Kind::Param )
		{
			params[d.name] = Num( d.value );
		}
	}
	std::set<std::string> itemFields;
	for ( const Declaration& d : ReadDeclarations( text, ignored ) )
	{
		if ( d.kind == Declaration::Kind::Field && d.on == "item" )
		{
			itemFields.insert( d.name );
		}
	}
	struct Using
	{
		Using( const std::map<std::string, std::string>* p, const std::set<std::string>* f )
		{
			t_params = p;
			t_itemFields = f;
		}
		~Using()
		{
			t_params = nullptr;
			t_itemFields = nullptr;
		}
	} using_( &params, &itemFields );
	Out out;
	bool header = false;
	size_t i = 0;
	while ( i < lines.size() )
	{
		if ( Blank( lines[i] ) )
		{
			++i;
			continue;
		}
		const int number = int( i + 1 );
		const std::vector<Word> w = WordsOf( CodeOf( lines[i] ) );
		const std::string verb = w[0].text;
		size_t end = i + 1;
		while ( end < lines.size() && ( Blank( lines[end] ) || Column( lines[end] ) > 0 ) )
		{
			++end;
		}
		if ( header == false )
		{
			header = true;
			if ( verb != "cbsheet" )
			{
				errors.push_back( "line " + Number( number ) + ": a sheet's first line says its version: cbsheet 1" );
			}
			i = end;
			continue;
		}
		if ( Column( lines[i] ) > 0 )
		{
			errors.push_back( "line " + Number( number ) + ": indented, but under no block (a block starts at the line's start)" );
		}
		else if ( verb == "motion" || verb == "use" )
		{
			t_inMotion = true;
			LowerMotion( lines, i, end, out, errors );
			t_inMotion = false;
		}
		else if ( verb == "react" )
		{
			LowerReact( lines, i, end, out, errors );
		}
		else if ( verb == "predict" )
		{
			LowerPredict( lines, i, end, out );
		}
		else if ( verb == "hud" )
		{
			LowerHud( lines, i, end, out, errors );
		}
		else if ( verb == "cbsheet" )
		{
			errors.push_back( "line " + Number( number ) + ": the version is the first line only" );
		}
		else if ( IsDeclaration( verb ) )
		{
			// (ReadDeclarations reads it; ParseRules says its mistakes.)
		}
		else
		{
			errors.push_back( "line " + Number( number ) + ": \"" + verb + "\" starts no block: a sheet's blocks are motion, use, react, predict and hud" +
							  ( verb == "on" || verb == "while" ? std::string( " (a reaction is react " ) + verb + " ...)" : std::string() ) );
		}
		i = end;
	}
	Lowered lowered;
	for ( const auto& [line, from] : out.lines )
	{
		lowered.text += line + "\n";
		lowered.lineOf.push_back( from );
	}
	return lowered;
}

std::vector<Rule> ParseRules( const std::string& text, Strings& errors )
{
	const int version = SheetVersion( text );
	if ( version == 0 )
	{
		return ParseVersion0( text, errors );
	}
	if ( version < 0 )
	{
		errors.push_back( "line 1: this game reads sheets of version 1 (cbsheet 1) and before" );
		return {};
	}
	ReadDeclarations( text, errors );
	const Lowered lowered = LowerVersion1( text, errors );
	Strings found;
	std::vector<Rule> rules = ParseVersion0( lowered.text, found );
	auto map = [&]( int n ) { return n >= 1 && size_t( n ) <= lowered.lineOf.size() ? lowered.lineOf[size_t( n - 1 )] : n; };
	for ( const std::string& error : found )
	{
		// ("line N: why", N the lowered text's)
		const size_t colon = error.find( ": " );
		if ( error.rfind( "line ", 0 ) == 0 && colon != std::string::npos && IsValidInt( error.substr( 5, colon - 5 ) ) )
		{
			errors.push_back( "line " + Number( map( int( ToInt( error.substr( 5, colon - 5 ) ) ) ) ) + error.substr( colon ) );
		}
		else
		{
			errors.push_back( error );
		}
	}
	std::set<const void*> seen;
	for ( Rule& rule : rules )
	{
		rule.line = map( rule.line );
		rule.head = rule.head == 0 ? 0 : map( rule.head );
		Value properties( rule.properties );
		RemapLines( properties, lowered.lineOf, seen );
	}
	return rules;
}

std::vector<Declaration> ReadDeclarations( const std::string& text, Strings& errors )
{
	std::vector<Declaration> out;
	if ( SheetVersion( text ) != 1 )
	{
		return out;
	}
	const std::vector<std::string> lines = LinesOf( text );
	std::map<std::string, size_t> seen; // name -> its declaration
	for ( size_t k = 0; k < lines.size(); ++k )
	{
		if ( Blank( lines[k] ) || Column( lines[k] ) > 0 )
		{
			continue;
		}
		const int number = int( k + 1 );
		Strings unitErrors;
		const std::string code = CodeOf( WithoutUnits( lines[k], number, unitErrors ) );
		const std::vector<Word> w = WordsOf( code );
		const std::string verb = w[0].text;
		if ( IsDeclaration( verb ) == false )
		{
			continue;
		}
		auto fail = [&]( const std::string& why ) { errors.push_back( "line " + Number( number ) + ": " + why ); };
		auto nameOk = [&]( const std::string& name ) {
			if ( name.empty() || name.size() > 48 || name.find_first_not_of( "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_." ) != std::string::npos ||
				 NameStart( name[0] ) == false )
			{
				return false;
			}
			return true;
		};
		Declaration d;
		d.line = number;
		bool ok = w.size() >= 2 && nameOk( w[1].text );
		if ( ok )
		{
			d.name = w[1].text;
		}
		if ( verb == "field" )
		{
			d.kind = Declaration::Kind::Field;
			ok = ok && w.size() >= 3 && ( w[2].text == "int" || w[2].text == "float" || w[2].text == "bool" || w[2].text == "timer" );
			if ( ok )
			{
				d.type = w[2].text;
			}
			for ( size_t i = 3; ok && i < w.size(); )
			{
				const std::string& word = w[i].text;
				if ( word == "on" && i + 1 < w.size() && ( w[i + 1].text == "player" || w[i + 1].text == "world" || w[i + 1].text == "item" ) )
				{
					d.on = w[i + 1].text;
					i += 2;
				}
				else if ( word == "shared" || word == "private" )
				{
					( word == "shared" ? d.shared : d.privately ) = true;
					i += 1;
				}
				else if ( word == "=" && i + 1 < w.size() )
				{
					const std::string& v = w[i + 1].text;
					ok = v == "true" || v == "false" || IsValidFloat( v );
					d.value = v == "true" ? 1.0 : v == "false" ? 0.0 : ok ? ToFloat( v ) : 0.0;
					i += 2;
				}
				else
				{
					ok = false;
				}
			}
			ok = ok && ( d.shared && d.privately ) == false && ( d.on == "player" || ( d.on == "item" && d.privately == false ) || ( d.shared == false && d.privately == false ) );
			if ( ok == false )
			{
				fail( "field NAME int|float|bool|timer [on player|world|item] [shared|private] [= VALUE] (shared and private for a player's field)" );
				continue;
			}
			if ( d.type == "timer" && d.privately )
			{
				fail( "a timer is a player's field, an item's or the world's (a private one: not yet)" );
				continue;
			}
		}
		else if ( verb == "event" )
		{
			d.kind = Declaration::Kind::Event;
			d.everyone = w.size() == 3 && w[2].text == "everyone";
			if ( ok == false || ( w.size() != 2 && d.everyone == false ) )
			{
				fail( "event NAME [everyone]" );
				continue;
			}
		}
		else if ( verb == "action" )
		{
			d.kind = Declaration::Kind::Action;
			if ( w.size() == 4 && w[2].text == "key" )
			{
				d.key = w[3].text;
			}
			if ( ok == false || ( w.size() != 2 && d.key.empty() ) )
			{
				fail( "action NAME [key KEY] (as Godot names keys: R, F, 1, MouseLeft)" );
				continue;
			}
		}
		else
		{
			d.kind = Declaration::Kind::Param;
			if ( ok == false || w.size() != 3 || IsValidFloat( w[2].text ) == false )
			{
				fail( "param NAME NUMBER [UNIT]: a number of the item (gun.damage 25)" );
				continue;
			}
			d.value = ToFloat( w[2].text );
		}
		auto before = seen.find( d.name );
		if ( before != seen.end() )
		{
			const Declaration& was = out[before->second];
			if ( was.kind != d.kind || was.type != d.type || was.on != d.on || d.kind == Declaration::Kind::Param )
			{
				fail( d.name + " is declared at line " + Number( was.line ) + " already" );
			}
			continue;
		}
		seen[d.name] = out.size();
		out.push_back( d );
	}
	return out;
}

std::string Meaning( const std::string& text )
{
	Strings errors;
	const std::vector<Rule> rules = ParseRules( text, errors );
	return DumpMeaning( rules, errors );
}

bool ConvertToVersion1( const std::string& version0, std::string& version1, std::string& why )
{
	if ( SheetVersion( version0 ) != 0 )
	{
		why = "it is not a version 0 sheet";
		return false;
	}
	const std::vector<std::string> lines = LinesOf( version0 );
	Converting c;
	c.out.push_back( "cbsheet 1" );
	c.out.push_back( std::string() );
	size_t i = 0;
	while ( i < lines.size() )
	{
		if ( Blank( lines[i] ) )
		{
			c.out.push_back( Strip( lines[i] ) );
			++i;
			continue;
		}
		const std::vector<Word> w = WordsOf( CodeOf( lines[i] ) );
		const std::string verb = w[0].text;
		if ( StartsBlock( verb ) == false )
		{
			c.Fail( i, "\"" + verb + "\" outside a rule" );
			why = c.why;
			return false;
		}
		// The block: up to the next line that starts one; the comments and blank lines at its end are
		// the next one's.
		size_t end = i + 1;
		while ( end < lines.size() && ( Blank( lines[end] ) || StartsBlock( WordsOf( CodeOf( lines[end] ) )[0].text ) == false ) )
		{
			++end;
		}
		size_t last = end;
		while ( last > i + 1 && Blank( lines[last - 1] ) )
		{
			--last;
		}
		bool ok = true;
		if ( verb == "motion" || verb == "use" )
		{
			ok = ConvertMotion( lines, i, last, c );
		}
		else if ( verb == "predict" )
		{
			ok = ConvertPredict( lines, i, last, c );
		}
		else if ( verb == "on" || verb == "while" )
		{
			ok = ConvertReaction( lines, i, last, c );
		}
		else
		{
			last = ConvertHud( lines, i, c );
		}
		if ( ok == false )
		{
			why = c.why;
			return false;
		}
		i = last;
	}
	while ( c.out.size() > 1 && c.out.back().empty() )
	{
		c.out.pop_back();
	}
	version1.clear();
	for ( const std::string& line : c.out )
	{
		version1 += line + "\n";
	}
	// It means what it meant, or it is not written.
	const std::string before = Meaning( version0 );
	const std::string after = Meaning( version1 );
	if ( before != after )
	{
		size_t at = 0;
		while ( at < before.size() && at < after.size() && before[at] == after[at] )
		{
			++at;
		}
		const size_t from = before.rfind( '\n', at ) == std::string::npos ? 0 : before.rfind( '\n', at ) + 1;
		why = "the converted sheet would mean something else, from: " + before.substr( from, 160 ) + " / now: " + after.substr( from, 160 );
		return false;
	}
	return true;
}

bool FormatVersion1( const std::string& text, std::string& formatted, std::string& why )
{
	if ( SheetVersion( text ) != 1 )
	{
		why = "it is not a version 1 sheet (cbsheet 1): convert it first";
		return false;
	}
	formatted.clear();
	bool blank = false;
	bool any = false;
	for ( const std::string& line : LinesOf( text ) )
	{
		const std::string body = Strip( line );
		if ( body.empty() )
		{
			blank = any;
			continue;
		}
		if ( blank )
		{
			formatted += "\n";
		}
		blank = false;
		any = true;
		formatted += Tabs( ( Column( line ) + 3 ) / 4 ) + body + "\n";
	}
	if ( Meaning( formatted ) != Meaning( text ) )
	{
		why = "formatted, it would mean something else (its indentation is not in fours?)";
		return false;
	}
	return true;
}

} // namespace cb::sheet
