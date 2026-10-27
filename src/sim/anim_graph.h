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
//   aiming, backward, state_time; (for motions: linked, held.<action>, pressed.<action>;) a stance's name (true while any layer has it); a mod event's name (on the tick it
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

// What "held.jump" and "held.sprint" are, besides a mod's action bit (0 to kMaxActions - 1).
inline constexpr int kMotionActionJump = kMaxActions;
inline constexpr int kMotionActionSprint = kMaxActions + 1;

inline constexpr int kMaxGraphStates = 64;	   // per layer
inline constexpr int kMaxBlendPoints = 32;	   // per blend space
inline constexpr float kMaxStateSpeed = 16.0f;

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
		Curve, // the value on the stack through the curve `index` (AnimGraphInputs::curves)
	};
	enum class VarKind : uint8_t
	{
		Builtin,
		Stance,		 // index: stance number in AnimState (schema index + 1)
		Event,		 // index: schema event
		EntityField, // index: board slot
		GlobalField,
		HeldKind, // index: schema item kind
		// "held.dash", "held.jump": the action is down this tick (index: its bit, or kMotionActionJump /
		// kMotionActionSprint). Motions read it; a state machine has no input and reads 0.
		HeldAction,
		// "pressed.dash", "pressed.jump": the action went down this tick. The same index; motions only.
		PressedAction,
		Zero,	  // an unknown name: reads as 0 (reported when the graph is compiled)
		// "carried.gun.pistol": the player has an item of the kind, in the hand or put away (index:
		// schema item kind). The kind's bare name asks for the hand alone.
		CarriedKind,
		// "motion.dash.boots.Use", "motion.Hook": that motion of the player's is on (index: its
		// place among the server's motions; the first 64 can be asked). The set's name and the
		// node's, or the node's alone when no other motion has it.
		MotionOn,
		// "using.gun.pistol": the player uses an item of the kind this tick: in the hand with the use
		// button down, or by its slot's key (the press; while held, for one used while held).
		UsingKind,
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
		// A probe of the player's motions holds on to something (sim/motions.h). Motions read it; a
		// state machine reads 0.
		Linked,
		// How far the player is from what that probe holds on to, in metres (0: none is out). So a
		// motion can let go when it has arrived: "link_distance < 1.5".
		LinkDistance,
		// A motion's (sim/motions.h): seconds since its step started; inside a find, how far what it
		// found is (metres) and how far out it is along the ground (0 at the player, 1 at the shape's
		// farthest edge). Elsewhere 0.
		Time,
		Distance,
		Reach,
		BuiltinCount,
	};
	struct Step
	{
		Op op = Op::Const;
		VarKind kind = VarKind::Zero;
		BoardType type = BoardType::Int;
		uint16_t index = 0;
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
	// How the new state fades in over the crossfade (Godot's xfade_curve): its weight at even steps
	// of the fade from start to end, read between them in a straight line. Empty: linear.
	std::vector<float> curve;
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
	// How fast its clock runs (a TimeScale node in the state's blend tree, or a clip's custom
	// timeline): 1 is the clip's own speed, 0.5 half of it. An expression, read every tick, held in
	// [0, kMaxStateSpeed]. Empty: 1. Markers move with it: they sit on the clip's own clock.
	AnimExpr speed;
	std::vector<AnimGraphTransition> transitions; // in the order they are tried
	// The transitions of other states that lead here on a mod event ("pistol.fired"): while the
	// layer is already in this state, the event starts it over, with that transition's crossfade.
	// A shot fired during the recoil of the last one plays its recoil again, instead of being
	// ignored until the clip has ended. (Godot's state machine has no transition from a state to
	// itself, so this is derived, not authored.)
	std::vector<AnimGraphTransition> restarts;
};

struct AnimGraphLayer
{
	std::string name;
	std::vector<std::string> mask; // bones (humanoid-profile names) it covers; empty: every bone
	AnimExpr weight;			   // empty: 1
	int start = 0;
	// Added to what is below it instead of laid over it (Godot's Add2, a OneShot's Add mix mode):
	// its pose's difference from the skeleton's rest, by its weight, through its mask.
	bool additive = false;
	// A OneShot: its one state plays once from the tick `request` turns true (again: from the
	// start), fading in over `fadeIn` and out over the last `fadeOut` seconds; its weight is that
	// fade. Its layer state keeps whether it plays in previousBlend, and the request's last value in
	// previousBlendY (a one-state layer has no use for them).
	bool shot = false;
	AnimExpr request;
	float fadeIn = 0.0f;
	float fadeOut = 0.0f;
	std::vector<AnimGraphState> states;
};

struct AnimGraph
{
	std::vector<AnimGraphClip> clips;
	std::vector<AnimGraphLayer> layers;
	std::string text;					// what it was compiled from

	int FindClip( const std::string& name ) const;
	// True when a layer above the base reads that stance (its number in AnimState) or event (its
	// schema index) in a weight, a blend input or a transition.
	bool UpperLayersRead( AnimExpr::VarKind kind, int index ) const;
	// True when some clip's marker emits this schema event.
	bool EmitsEvent( int event ) const;
};

// Animation packs a server's mods provide (sim/mod_schema.h animPacks), compiled; index = source - 1.
using AnimGraphPacks = std::vector<std::shared_ptr<const AnimGraph>>;

// Whether a layer above the base reads that stance or event, the character's own or one any pack
// brings (a pack's layer of the base's name is the base, and is not counted): what the viewer's
// lead runs ahead for a prediction (present/anim_lead.h). An item's pack (the pistol's recoil on
// "item.use") is read as much as the character's own machine.
bool UpperLayersRead( const AnimGraph& character, const AnimGraphPacks& packs, AnimExpr::VarKind kind, int index );

// Overlays: after a character's own layers a player has this many more, for packs that bring a
// layer the character does not have (a dash, hanging on a line). Such a layer is laid over the
// whole body (or its own bone filter) with its own weight, which says when it shows
// ("motion.Dash"). An item's pack takes a slot for each such layer while the item is carried
// (AnimState::graph): the hook's hold for the left arm, and its hang for the whole body.
constexpr size_t kOverlaySlots = 4;
inline size_t LayerSlots( const AnimGraph& character )
{
	return character.layers.size() + kOverlaySlots;
}
// The `nth` layer of `pack` that the character does not have (its overlays, in the pack's order),
// or -1; and how many it has.
int OverlayLayerOf( const AnimGraph& character, const AnimGraph& pack, int nth = 0 );
int OverlayCount( const AnimGraph& character, const AnimGraph& pack );
// Which of its pack's overlays slot `l` of `graph` (AnimState::graph) plays: the slots of one pack
// are its overlays in order, so it is how many overlay slots before it play the same pack.
template <class Layers> int OverlayNth( const AnimGraph& character, const Layers& graph, size_t l )
{
	int nth = 0;
	for ( size_t k = character.layers.size(); k < l && k < graph.size(); ++k )
	{
		nth += graph[k].source == graph[l].source ? 1 : 0;
	}
	return nth;
}

// The layer that plays in slot `l` for `source` (0: its own; n: pack n-1's), and the graph it
// belongs to (its clips, markers). A character's own slot: the pack's layer of the same name, else
// the character's own. An overlay slot (past the character's layers): the pack's `nth` overlay
// (OverlayNth), else a layer of no states, which plays nothing.
const AnimGraphLayer& ResolveLayer( const AnimGraph& character, const AnimGraphPacks& packs, size_t l, uint8_t source,
									const AnimGraph*& owner, int nth = 0 );

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
// A curve an expression reads ("curve(Falloff, reach)"): its values at even steps from `min` to
// `max` (at least two), read between them in a straight line, held at its ends.
struct AnimCurve
{
	std::string name;
	float min = 0.0f;
	float max = 1.0f;
	std::vector<float> samples;
};
float SampleAnimCurve( const AnimCurve& curve, float x );

// The curves "curve(NAME, x)" may name while expressions are compiled on this thread (a motion's:
// sim/motions.cpp sets it); null: a curve is an error.
void SetAnimCurveNames( const std::vector<AnimCurve>* curves );

struct AnimGraphInputs
{
	float builtins[AnimExpr::BuiltinCount] = {};
	const AnimState* state = nullptr;
	const BoardValues* board = nullptr;		  // the player's (null: all 0)
	const BoardValues* globalBoard = nullptr; // the game's
	const ModEventRecord* events = nullptr;
	uint32_t eventCount = 0; // SimGlobals::modEventCount
	uint32_t tick = 0;
	// A motion's: an event also counts the tick after it was recorded late (after the motions had
	// run: an animation's marker), as a motion's trigger hears it. A state machine runs after the
	// markers, so it hears them on their own tick.
	bool hearLate = false;
	uint32_t netId = 0;
	// Kinds of the items the player holds (schema indices).
	// The player's input this tick, for "held.<action>" (motions; null for a state machine).
	const PlayerInput* input = nullptr;
	// What went down this tick, for "pressed.<action>": mod action bits, and the engine's buttons.
	ActionBits pressedActions = 0;
	uint8_t pressedButtons = 0;
	const uint16_t* heldKinds = nullptr;
	uint32_t heldCount = 0;
	// The player's motions that are on, a bit each ("motion.<name>"): for a state machine as this
	// tick's motions left them, for a motion's own conditions as the tick found them.
	uint64_t motionsOn = 0;
	// Kinds of everything the player has, the hand's among them ("carried.<kind>").
	const uint16_t* carriedKinds = nullptr;
	uint32_t carriedCount = 0;
	// Kinds it is using this tick ("using.<kind>").
	const uint16_t* usingKinds = nullptr;
	uint32_t usingCount = 0;
	// The curves "curve(...)" reads (the motion's own; null: 0).
	const std::vector<AnimCurve>* curves = nullptr;
};

float EvaluateAnimExpr( const AnimExpr& expr, const AnimGraphInputs& inputs, float stateTime );

// How much of each of a state's points plays at a blend input (x, and y for a 2D space); a single
// clip's only point gets 1. Weights sum to 1.
using AnimBlendWeights = std::array<float, kMaxBlendPoints>;
void AnimGraphWeights( const AnimGraphState& state, float x, float y, AnimBlendWeights& out );

// A state's speed this tick (AnimGraphState::speed).
float AnimStateSpeed( const AnimGraphState& state, const AnimGraphInputs& inputs, float stateTime );

// How much of the layer's state shows over the one it fades from, 0 to 1: the crossfade so far,
// through the curve of the transition that started it (linear without one).
float AnimGraphFadeIn( const AnimGraphLayer& layer, const AnimGraphLayerState& L );

// Advances the graph's layers in `state` by one tick (a layer not started yet begins in its start
// state). Markers crossed append their schema event
// indices to `markers`.
void UpdateAnimGraph( AnimState& state, const AnimGraph& graph, const AnimGraphPacks& packs, AnimGraphInputs& inputs, float dt,
					  std::vector<int>& markers );

// Parses a decimal number the same way on every machine (not the C library's, which depends on the
// locale and the platform's rounding): graph.cfg and expressions feed the simulation.
bool ParseAnimFloat( const char* text, size_t length, float& out );

} // namespace cb
