#pragma once

// Version 1 of the sheet language (M285; docs/sheets.md, docs/step6-plan.md §6): one spelling, its
// first line "cbsheet 1". A sheet without it is version 0 (the spelling before M285), read until no
// sheet ships it.
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
// Version 1 says what version 0 said, in other words: the parser reads it by lowering it, line for
// line, to version 0 (LowerVersion1), so both mean exactly the same, and a sheet converted from
// version 0 (ConvertToVersion1) is checked to parse to what it parsed to. What version 1 says that
// version 0 cannot (step 6's later words) is the compiler's to learn as the engine does.

#include "sheet_rules.h"

#include <string>
#include <vector>

namespace cb::sheet
{

// The version a sheet's first line says ("cbsheet N"): 0 without one, -1 when it names one this
// compiler does not read.
int SheetVersion( const std::string& text );

// Version 1 as version 0: the text, and for each of its lines (from 1) the version 1 line it came
// from. Mistakes of version 1's own ("line N: why", N its own) in `errors`.
struct Lowered
{
	std::string text;
	std::vector<int> lineOf; // [k]: the source line of lowered line k + 1
};
Lowered LowerVersion1( const std::string& text, Strings& errors );

// Version 0 as version 1, comments kept: false, with why, when a line has no version 1 spelling
// yet, or the converted sheet would not parse to what the original did (it never writes one that
// means something else).
bool ConvertToVersion1( const std::string& version0, std::string& version1, std::string& why );

// A version 1 sheet in the one style: indented with tabs (four columns each), no spaces at the ends
// of lines, at most one blank line in a row, one newline at the end. False, with why, when the
// result would not parse to what the sheet did.
bool FormatVersion1( const std::string& text, std::string& formatted, std::string& why );

// What a sheet means, for comparing two spellings of it: its rules and its mistakes as Dump writes
// them, without line numbers and with every dictionary's keys in order.
std::string Meaning( const std::string& text );

} // namespace cb::sheet
