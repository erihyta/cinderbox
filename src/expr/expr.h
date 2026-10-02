#pragma once

// The one expression language: what conditions and values are written in, wherever they are read.
// A character's state machine, a reaction, a prediction and a HUD node all parse the same text;
// they differ only in where a name's value comes from.
//
//   numbers        3   0.25   1e-3   true (1)   false (0)
//   names          pistol.ammo   speed   is_local   event.value
//                  with a path and a colon where the reader knows paths: ^^:combat.dead,
//                  $other:combat.health, $world:deathmatch.round
//   ?name          1 when the name is known (its mod runs), else 0
//   arithmetic     + - * /   unary -   ( )
//   comparison     == != < <= > >=       (1 or 0)
//   logic          not !   and &&   or ||
//
// From loosest to tightest: or, and, not, comparison, + -, * /, unary -. So "!a == b" is
// "not (a == b)", and "a or b and c" is "a or (b and c)". Anything that is not 0 is true. A name
// the reader does not know reads as 0; dividing by 0 gives 0. An empty expression is true.
//
// Compile() turns text into a small stack program with the names kept as text; Evaluate() runs it
// against a reader. The simulation resolves the names once instead and runs its own copy of the
// program (sim/anim_graph.h), with the same arithmetic.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cb::expr
{

enum class Op : uint8_t
{
	Const,
	Name,  // the value of names[name]
	Known, // 1 when names[name] is known
	Not,
	Negate,
	Add,
	Sub,
	Mul,
	Div,
	Less,
	LessEqual,
	Greater,
	GreaterEqual,
	Equal,
	NotEqual,
	And,
	Or,
};

struct Step
{
	Op op = Op::Const;
	uint16_t name = 0;
	float value = 0.0f;
};

struct Program
{
	std::vector<Step> steps; // empty: always true
	std::vector<std::string> names;
	std::string text;

	bool Empty() const
	{
		return steps.empty();
	}
};

// Deeper nesting than this does not compile, so evaluation runs on a fixed stack.
inline constexpr int kMaxStack = 32;

// False with `error` when the text is not an expression.
bool Compile( const std::string& text, Program& out, std::string& error );

// A name's value: false when the reader does not know the name (it then reads as 0).
using Reader = std::function<bool( const std::string& name, float& value )>;

// The value of the program (1 for an empty one).
float Evaluate( const Program& program, const Reader& read );

// One binary step on two values, as Evaluate does it: shared with readers that run their own copy
// of a program (the simulation), so the arithmetic is the same everywhere.
float Apply( Op op, float a, float b );

// Splits "path:name" at its colon ("" for the path of a plain name).
void SplitName( const std::string& name, std::string& path, std::string& plain );

// Parses a decimal number the same way on every machine (not the C library's, which depends on the
// locale and the platform's rounding): expressions and graph.cfg feed the simulation.
bool ParseFloat( const char* text, size_t length, float& out );

} // namespace cb::expr
