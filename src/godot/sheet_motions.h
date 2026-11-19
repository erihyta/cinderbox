#pragma once

// A rules sheet's motion blocks (docs/rules-language.md), baked to the lines of a motions file of
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

// How many points a path a motion follows is baked into.
inline constexpr int kPathSamples = 32;

// What a line of a motions file cannot hold: a tab, a line's end, a #.
bool MotionTextPlain( const godot::String& text );

// A motion block of a rules sheet (cue_rules.h: its properties) as one motion's lines of a motions
// file of version 3. `kind`: the item whose sheet it is (its "on use", and who may run it: whoever has
// the item out, or carries it), empty in a motion set's. "" with `error` when it cannot be baked.
// `from`: the node the sheet's CbRules is under (a find's node path starts there); `root`: the item
// or the motion set being baked (a find's shape is placed in its frame).
std::string BakeSheetMotion( const godot::Dictionary& motion, const godot::String& kind, const godot::Node* from, const godot::Node* root,
							 godot::String& error );

// A find's shape (a sheet's "find SHAPE": its properties) as "shape" lines of a motions file: the
// convex pieces of a node of the scene (an Area3D's CollisionShape3D children, or one), in the
// player's frame; or one written in the line, around the player's middle.
bool FindShapeLines( const godot::Dictionary& shape, const godot::Node* from, const godot::Node* root, std::string& out, godot::String& why );

// The motion blocks of the rules sheets under `node` (a CbRules at any depth), baked: their lines,
// and their names. False with `error` ("<file> line N: why").
bool BakeSheetMotions( const godot::Node* node, const godot::String& kind, std::string& lines, std::vector<std::string>& names,
					   godot::String& error );

} // namespace cb::gd
