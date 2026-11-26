#pragma once

// The sheet compiler's front end (M293, step 6.5: docs/step6.5-plan.md). A version 1 sheet's lines
// become a tree (what is indented under a line is under it), with what it uses composed in, its lets
// and units resolved, and its declarations read. The lowering (sheet_rules.cpp) walks the tree into
// the rules the engine runs; nothing is written as text in another spelling to be read again.
//
// Internal to src/sheet: the public words are sheet_rules.h and sheet_v1.h.

#include "sheet_value.h"
#include "sheet_v1.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace cb::sheet
{

// A word of a line's code: "rgb(1, 0, 0)" and "(1, 2, 3)" are one, and so is "a quoted text" (with
// its quotes).
struct Word
{
	std::string text;
	size_t start = 0;
	size_t end = 0;
};
std::vector<Word> WordsOf( const std::string& code );
// The code from a word on ("" past the last).
std::string From( const std::string& code, const std::vector<Word>& w, size_t k );

// How far in a line starts: a tab is four columns.
int Column( const std::string& line );
// The line without its comment (outside quotes) and the spaces around it.
std::string CodeOf( const std::string& line );
// The lines of a text (without the newline that ends it).
std::vector<std::string> LinesOf( const std::string& text );

// A sheet's lines with what it uses composed in (M289), each with its number: this sheet's (from 1),
// or kFromUsed + the used sheet's. The version line and the uses line are not among them.
struct Source
{
	std::vector<std::pair<std::string, int>> lines;
	std::string used; // the sheet it uses ("": none)
	int usesLine = 0;
	// "line N: why" for one of its lines (a used sheet's: "line USES: in PATH line N: why").
	std::string Mistake( int line, const std::string& why ) const;
};
Source Compose( const std::string& text, Strings& errors );

// The declarations among a source's lines (at a line's start; units checked and dropped).
std::vector<Declaration> DeclarationsOf( const Source& source, Strings& errors );

// A line of the sheet, as the lowering reads it.
struct Line
{
	int number = 0;	  // this sheet's line (from 1), or kFromUsed + the used sheet's
	int column = 0;	  // how far in it starts
	std::string code; // without its comment and the spaces around it; lets and units resolved
};

// A line and the lines under it.
struct Node
{
	Line line;
	std::vector<Node> under;
	// Its first word ("" for none).
	std::string Verb() const;
};

struct Tree
{
	Source source;
	std::vector<Node> blocks;				   // the lines at a line's start, each with what is under it
	std::map<std::string, std::string> params; // a param's name, and its number as text
	std::set<std::string> itemFields;		   // the names of the fields "on item"
	std::set<std::string> tags, statuses, motions; // (M296) what blocked by and cancels may name
	std::string Mistake( int line, const std::string& why ) const
	{
		return source.Mistake( line, why );
	}
};

// A version 1 sheet as a tree. Its mistakes (of composing, declaring, lets, units, indenting) as
// "line N: why".
Tree ReadTree( const std::string& text, Strings& errors );

// An expression in the engine's spelling (src/expr): `combat.dead of holder` is ^^:combat.dead,
// `known x` ?x, `active M` motion.M, `ready M` ready.M, `local` is_local, a param its number, and in
// a motion an item's own field item:FIELD. What is quoted stays as it is.
std::string EngineExpression( const Tree& tree, const std::string& text, bool inMotion );

// A path whose head is an anchor (holder, me, actor, target, local, world), in the engine's
// spelling: holder/Head is ^^/Head.
std::string EnginePath( const std::string& path );

// Short names for the sheet's project's files: a sound NAME is res://assets/sfx/NAME.wav, an effect
// NAME res://vfx/NAME.tscn; a full res:// path stays.
std::string SoundPath( const std::string& name );
std::string EffectPath( const std::string& name );

bool IsDeclaration( const std::string& verb );

} // namespace cb::sheet
