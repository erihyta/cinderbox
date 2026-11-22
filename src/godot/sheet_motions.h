#pragma once

// A rules sheet's motion blocks (docs/sheets.md), baked to the lines of a motions file of
// version 3 for the simulation (sim/motions.h): what a mod's players can do besides walking and
// jumping, with no code. A set's sheets bake into its file (CbMotionSet, cinderbox_motion.h); an
// item's into motions/<kind>.cfg (CbItem). Until M268 the motion nodes (CbMotion, CbProbe,
// CbImpulse, CbForce, CbLink, CbMove, CbLaunch) baked to the same file, and a sheet's statements
// were baked through them; a sheet is the one way now.

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>
#include <vector>

namespace cb::gd
{

// What a line of a motions file cannot hold: a tab, a line's end, a #.
bool MotionTextPlain( const godot::String& text );

// The motion blocks of the rules sheets under `node` (a CbRules at any depth), baked by the sheet
// compiler (src/sheet, M283: sheet_bake.h), this scene answering for the nodes they name: their lines,
// and their names. False with `error` ("<file> line N: why").
bool BakeSheetMotions( const godot::Node* node, const godot::String& kind, std::string& lines, std::vector<std::string>& names,
					   godot::String& error );

} // namespace cb::gd
