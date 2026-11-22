#pragma once

// A rules sheet, parsed (M283: the one parser of the sheet language, plain C++). The language is
// docs/sheets.md (motion blocks) and docs/looks.md (reactions, predictions, HUD lines);
// docs/step6-plan.md is where it goes next. Until M283 this was cue_rules.cpp's ParseRules, on
// Godot's types: the same lines read the same way, to the same properties, so every sheet that
// shipped parses as it did (the golden files: tests/golden/sheets).
//
// A rule is what one node becomes: a CbReaction (one action of an "on" or "while" rule, its
// settings at the time), a CbPrediction, a CbFieldBinding (a HUD line), or a motion block ("motion",
// a use: run by the simulation, baked by sheet_bake.h). Its properties are a CbReaction's,
// a CbPrediction's or a CbFieldBinding's by name; a motion's are its settings and its steps.

#include "sheet_value.h"

#include <string>
#include <vector>

namespace cb::sheet
{

struct Rule
{
	int line = 0;
	int head = 0; // the line its rule starts on (on, while, predict, use): where its event and its "if" are
	bool prediction = false;
	std::string type = "CbReaction"; // CbReaction, CbPrediction, CbFieldBinding; "motion": a motion block
	Dict properties;
};

// The rules of a sheet. Problems are "line N: why"; a rule with one is left out.
std::vector<Rule> ParseRules( const std::string& text, Strings& errors );

// The movement parameters' names (sim/move_params.h), said once by whoever knows them (the
// extension, the server, the tests): in a move's line, where one value (an expression of several
// words) ends and the next parameter starts. Without them each value is one word.
void SetMoveParameterNames( const Strings& names );

// The CbReaction properties that hold a node path.
bool IsPathProperty( const std::string& name );

// A prediction's (and a motion's, a use's) "FIELD OP NUMBER": pistol.ammo -= 1.
struct Change
{
	std::string field;
	char op = '='; // '-', '+' or '='
	double value = 0.0;
};
bool ParseChange( const std::string& text, Change& out, std::string* error = nullptr );

// A CbReaction's "when", as cue_reaction.h numbers it.
inline constexpr int kReactionOnEvent = 0;
inline constexpr int kReactionWhile = 1;

} // namespace cb::sheet
