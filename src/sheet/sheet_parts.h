#pragma once

// The parts of a scene a sheet's motions name (M294, step 6.5: docs/step6.5-plan.md §3.2): the
// markers, paths and shapes a find, a cast, a launch or a follow names, as data. The editor's bake
// records them (RecordingScene) and writes them with the names of the sheets, as an owner's
// motions/<owner>.cfg; the server compiles the sheets against them (PartsScene), so every machine
// runs what the sheet says, and the item carries no motion it compiled elsewhere.
//
//   cinderbox_parts	1
//   # what made it
//   kind	gun.pistol                         an item's (none for a motion set)
//   use	1                                  how the item is used (its CbItem's use)
//   sheet	prefabs/pistol.cbsheet             a sheet, then what its motions name:
//   marker	Muzzle	ZX	ZY	ZZ	X	Y	Z            its forward axis and its place
//   path	Arc	X	Y	Z	...                    kPathSamples points
//   shapes	Blast	root	MX	MY	MZ	N            a node's shapes (root or own frame, moved by M): N pieces
//   piece	R	X	Y	Z	...                      a piece: its radius and its points
//
// Every number as the bake had it (Num: it reads back the same float), so the server's motions are
// the editor's, byte for byte.

#include "sheet_bake.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cb::sheet
{

// What a sheet's motions asked of the scene it is in, with the answers.
struct Parts
{
	struct Marker
	{
		Float3 zAxis, origin;
	};
	std::map<std::string, Marker> markers;			   // by path
	std::map<std::string, std::vector<Float3>> paths;  // by path
	std::map<std::string, std::vector<ShapePiece>> shapes; // by ShapesKey
};
// A node's shapes asked for: its path, in which frame, moved by how much.
std::string ShapesKey( const std::string& path, bool own, Float3 moved );

// A scene that answers from recorded parts (the server, cb_sheet): what was not recorded is a mistake
// (the scene changed after its bake: bake it again).
class PartsScene final : public Scene
{
public:
	explicit PartsScene( const Parts& parts );
	bool Place( const std::string& path, const char* what, Float3& zAxis, Float3& origin, std::string& why ) const override;
	bool PathSamples( const std::string& path, std::vector<Float3>& out, std::string& why ) const override;
	bool Shapes( const std::string& path, bool own, Float3 moved, std::vector<ShapePiece>& out, std::string& why ) const override;

private:
	const Parts& m_parts;
};

// A scene that answers as another does and records each answer (the editor's bake).
class RecordingScene final : public Scene
{
public:
	RecordingScene( const Scene& inner, Parts& parts );
	bool Place( const std::string& path, const char* what, Float3& zAxis, Float3& origin, std::string& why ) const override;
	bool PathSamples( const std::string& path, std::vector<Float3>& out, std::string& why ) const override;
	bool Shapes( const std::string& path, bool own, Float3 moved, std::vector<ShapePiece>& out, std::string& why ) const override;

private:
	const Scene& m_inner;
	Parts& m_parts;
};

// Whose motions: an item (its kind, and how it is used) or a motion set (no kind), its sheets that
// have motion blocks, and the parts each one's motions name.
struct OwnerParts
{
	std::string kind;
	int use = 0;
	struct Sheet
	{
		std::string path; // its file in the item ("prefabs/pistol.cbsheet")
		Parts parts;
	};
	std::vector<Sheet> sheets;
};
// As a file's text (above); `about` is its comment line.
std::string WriteParts( const OwnerParts& owner, const std::string& about );
// False with why (a motions file baked before M294 among them: bake it again).
bool ReadParts( const std::string& text, OwnerParts& out, std::string& error );

// One sheet of an owner, and the scene that answers for the nodes it names.
struct SheetSource
{
	std::string name; // as its mistakes say it ("prefabs/pistol.cbsheet", or "(a sheet of its own)")
	std::string path; // its file in the project, for the parts ("": kept in a scene)
	std::string text;
	const Scene* scene = nullptr;
};
// An owner's motion blocks, from all its sheets, as a motions file's lines (after its header), and
// their names. With `record`, the parts each sheet's motions name are recorded there, a sheet each.
// False with error ("NAME line N: why"). The editor's bake and the server's run this one function.
bool BakeOwnerMotions( const std::vector<SheetSource>& sheets, const std::string& kind, int use, std::string& lines, std::vector<std::string>& names,
					   OwnerParts* record, std::string& error );

// A move's values end at the next movement parameter's name: the simulation's names, for the parser
// (SetMoveParameterNames). MotionsFromParts says them itself.
void UseSimulationMoveParameters();

// An owner's motions file (sim/motions.h: what the server sends every simulation), compiled from its
// sheets, which `read` gives by their paths in the item, and their recorded parts. False with error.
bool MotionsFromParts( const OwnerParts& owner, const std::function<bool( const std::string& path, std::string& text )>& read, std::string& text,
					   std::string& error );

} // namespace cb::sheet
