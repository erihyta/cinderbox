#pragma once

// A rules sheet: a look's reactions as lines of text, in a file beside the scene (brawl.rules), not
// one CbReaction node each. Logic is text; what is a place or a thing (a node to light up, a mesh, a
// socket) stays a node, and a rule names it by its path. A CbRules node in the scene points to the
// file; when the scene is used, each action of each rule becomes a CbReaction under it, the same as
// one placed by hand, so the director, the guard and the Cue Preview see nothing new.
//
//   # The kit's hits: a thud where they land.
//   on brawl.jab.hit
//       subject $at
//       at point
//       sound res://assets/sfx/bat_hit.wav volume -6 jitter 0.1 range 40
//   on brawl.finisher.hit if $other:combat.health < 20
//       subject $at
//       at point
//       sound res://assets/sfx/bat_hit.wav
//       shake 0.2
//   while melee.hot
//       set Barrel surface_material_override/0:emission_energy_multiplier 4 blend 0.2
//
// A rule starts with "on <event> [if <expression>]" or "while <expression>"; the lines under it
// (indenting is for the eye) are its settings and its actions:
//
//   settings   subject PATH, side a|b|either, kind KIND, template NAME, delay S, chance P,
//              cooldown S, wait (wait for the server's cue), at point|end|beam|follow|PATH
//              [from PATH: a beam's start] [offset X Y Z]: for every action of the rule
//   actions    sound PATH [volume DB] [pitch P] [jitter J] [range M] [bus NAME] [volume_by EXPR]
//              scene PATH [under PATH] [life S]
//              play PLAYER ANIMATION [off ANIMATION]
//              set PATH PROPERTY VALUE [blend S]      (VALUE: a number, true, false, rgb(r,g,b),
//                                                     rgba(r,g,b,a), (x,y,z), a word or "text")
//              set PATH PROPERTY = EXPRESSION
//              call PATH METHOD [ARGUMENT ...]
//              shake AMOUNT [time S]
//              flash rgba(r,g,b,a) [time S]
//
// Paths are cue paths (cue_paths.h): an anchor ($at, $other, ^, ^^, $local, $world) and a path from
// it, or a plain path from the node the CbRules node is under ("Barrel", "Muzzle/Sparks"; "." is
// that node itself), which climbs no higher than the scene. # starts a comment. A line that cannot
// be read is said with its number, and its rule is left out; the rest of the sheet still works.

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

namespace cb::cue
{

// One action of a rule, as the CbReaction it becomes: its properties by name, and its line.
struct Rule
{
	int line = 0;
	godot::Dictionary properties;
};

// The rules of a sheet. Problems are "line N: why"; a rule with one is left out.
std::vector<Rule> ParseRules( const godot::String& text, godot::PackedStringArray& errors );

// The CbReaction properties that hold a node path.
bool IsPathProperty( const godot::String& name );

} // namespace cb::cue

namespace cb::gd
{

// A scene's rules sheet (cue_rules.h): the file it reads, and the CbReactions its rules become, under
// it (internal children: never saved with the scene).
class CbRules : public godot::Node
{
	GDCLASS( CbRules, godot::Node )

public:
	void set_file( const godot::String& v );
	godot::String get_file() const
	{
		return m_file;
	}
	// How many reactions its rules made (0 before it is in the tree), and what it could not read.
	int64_t get_reaction_count() const;
	godot::PackedStringArray get_problems() const
	{
		return m_problems;
	}
	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();
	void _notification( int what );

private:
	void Build();
	void Clear();
	godot::String m_file;
	godot::PackedStringArray m_problems;
	bool m_built = false;
};

} // namespace cb::gd
