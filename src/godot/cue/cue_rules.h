#pragma once

// A rules sheet: a look's reactions as lines of text, in a file beside the scene (brawl.cbsheet: plain
// text, which Godot's script editor opens, M297.3), not one CbReaction node each. Logic is text; what is a place or a thing (a
// node to light up, a mesh, a socket) stays a node, and a rule names it by its path. A CbRules node
// in the scene holds the sheet; when the scene is used, each action of each rule becomes a
// CbReaction under it, the same as one placed by hand, so the director, the guard and the Cue
// Preview see nothing new.
//
//   cbsheet 1
//
//   # The kit's hits: a thud where they land.
//   react on brawl.jab.hit
//       as actor
//           at point
//               sound bat_hit volume -6 jitter 0.1 range 40
//   react while melee.hot
//       set Barrel surface_material_override/0:emission_energy_multiplier 4 blend 0.2
//
// The language (its blocks: motion, use, react, predict, hud; scopes; the words) is docs/sheets.md,
// and the parser the sheet compiler's (src/sheet: sheet_rules.h, sheet_v1.h). A sheet without its
// "cbsheet 1" line is version 0, the spelling before M285, read until none ships. Plain paths are
// from the node the CbRules node is under ("Barrel"; "." is that node), and climb no higher than the
// scene. A line that cannot be read is said with its number, and its rule is left out; the rest of
// the sheet still works.

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

namespace cb::cue
{

// One action of a rule, as the CbReaction it becomes (or a prediction, as its CbPrediction): its
// properties by name, and its line.
struct Rule
{
	int line = 0;
	int head = 0; // the line its rule starts on (on, while, predict, use): where its event and its "if" are
	bool prediction = false;
	godot::String type = "CbReaction"; // the node it becomes: CbReaction, CbPrediction, CbFieldBinding;
										   // "motion": a motion block (a use is one), for its bake
	godot::Dictionary properties;
};

// The rules of a sheet. Problems are "line N: why"; a rule with one is left out.
std::vector<Rule> ParseRules( const godot::String& text, godot::PackedStringArray& errors );

// The CbReaction properties that hold a node path.
bool IsPathProperty( const godot::String& name );

// The movement parameters' names (sim/move_params.h), said once by the extension that knows them
// (register_types.cpp): in a move's line, where one value (an expression of several words) ends and
// the next parameter starts. Without them each value is one word.
void SetMoveParameterNames( const godot::PackedStringArray& names );

} // namespace cb::cue

namespace cb::gd
{

// A scene's rules sheet (cue_rules.h): the sheet it reads (sheet: a .cbsheet file of the project, by
// its path, M297.3; until then a CbSheet resource), and the CbReactions (and CbPredictions) its rules
// become, under it (internal children: never saved with the scene). A Node3D: what a rule adds with
// no place of its own (a scene, a sound) goes where this node is. In the editor, a sheet saved (in
// the script editor, or anywhere) builds them again.
class CbRules : public godot::Node3D
{
	GDCLASS( CbRules, godot::Node3D )

public:
	void set_sheet( const godot::String& v );
	godot::String get_sheet() const
	{
		return m_sheet;
	}
	// What the sheet says ("" without one, or one that cannot be read), and what it is called in a
	// message (its path).
	godot::String SheetText() const;
	godot::String SheetName() const
	{
		return m_sheet;
	}
	// Why the sheet cannot be read ("" when it can: none set, or a file of the project's there).
	godot::String SheetProblem() const;
	// How many reactions its rules made (0 before it is in the tree), and what it could not read.
	int64_t get_reaction_count() const;
	godot::PackedStringArray get_problems() const
	{
		return m_problems;
	}
	godot::PackedStringArray _get_configuration_warnings() const override; // (get_warnings, to tools)

protected:
	static void _bind_methods();
	void _notification( int what );

private:
	void Build();
	void Clear();
	godot::String m_sheet;
	godot::PackedStringArray m_problems;
	bool m_built = false;
	// (The editor's: when the file was last read, and how long since it was last looked at.)
	uint64_t m_modified = 0;
	double m_sinceLook = 0.0;
};

} // namespace cb::gd
