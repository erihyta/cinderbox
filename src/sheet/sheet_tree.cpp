#include "sheet_tree.h"

#include "sheet_text.h"

#include <algorithm>
#include <functional>

namespace cb::sheet
{

namespace
{

std::string Number( int n )
{
	return std::to_string( n );
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

bool Blank( const std::string& line )
{
	return CodeOf( line ).empty();
}

bool NameStart( char c )
{
	return ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || c == '_';
}

bool NameChar( char c )
{
	return NameStart( c ) || ( c >= '0' && c <= '9' ) || c == '.';
}

// --- Anchors: the words for whose a path or a field is, and the engine's signs for them ------------

struct Anchor
{
	const char* word;
	const char* sign;
};

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

// --- An expression's pieces: names (with their dots), and what is between them ------------------

struct Piece
{
	bool word = false;
	std::string text;
};

std::vector<Piece> PiecesOf( const std::string& text )
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
		if ( NameChar( c ) )
		{
			size_t end = i;
			while ( end < n && NameChar( text[end] ) )
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
std::string WithoutUnits( const std::string& line, const std::function<void( const std::string& )>& fail )
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
			fail( "\"" + w[k - 1].text + " " + w[k].text + "\" is " + UnitName( unit ) + "; " + owner + " takes " + UnitName( wanted ) );
		}
		out += code.substr( from, w[k - 1].end - from );
		from = w[k].end;
	}
	return out + code.substr( from ) + line.substr( hash );
}

// --- Lets (M289): a name for some words ------------------------------------------------------------

// The words of the language: a let cannot take one as its name (it would change what the word does
// wherever it stands: "let up = 5" made "along up" a marker named 5).
bool IsLanguageWord( const std::string& word )
{
	static const std::set<std::string> words = {
		// blocks, declarations, settings
		"cbsheet", "uses", "replace", "let", "field", "event", "action", "param", "status", "motion", "use", "react", "predict", "hud", "on", "while", "when",
		"if", "else", "wait", "for", "within", "or", "timeout", "every", "end", "as", "at", "after", "chance", "cooldown", "server", "only", "refill",
		"ground", "lasts", "until", "tags", "blocked", "by", "cancels", "holder", "carrier", "item", "shared", "private", "everyone", "key", "int", "float",
		"bool", "timer",
		// lines
		"change", "emit", "value", "impulse", "force", "throw", "launch", "break", "find", "grab", "cast", "apply", "move", "link", "follow", "remove",
		"self", "sound", "create", "play", "set", "call", "shake", "flash", "show", "text", "bind", "into", "held", "stance", "press", "release", "tap",
		"hold",
		// options
		"along", "toward", "kind", "top", "ramp", "replace", "turn", "direction", "marks", "new", "carried", "speed", "lift", "ahead", "seconds", "from",
		"radius", "players", "bodies", "max", "turned", "facing", "look", "none", "sight", "in", "first", "range", "holds", "level", "length", "reel",
		"alone", "over", "offset", "beam", "point", "under", "off", "blend", "volume", "pitch", "jitter", "bus", "volume_by", "owner", "target", "found",
		"hit", "up", "to", "away", "world", "nothing", "vertical", "horizontal", "all", "velocity", "momentum", "acceleration", "sphere", "box", "capsule",
		"ray", "me", "actor", "local", "attached", "steer", "strength", "down", "per", "second", "tag",
		// expressions
		"not", "and", "is", "known", "active", "ready", "remaining", "of", "first_person", "grounded", "linked", "distance", "reach", "time", "player",
		"prop", "true", "false" };
	return words.count( word ) != 0;
}

void ApplyLets( std::vector<std::pair<std::string, int>>& lines, const Source& source, Strings& errors )
{
	std::map<std::string, std::string> lets;
	for ( auto& [line, number] : lines )
	{
		if ( Column( line ) != 0 || Blank( line ) )
		{
			continue;
		}
		const std::string code = CodeOf( line );
		const std::vector<Word> w = WordsOf( code );
		if ( w[0].text != "let" )
		{
			continue;
		}
		if ( w.size() < 4 || w[2].text != "=" || NameStart( w[1].text[0] ) == false )
		{
			errors.push_back( source.Mistake( number, "let NAME = WORDS" ) );
		}
		else if ( IsLanguageWord( w[1].text ) )
		{
			errors.push_back( source.Mistake( number, "\"" + w[1].text + "\" is a word of the language: a let needs a name of its own" ) );
		}
		else
		{
			lets[w[1].text] = From( code, w, 3 );
		}
		line.clear();
	}
	if ( lets.empty() )
	{
		return;
	}
	for ( auto& [line, number] : lines )
	{
		const size_t hash = CommentAt( line );
		std::string out;
		for ( const Piece& p : PiecesOf( line.substr( 0, hash ) ) )
		{
			auto it = p.word ? lets.find( p.text ) : lets.end();
			out += it != lets.end() ? it->second : p.text;
		}
		line = out + line.substr( hash );
	}
}

// --- Composing (M289): a sheet's own parts in place of the used sheet's ----------------------------

// A sheet's top-level parts: each a line at the line's start and the lines under it (blank lines and
// comments among them), with its key: a declaration's kind and name, a block's head.
struct Part
{
	std::string key;
	bool replaces = false; // "replace BLOCK"
	size_t first = 0, end = 0;
};

std::vector<Part> PartsOf( const std::vector<std::string>& lines )
{
	std::vector<Part> out;
	for ( size_t k = 0; k < lines.size(); ++k )
	{
		if ( Blank( lines[k] ) || Column( lines[k] ) != 0 )
		{
			if ( out.empty() == false )
			{
				out.back().end = k + 1;
			}
			continue;
		}
		std::string code = CodeOf( lines[k] );
		Part u;
		u.first = k;
		u.end = k + 1;
		if ( code.rfind( "replace ", 0 ) == 0 )
		{
			u.replaces = true;
			code = Strip( code.substr( 8 ) );
		}
		const std::vector<Word> w = WordsOf( code );
		const std::string verb = w[0].text;
		if ( ( IsDeclaration( verb ) || verb == "let" || verb == "motion" ) && w.size() >= 2 )
		{
			u.key = verb + " " + w[1].text;
		}
		else
		{
			// (A block's whole first line, its spaces made one.)
			for ( const Word& one : w )
			{
				u.key += ( u.key.empty() ? "" : " " ) + one.text;
			}
		}
		out.push_back( u );
	}
	return out;
}

// The tree of one block's lines under its first: each line under the nearest line before it that
// starts less far in.
Node BlockOf( const std::vector<Line>& lines, size_t first, size_t end )
{
	Node block;
	block.line = lines[first];
	std::vector<Node*> open = { &block };
	for ( size_t k = first + 1; k < end; ++k )
	{
		while ( open.size() > 1 && open.back()->line.column >= lines[k].column )
		{
			open.pop_back();
		}
		Node one;
		one.line = lines[k];
		open.back()->under.push_back( one );
		open.push_back( &open.back()->under.back() );
	}
	return block;
}

} // namespace

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

std::string From( const std::string& code, const std::vector<Word>& w, size_t k )
{
	return k < w.size() ? code.substr( w[k].start ) : std::string();
}

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

std::string CodeOf( const std::string& line )
{
	return Strip( line.substr( 0, CommentAt( line ) ) );
}

std::vector<std::string> LinesOf( const std::string& text )
{
	std::vector<std::string> lines = Split( Replace( text, "\r", "" ), "\n" );
	if ( lines.empty() == false && lines.back().empty() )
	{
		lines.pop_back(); // (the newline that ends the file)
	}
	return lines;
}

bool IsDeclaration( const std::string& verb )
{
	return verb == "field" || verb == "event" || verb == "action" || verb == "param" || verb == "status" || verb == "tag";
}

std::string Source::Mistake( int line, const std::string& why ) const
{
	return line >= kFromUsed ? "line " + Number( usesLine ) + ": in " + used + " line " + Number( line - kFromUsed ) + ": " + why : "line " + Number( line ) + ": " + why;
}

Source Compose( const std::string& text, Strings& errors )
{
	Source out;
	const std::vector<std::string> lines = LinesOf( text );
	auto plain = [&]() {
		out.lines.clear();
		for ( size_t k = 0; k < lines.size(); ++k )
		{
			out.lines.push_back( { lines[k], int( k + 1 ) } );
		}
		return out;
	};
	// Its uses line: the first block after the version, "uses PATH".
	const std::vector<Part> own = PartsOf( lines );
	const Part* uses = nullptr;
	for ( const Part& u : own )
	{
		uses = u.key.rfind( "uses ", 0 ) == 0 ? &u : uses;
	}
	if ( uses == nullptr )
	{
		for ( const Part& u : own )
		{
			if ( u.replaces )
			{
				errors.push_back( "line " + Number( int( u.first + 1 ) ) + ": replace is for a sheet that uses another" );
			}
		}
		return plain();
	}
	out.used = Strip( uses->key.substr( 5 ) );
	out.usesLine = int( uses->first + 1 );
	std::string usedText;
	if ( TheSheetLoader() == nullptr || TheSheetLoader()( out.used, usedText ) == false )
	{
		errors.push_back( "line " + Number( out.usesLine ) + ": uses " + out.used + ": no such sheet (a res:// path)" );
		return plain();
	}
	if ( SheetVersion( usedText ) != 1 )
	{
		errors.push_back( "line " + Number( out.usesLine ) + ": uses " + out.used + ": a version 1 sheet (cbsheet 1)" );
		return plain();
	}
	const std::vector<std::string> theirs = LinesOf( usedText );
	const std::vector<Part> base = PartsOf( theirs );
	// This sheet's, by key: what takes the place of the used sheet's.
	std::map<std::string, const Part*> mine;
	for ( const Part& u : own )
	{
		if ( &u != uses && u.key.rfind( "cbsheet", 0 ) != 0 )
		{
			mine[u.key] = &u;
		}
	}
	std::set<std::string> taken;
	auto add = [&]( const std::vector<std::string>& from, size_t first, size_t end, int offset, bool dropReplace ) {
		for ( size_t k = first; k < end; ++k )
		{
			std::string line = from[k];
			if ( dropReplace && k == first && line.rfind( "replace ", 0 ) == 0 )
			{
				line = line.substr( 8 );
			}
			out.lines.push_back( { line, offset + int( k + 1 ) } );
		}
	};
	out.lines.push_back( { "cbsheet 1", 1 } );
	for ( const Part& u : base )
	{
		if ( u.key.rfind( "cbsheet", 0 ) == 0 )
		{
			continue;
		}
		if ( u.key.rfind( "uses ", 0 ) == 0 )
		{
			errors.push_back( "line " + Number( out.usesLine ) + ": uses " + out.used + ", which uses another: a sheet uses one at most, which uses none" );
			continue;
		}
		auto it = mine.find( u.key );
		if ( it == mine.end() )
		{
			add( theirs, u.first, u.end, kFromUsed, false );
			continue;
		}
		const bool block = IsDeclaration( WordsOf( u.key )[0].text ) == false && u.key.rfind( "let ", 0 ) != 0;
		if ( block && it->second->replaces == false )
		{
			errors.push_back( "line " + Number( int( it->second->first + 1 ) ) + ": " + out.used + " has this block (line " + Number( int( u.first + 1 ) ) +
							  "): write replace before it" );
		}
		add( lines, it->second->first, it->second->end, 0, true );
		taken.insert( u.key );
	}
	for ( const Part& u : own )
	{
		if ( &u == uses || u.key.rfind( "cbsheet", 0 ) == 0 || taken.count( u.key ) != 0 )
		{
			continue;
		}
		if ( u.replaces )
		{
			errors.push_back( "line " + Number( int( u.first + 1 ) ) + ": replace: " + out.used + " has no " + u.key );
			continue;
		}
		add( lines, u.first, u.end, 0, false );
	}
	return out;
}

std::vector<Declaration> DeclarationsOf( const Source& source, Strings& errors )
{
	std::vector<Declaration> out;
	std::map<std::string, size_t> seen; // name -> its declaration
	// (M296) The params first: a field may start at one ("field gun.rounds int on item = gun.capacity").
	std::map<std::string, double> params;
	for ( const auto& [line, number] : source.lines )
	{
		const std::vector<Word> w = WordsOf( CodeOf( WithoutUnits( line, []( const std::string& ) {} ) ) );
		if ( Column( line ) == 0 && w.size() == 3 && w[0].text == "param" && IsValidFloat( w[2].text ) )
		{
			params[w[1].text] = ToFloat( w[2].text );
		}
	}
	for ( const auto& [line, number] : source.lines )
	{
		if ( Blank( line ) || Column( line ) > 0 )
		{
			continue;
		}
		const std::string code = CodeOf( WithoutUnits( line, []( const std::string& ) {} ) );
		const std::vector<Word> w = WordsOf( code );
		const std::string verb = w[0].text;
		if ( IsDeclaration( verb ) == false )
		{
			continue;
		}
		auto fail = [&]( const std::string& why ) { errors.push_back( source.Mistake( number, why ) ); };
		auto nameOk = [&]( const std::string& name ) {
			return name.empty() == false && name.size() <= 48 && name.find_first_not_of( "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_." ) == std::string::npos &&
				   NameStart( name[0] );
		};
		Declaration d;
		d.line = number >= kFromUsed ? source.usesLine : number;
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
					const auto param = params.find( v ); // (M296: a param's number)
					ok = v == "true" || v == "false" || IsValidFloat( v ) || param != params.end();
					d.value = v == "true" ? 1.0 : v == "false" ? 0.0 : param != params.end() ? param->second : ok ? ToFloat( v ) : 0.0;
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
				fail( "field NAME int|float|bool|timer [on player|world|item] [shared|private] [= VALUE or a param] (shared and private for a player's field)" );
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
		else if ( verb == "status" )
		{
			d.kind = Declaration::Kind::Status;
			if ( ok == false || w.size() != 2 )
			{
				fail( "status NAME" );
				continue;
			}
		}
		else if ( verb == "tag" )
		{
			// (M296) What a motion's tags, blocked by and cancels may name, besides its sheet's motions and
			// statuses: a tag carried in another sheet is declared in each that names it.
			d.kind = Declaration::Kind::Tag;
			if ( ok == false || w.size() != 2 )
			{
				fail( "tag NAME" );
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

std::string Node::Verb() const
{
	const std::vector<Word> w = WordsOf( line.code );
	return w.empty() ? std::string() : w[0].text;
}

Tree ReadTree( const std::string& text, Strings& errors )
{
	Tree tree;
	tree.source = Compose( text, errors );
	const Source& source = tree.source;
	for ( const Declaration& d : DeclarationsOf( source, errors ) )
	{
		if ( d.kind == Declaration::Kind::Param )
		{
			tree.params[d.name] = Num( d.value );
		}
		if ( d.kind == Declaration::Kind::Field && d.on == "item" )
		{
			tree.itemFields.insert( d.name );
		}
		if ( d.kind == Declaration::Kind::Tag )
		{
			tree.tags.insert( d.name );
		}
		if ( d.kind == Declaration::Kind::Status )
		{
			tree.statuses.insert( d.name );
		}
	}
	std::vector<std::pair<std::string, int>> lines = source.lines;
	ApplyLets( lines, source, errors );
	std::vector<Line> read;
	for ( auto& [raw, number] : lines )
	{
		const int n = number;
		const std::string line = WithoutUnits( raw, [&]( const std::string& why ) { errors.push_back( source.Mistake( n, why ) ); } );
		if ( Blank( line ) == false )
		{
			read.push_back( Line{ number, Column( line ), CodeOf( line ) } );
		}
	}
	// The version, then blocks: a line at the line's start and the lines further in after it.
	bool header = false;
	size_t i = 0;
	while ( i < read.size() )
	{
		size_t end = i + 1;
		while ( end < read.size() && read[end].column > 0 )
		{
			++end;
		}
		const Line& head = read[i];
		const std::string verb = WordsOf( head.code )[0].text;
		if ( header == false )
		{
			header = true;
			if ( verb != "cbsheet" )
			{
				errors.push_back( source.Mistake( head.number, "a sheet's first line says its version: cbsheet 1" ) );
			}
			i = end;
			continue;
		}
		if ( head.column > 0 )
		{
			errors.push_back( source.Mistake( head.number, "indented, but under no block (a block starts at the line's start)" ) );
		}
		else if ( verb == "motion" || verb == "use" || verb == "react" || verb == "predict" || verb == "hud" )
		{
			tree.blocks.push_back( BlockOf( read, i, end ) );
			const std::vector<Word> w = WordsOf( head.code );
			if ( verb == "motion" && w.size() >= 2 )
			{
				tree.motions.insert( w[1].text );
			}
		}
		else if ( verb == "cbsheet" )
		{
			errors.push_back( source.Mistake( head.number, "the version is the first line only" ) );
		}
		else if ( IsDeclaration( verb ) == false && verb != "uses" )
		{
			errors.push_back( source.Mistake( head.number, "\"" + verb + "\" starts no block: a sheet's blocks are motion, use, react, predict and hud" +
															   ( verb == "on" || verb == "while" ? " (a reaction is react " + verb + " ...)" : std::string() ) ) );
		}
		i = end;
	}
	return tree;
}

std::string EngineExpression( const Tree& tree, const std::string& text, bool inMotion )
{
	const std::vector<Piece> p = PiecesOf( text );
	std::string out;
	for ( size_t i = 0; i < p.size(); ++i )
	{
		if ( p[i].word == false )
		{
			out += p[i].text;
			continue;
		}
		const std::string& w = p[i].text;
		auto param = tree.params.find( w );
		if ( param != tree.params.end() )
		{
			out += param->second;
			continue;
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
		if ( ( w == "known" || w == "active" || w == "ready" || w == "cooldown" || w == "uses" || w == "remaining" ) && spaceAt( i + 1 ) && wordAt( i + 2 ) != nullptr &&
			 NameStart( ( *wordAt( i + 2 ) )[0] ) )
		{
			// (ready, cooldown, uses: a motion's state, M287.)
			out += w == "known" ? "?" : w == "active" ? "motion." : w + ".";
			i += 1; // (the spaces: the name is read next)
			continue;
		}
		if ( inMotion && tree.itemFields.count( w ) != 0 )
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

std::string EnginePath( const std::string& path )
{
	const size_t cut = path.find_first_of( "/@" );
	const char* sign = SignOf( path.substr( 0, cut ) );
	return sign != nullptr ? sign + ( cut == std::string::npos ? std::string() : path.substr( cut ) ) : path;
}

std::string SoundPath( const std::string& name )
{
	if ( BeginsWith( name, "res://" ) )
	{
		return name;
	}
	return "res://assets/sfx/" + name + ( EndsWith( name, ".wav" ) || EndsWith( name, ".ogg" ) || EndsWith( name, ".mp3" ) ? "" : ".wav" );
}

std::string EffectPath( const std::string& name )
{
	if ( BeginsWith( name, "res://" ) )
	{
		return name;
	}
	return "res://vfx/" + name + ( EndsWith( name, ".tscn" ) || EndsWith( name, ".scn" ) ? "" : ".tscn" );
}

} // namespace cb::sheet
