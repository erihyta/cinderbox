#pragma once

// A rules sheet's motion blocks (docs/sheets.md), baked to the lines of a motions file of
// version 3 for the simulation (sim/motions.h): what a mod's players can do besides walking and
// jumping, with no code. The editor bakes them to check them and to record the parts of the scene
// they name (a set's into its motions/<set_name>.cfg, CbMotionSet; an item's into
// motions/<kind>.cfg, CbItem); the server compiles the same from the sheets and those parts (M294). Until M268 the motion nodes (CbMotion, CbProbe,
// CbImpulse, CbForce, CbLink, CbMove, CbLaunch) baked to the same file, and a sheet's statements
// were baked through them; a sheet is the one way now.

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>
#include <vector>

namespace cb::sheet
{
struct OwnerParts;
}

namespace cb::gd
{

// What a line of a motions file cannot hold: a tab, a line's end, a #.
bool MotionTextPlain( const godot::String& text );

// The motion blocks of the rules sheets under `node` (a CbRules at any depth), baked by the sheet
// compiler (src/sheet: sheet_parts.h, BakeOwnerMotions), this scene answering for the nodes they name:
// their lines, and their names; with `parts`, what they named of the scene, recorded (M294: the file
// the item carries, from which the server compiles the same). `kind` and `use`: an item's (its CbItem's
// kind and use), or "" and 0 for a motion set. False with `error` ("<file> line N: why").
bool BakeSheetMotions( const godot::Node* node, const godot::String& kind, int use, std::string& lines, std::vector<std::string>& names,
					   cb::sheet::OwnerParts* parts, godot::String& error );

} // namespace cb::gd
