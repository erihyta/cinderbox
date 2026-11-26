#pragma once

// The sheet format (docs/sheets.md): version 1, its first line "cbsheet 1". Version 0 (the spelling
// before M285) is not read since M293 (step 6.5): every sheet was converted in M285, and M292
// refused it where content comes in.
//
//   cbsheet 1
//
//   motion Fire on use if gun.ammo > 0 and not gun.reloading
//       cooldown 0.2
//       change gun.ammo -= 1
//       emit item.use
//
//   react on item.use                       (a reaction: the viewer's)
//       as holder                           (scopes: the lines under them)
//           create muzzle_flash under Muzzle for 0.3
//           at Muzzle
//               sound gunshot volume -5 jitter 0.06 range 70
//
//   hud Line                                (one control's lines)
//       text "READY" if brawl.cd_dash <= 0
//
// The compiler reads a sheet as a tree (sheet_tree.h) and lowers it into rules (sheet_rules.h).

#include "sheet_rules.h"

#include <functional>
#include <string>
#include <vector>

namespace cb::sheet
{

// The version a sheet's first line says ("cbsheet N"): 0 without one, -1 when it names one this
// compiler does not read.
int SheetVersion( const std::string& text );
// What is said of a sheet that is not version 1 (the server reading a mod's sheets, publishing,
// cb_sheet check, the editor's Sheet screen); empty for a version 1 sheet, or one with nothing in it.
std::string NotVersion1( const std::string& text );

// A version 1 sheet in the one style: indented with tabs (four columns each), no spaces at the ends
// of lines, at most one blank line in a row, one newline at the end. False, with why, when the
// result would not parse to what the sheet did.
bool FormatVersion1( const std::string& text, std::string& formatted, std::string& why );

// What a sheet means, for comparing two spellings of it: its rules and its mistakes as Dump writes
// them, without line numbers and with every dictionary's keys in order.
std::string Meaning( const std::string& text );

// --- Declarations (M286): the names a sheet brings, at the start of a line, one each --------------
//
//   field gun.ammo int                      a field of the player (the default), sent to its player
//   field team.id int shared                ... sent to every player
//   field shop.coins int private            ... its player's alone, never shown on others
//   field deathmatch.round int on world     the game's, one for everyone
//   field gun.rounds int on item = 12       a field of each copy of the item, what a new one starts with
//   event brawl.jab.hit                     an event (said at a character: heard by who sees it)
//   event deathmatch.kill everyone          ... heard by every player
//   action reload key R                     a key a player presses (R by default)
//   param gun.damage 25                     a number of the item whose scene holds the sheet: the
//   param gun.reload_seconds 1.5 s          server's item property, and this sheet's word for it
//   status stunned                          a timed flag on a player (M288): applied, read, blocking
//
// The server declares a sheet's fields, events and actions for the mod whose item has it (after
// the mod's C++: a name the C++ declares with another type is a mistake when the server starts);
// the item's bake writes its params and its item fields into items/<kind>.cfg. In the sheet, a
// param is its number wherever an expression names it. Units (s, m, m/s, deg) may follow a number
// anywhere; they are checked against the word they are for, and dropped.
struct Declaration
{
	enum class Kind
	{
		Field,
		Event,
		Action,
		Param,
		Status, // (M288) a timed flag on a player: stunned
		Tag, // (M296) a name motions carry (tags NAME) and are blocked by or cancel: attack
	};
	Kind kind = Kind::Field;
	int line = 0;
	std::string name;
	std::string type = "int";	// a field's: int, float, bool, timer (a float that counts down to 0, a second a second)
	std::string on = "player";	// a field's: player, world, item
	bool shared = false;		// a field's: sent to every player
	bool privately = false;		// a field's: its player's alone
	bool everyone = false;		// an event's
	std::string key;			// an action's
	double value = 0.0;			// a param's; an item field's start
};
std::vector<Declaration> ReadDeclarations( const std::string& text, Strings& errors );

// --- One sheet for many items (M289) --------------------------------------------------------------
//
//   cbsheet 1
//   uses res://prefabs/gun.cbsheet          its declarations, lets and blocks, as if written here
//   param gun.reload_seconds 2 s            a declaration of the same name: this sheet's instead
//   field gun.rounds int on item = 30
//   replace motion Fire when using.gun.rifle and gun.rounds > 0     a block of the same head: this one
//       ...
//
//   let hitbox = box (0.8, 1.2, 2.6) at (0, 1, 1.4) turned look     a word for some words, wherever
//   find hitbox max 1                                                it stands (not in quotes); a
//                                                                    word of the language is no let's name
//
// A sheet uses one other at most, which uses none. A block's head is its first line (a motion's:
// "motion NAME"). A line of the used sheet is said as "in PATH line N" at the uses line, and its
// rules' lines are kFromUsed + its own.

// How a sheet a "uses" line names is read, by its res:// path: each host's (the editor's files, a
// workshop item's, the command line's folder). None: "uses" is a mistake.
using SheetLoader = std::function<bool( const std::string& path, std::string& text )>;
void SetSheetLoader( SheetLoader loader );
const SheetLoader& TheSheetLoader();

inline constexpr int kFromUsed = 100000;

} // namespace cb::sheet
