#pragma once

// A sheet's motion blocks, baked for the simulation (M283: moved from src/godot/sheet_motions.cpp,
// line for line, onto plain types). A motion block is parsed by ParseRules (sheet_rules.h); this
// writes it as a motions file's text (sim/motions.h reads it), and checks that text with the
// simulation's own compiler.
//
// What a sheet names in its scene (a shape a find uses, a marker, a path) is the scene's: a Scene
// answers for it. In the editor it is the Godot scene the sheet is in (src/godot/sheet_motions.cpp);
// on the server, the parts the editor's bake recorded (sheet_parts.h, M294).

#include "sheet_text.h"
#include "sheet_value.h"

#include <string>
#include <vector>

namespace cb::sheet
{

// Samples a follow's path is written as.
inline constexpr int kPathSamples = 32;

// A piece of a shape a find (or a cast) uses, as its line has it: a radius (0 for a box, a cylinder, a
// hull) and its points, in Godot's frame of what is baked (the line turns them).
struct ShapePiece
{
	float radius = 0.0f;
	std::vector<Float3> points;
};

class Scene
{
public:
	virtual ~Scene() = default;

	// A node beside the sheet (a Marker3D): its forward axis (the basis' third column) and its origin,
	// in the frame of what is baked. False with why (`what`: the word that named it, "along", "from").
	virtual bool Place( const std::string& path, const char* what, Float3& zAxis, Float3& origin, std::string& why ) const = 0;
	// A Path3D beside the sheet: kPathSamples points along it, evenly, from where it starts, in the
	// frame of what is baked. False with why.
	virtual bool PathSamples( const std::string& path, std::vector<Float3>& out, std::string& why ) const = 0;
	// A node's shapes (an Area3D's CollisionShape3D children, or one), those that are on, each moved
	// by `moved` (in the shape's frame: Godot's): in the frame of what is baked, or with `own` in the
	// node's own frame (a cast's shape, around its origin). False with why.
	virtual bool Shapes( const std::string& path, bool own, Float3 moved, std::vector<ShapePiece>& out, std::string& why ) const = 0;
};

// A motion block (a Rule of type "motion": its properties) as a motions file's lines; "" with why.
// `kind`: the item kind of the sheet ("" for a motion set's); `use`: how the item is used (its
// CbItem's use: kItemUsePress, kItemUseHold, ...).
std::string BakeSheetMotion( const Dict& motion, const std::string& kind, int use, const Scene& scene, std::string& error );

} // namespace cb::sheet
