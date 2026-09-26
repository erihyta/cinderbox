#pragma once

// A character's animation state machine, authored as a Godot AnimationTree and baked to text
// (graph.cfg), run by the simulation.
//
// The graph decides which clip plays when; the simulation runs it every tick for every player, so
// the server's hit tests and every client's pose agree, and a rollback replays it exactly. It needs
// no clip data beyond each clip's length, loop flag and markers: the bones are ozz's business (the
// pose samples the clips by name).
//
//   - Layers: state machines stacked like the AnimationTree's Blend2 nodes (the first is the base,
//     each later one blends over it through its bone mask, by its weight expression).
//   - States: one clip, or a 1D or 2D blend space (clips by input expressions, phase-synced; a 2D
//     space blends inside Godot's triangles, like BlendSpace2D).
//   - Transitions: Godot's (priority, crossfade, immediate or at end), taken when their condition
//     holds: the advance condition and the advance expression, both compiled here.
//
// Conditions read simulation values only, so every machine gets the same answer:
//   speed, forward_speed, move_forward, move_right, vertical_speed, grounded, airborne_time, jumped,
//   aiming, backward, state_time; a stance's name (true while any layer has it); a mod event's name (on the tick it
//   is emitted at this player: its value, or 1 when that is 0, so "attack" and "attack == 2" both
//   read); a board field's name (the player's value, or the global one); an item kind's name (true
//   while the player holds one, in any socket: "attack and melee.bat").
// Markers on clips emit the mod event of the same name when the playing state crosses them.

#include "components.h"
#include "mod_schema.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cb
{

struct ModEventRecord;

inline constexpr int kMaxGraphStates = 64;	   // per layer
inline constexpr int kMaxBlendPoints = 32;	   // per blend space

// A compiled condition or input: a little stack program over the values above.
struct AnimExpr
{
	enum class Op : uint8_t
	{
		Const,
		Var,
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
	enum class VarKind : uint8_t
	{
		Builtin,
		Stance,		 // index: stance number in AnimState (schema index + 1)
		Event,		 // index: schema event
		EntityField, // index: board slot
		GlobalField,
		HeldKind, // index: schema item kind
		Zero,	  // an unknown name: reads as 0 (reported when the graph is compiled)
	};
	enum Builtin : uint8_t
	{
		Speed,
		ForwardSpeed,
		VerticalSpeed,
		Grounded,
		AirborneTime,
		Jumped,
		Aiming,
		Backward,
		StateTime,
		MoveForward,
		MoveRight,
		BuiltinCount,
	};
	struct Step
	{
		Op op = Op::Const;
		VarKind kind = VarKind::Zero;
		BoardType type = BoardType::Int;
		uint8_t index = 0;
		float value = 0.0f;
	};
	std::vector<Step> steps; // empty: always true (conditions) or 0 (inputs)
	std::string text;

	bool Empty() const
	{
		return steps.empty();
	}
};

struct AnimGraphClip
{
	std::string name; // the Godot animation's name; the pose finds the ozz clip by it
	float length = 1.0f;
	bool loops = false;
	struct Marker
	{
		float time = 0.0f;
		std::string name;
		int event = -1; // schema event it emits, -1 when no mod declared it
	};
	std::vector<Marker> markers;
};

struct AnimGraphTransition
{
	int to = 0;
	int priority = 1;
	float xfade = 0.0f;
	bool atEnd = false;
	AnimExpr condition;
};

struct AnimGraphState
{
	std::string name;
	// One clip (a single point) or a blend space: points by position, blended by `input` (and
	// `inputY` for a 2D space, inside `triangles`).
	struct Point
	{
		float position = 0.0f;
		float y = 0.0f; // 2D spaces
		int clip = 0;
		bool backward = false;
	};
	std::vector<Point> points;
	bool blend = false;
	bool planar = false; // a 2D blend space
	AnimExpr input;
	AnimExpr inputY;
	std::vector<std::array<int, 3>> triangles; // point indices, as Godot triangulated them
	std::vector<AnimGraphTransition> transitions; // in the order they are tried
};

struct AnimGraphLayer
{
	std::string name;
	std::vector<std::string> mask; // bones (humanoid-profile names) it covers; empty: every bone
	AnimExpr weight;			   // empty: 1
	int start = 0;
	std::vector<AnimGraphState> states;
};

struct AnimGraph
{
	std::vector<AnimGraphClip> clips;
	std::vector<AnimGraphLayer> layers; // at most kMaxAnimLayers
	std::string text;					// what it was compiled from

	int FindClip( const std::string& name ) const;
	// True when some clip's marker emits this schema event.
	bool EmitsEvent( int event ) const;
};

// Animation packs a server's mods provide (sim/mod_schema.h animPacks), compiled; index = source - 1.
using AnimGraphPacks = std::vector<std::shared_ptr<const AnimGraph>>;

// The layer that plays as the character's layer `l` for `source` (0: its own; n: pack n-1's layer of
// the same name), and the graph it belongs to (its clips, markers). A source that does not provide
// that layer falls back to the character's own.
const AnimGraphLayer& ResolveLayer( const AnimGraph& character, const AnimGraphPacks& packs, size_t l, uint8_t source,
									const AnimGraph*& owner );

// Compiles graph.cfg text against the server's schema (names in conditions become board slots,
// stances and events). Unknown names read as 0 and are listed in `warnings`; a malformed file
// fails with `error`.
std::shared_ptr<const AnimGraph> CompileAnimGraph( const std::string& text, const ModSchema& schema, std::string& error,
												   std::string& warnings );

// Compiles every animation pack of a schema (index = source - 1); a pack whose graph does not
// compile (or is empty: its item was missing) is null, so its swaps fall back to the character's own.
AnimGraphPacks CompileAnimPacks( const ModSchema& schema, std::string& warnings );

// Compiles one expression on its own (tests, and the bake's validation). `schema` may be empty.
bool CompileAnimExpr( const std::string& text, const ModSchema& schema, AnimExpr& out, std::string& error, std::string& warnings );

// Everything a condition can read, for one player on one tick.
struct AnimGraphInputs
{
	float builtins[AnimExpr::BuiltinCount] = {};
	const AnimState* state = nullptr;
	const int32_t* board = nullptr;		  // the player's (null: all 0)
	const int32_t* globalBoard = nullptr; // SimGlobals::board
	const ModEventRecord* events = nullptr;
	uint32_t eventCount = 0; // SimGlobals::modEventCount
	uint32_t tick = 0;
	uint32_t netId = 0;
	// Kinds of the items the player holds (schema indices).
	const uint16_t* heldKinds = nullptr;
	uint32_t heldCount = 0;
};

float EvaluateAnimExpr( const AnimExpr& expr, const AnimGraphInputs& inputs, float stateTime );

// How much of each of a state's points plays at a blend input (x, and y for a 2D space); a single
// clip's only point gets 1. Weights sum to 1.
using AnimBlendWeights = std::array<float, kMaxBlendPoints>;
void AnimGraphWeights( const AnimGraphState& state, float x, float y, AnimBlendWeights& out );

// Advances the graph's layers in `state` by one tick (a layer not started yet begins in its start
// state). Markers crossed append their schema event
// indices to `markers`.
void UpdateAnimGraph( AnimState& state, const AnimGraph& graph, const AnimGraphPacks& packs, AnimGraphInputs& inputs, float dt,
					  std::vector<int>& markers );

// Parses a decimal number the same way on every machine (not the C library's, which depends on the
// locale and the platform's rounding): graph.cfg and expressions feed the simulation.
bool ParseAnimFloat( const char* text, size_t length, float& out );

} // namespace cb
