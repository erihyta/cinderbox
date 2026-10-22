#pragma once

// Motions: what a mod adds to how players move (a dash, a double jump, flight, a grappling hook),
// run by the simulation.
//
// A mod's rules run on the server, and what they decide reaches a client a round trip later, as
// commands. That is too late for movement: a dash has to start on the tick of the press, in the
// player's own prediction. So a motion is not a rule that runs somewhere: it is data every
// simulation runs, the way a character's state machine is (anim_graph.h). It is authored in Godot
// (CbMotion nodes), baked to text (motions/<set>.cfg in the mod's item), read by the server, sent
// to every client in the schema, and compiled against it. The input is what a client already
// simulates ahead, so a motion is predicted and rolled back like walking is.
//
// A motion is a trigger and what it does.
//
// The trigger:
//   when        press: an action goes down, one a mod declared ("dash") or the engine's "jump" /
//               "sprint". while: for as long as its condition holds (flight, a glide). event: a mod
//               event recorded at the player this tick (a server mod's Emit: a stun)
//   if          a condition, in the expression language, over what a state machine reads (speed,
//               grounded, airborne_time, vertical_speed, board fields, item kinds, stances, events)
//               and held.<action>, pressed.<action>, linked
//   cooldown    seconds between two uses
//   uses        how many before a refill: on the ground, or some seconds after the last use
//   duration    how long a press or an event keeps it on
//   until       a condition that ends it; with one, a press keeps it on until the condition holds
//               (pressed.grapple: the next press)
//
// While it is on:
//   param       movement parameters the player has (friction 0, airborne 1)
//   force       a lasting push on a target: an acceleration, a force (divided by the target's mass)
//               or a velocity to approach, along a frame, ramped in over some seconds. It can react:
//               the other end is pushed the other way with the same momentum
//   link        a rope from the player to a target: neither end goes further out than its length.
//               What the rope takes from one it gives the other, by their masses
//
// When it starts:
//   probe       a line along the look at what is under the crosshair, up to a range. It flies there
//               at a speed and takes hold. The motion happens only if the line finds something, and
//               is on from the moment it holds until what it holds on to is gone. It may hold only
//               some things (holds: level, bodies, players); anything else stops it
//   impulse     a change of a target's velocity along a frame, added or replacing a part of it
//   change      board fields of the player: dash.charges -= 1 (in a while motion, += and -= are per
//               second and = is set when it starts)
//   emit        a mod event at the player: looks react to it, state machines enter on it, server
//               mods hear it
//
// Steps (a rules sheet's motion block: docs/rules-language.md):
//   step        what comes after the step before: it starts once its wait is over (`after` seconds
//               since the step before started, then `when` its condition holds), at the earliest the
//               tick after. With `within`, a wait that takes longer gives up: the step's timeout step
//               runs (once statements only) and the motion ends; without one, it just ends
//   guard       the statements below it count only if its condition held when the step started (a
//               sheet's if / else), decided once, so a lasting one holds for the whole step
//   end         the motion ends after this tick
// A motion with steps is on until it ends: its `duration`, its `until`, an `end`, a timeout, or
// its last step's longest `for` (a move's seconds) run out. Without steps it is as it always was.
// Its once statements (impulses, launches, changes, emits) happen when their step starts; its
// lasting ones (forces, links, parameters) act while it is the step (a parameter: for its seconds).
//
// A target is who an effect acts on: the player itself, what the motion's probe found, or the
// entity a field of the player names (a NetId a server mod wrote there: who it is bound to).
//
// The rules stay the mod's: it declares the action and the fields, and decides who may (a field
// the condition reads, an item the player has to hold). The motion is the mechanism it switches on.

#include "anim_graph.h"
#include "components.h"
#include "mod_schema.h"
#include "move_params.h"

#include <memory>
#include <string>
#include <vector>

namespace cb
{

// Who an effect acts on.
struct MotionTarget
{
	enum class Kind : uint8_t
	{
		Self = 0,  // the player the motion runs for
		Hit = 1,   // what the motion's probe found (nothing, when that is the world)
		Field = 2, // the entity whose NetId is in a field of the player (0, or gone: nobody)
		Found = 3, // inside a find: each thing it found, in turn
	};
	Kind kind = Kind::Self;
	uint16_t slot = 0; // Kind::Field: the board slot
	bool known = true; // false: the field is not one a mod declares, so the effect does nothing
};

// A direction, as the player has it. Every effect's is the player's own, whoever it acts on.
enum class MotionFrame : uint8_t
{
	Look = 0,	// where the camera looks, pitch included
	Move = 1,	// where the movement input points (camera-relative); the facing when there is none
	Facing = 2, // where the body faces
	Up = 3,
	World = 4, // `direction`, as given
	To = 5,	   // from the player toward the target (what the probe found; the entity a field names)
	Away = 6,  // inside a find: from where the player is out to what it found, along the ground
	Marker = 7, // `direction` as drawn in the scene (a marker's forward), turned with the look
};

struct MotionEffect
{
	enum class Kind : uint8_t
	{
		Impulse = 0, // once, when the motion starts
		Force = 1,	 // every tick it is on
		Link = 2,	 // every tick it is on
		Launch = 3,	 // once, when the motion starts: an item of a kind is thrown into the world
		Follow = 4,	 // every tick it is on: the player is carried along a path (Motion::paths)
	};
	enum class Replace : uint8_t
	{
		None = 0,		// the impulse is added to the velocity
		Vertical = 1,	// the vertical speed is cleared first (a jump in the air is the same jump)
		Horizontal = 2, // the horizontal velocity is cleared first
		All = 3,
	};
	enum class Push : uint8_t
	{
		Acceleration = 0, // `strength` m/s^2, whatever the target weighs
		Force = 1,		  // `strength` newtons: a heavy target answers slowly
		Velocity = 2,	  // toward `speed` m/s along the frame, at up to `strength` m/s^2
	};

	Kind kind = Kind::Impulse;
	MotionTarget target;
	MotionFrame frame = MotionFrame::Move;
	b3Vec3 direction = { 0.0f, 1.0f, 0.0f }; // MotionFrame::World
	// Impulse: m/s. Force: see Push.
	float strength = 0.0f;
	Replace replace = Replace::None;
	Push push = Push::Acceleration;
	// Force. Acceleration, Force: the speed along the frame past which it pushes no more (0: none).
	// Velocity: the speed it brings the target to.
	float speed = 0.0f;
	float ramp = 0.0f;	// Force: seconds from nothing to full strength
	// Force: the other end (the player, or what the probe found) is pushed back. Link: the rope pulls
	// both ends by their weights; off, the player alone (what it holds is as if it were the world).
	bool react = false;
	// Link. The rope's length in metres; 0: the distance when the motion's probe takes hold.
	float length = 0.0f;
	float reel = 0.0f; // metres of rope taken in a second
	// Launch. The item kind thrown (schema index; `known` false: no mod declares it, nothing is
	// thrown), at `strength` m/s along the frame with `lift` m/s upward added and the player's own
	// velocity, from `ahead` metres in front of where its look starts. It is removed after
	// `seconds` (0: it stays, like anything dropped).
	uint16_t itemKind = 0;
	bool known = true;
	// Not a new one: the copy of that kind the player carries (the one in a hand first, else the
	// lowest slot's), its fields and all; none carried, nothing is thrown.
	bool carried = false;
	float lift = 0.0f;
	float ahead = 0.7f;
	float seconds = 0.0f;
	// The step's guard it is under (MotionStep::guards), -1: none.
	int8_t guard = -1;
	// Impulse, Force, Launch: its strength (a launch's speed) as an expression, read when it acts
	// (Motion::exprs; -1: `strength`).
	int16_t strengthExpr = -1;
	// Its direction turned after its frame gives it: by a heading (right is positive) and a pitch (up
	// is positive), as cosines and sines worked out once, the same on every machine.
	bool turned = false;
	float yawCos = 1.0f, yawSin = 0.0f, pitchCos = 1.0f, pitchSin = 0.0f;
	// Launch: where it starts, a marker's place in the player's frame (its feet, forward +Z), turned
	// with its heading; without one, ahead of where its look starts.
	bool fromMarker = false;
	b3Vec3 from = {};
	// Follow: the path (Motion::paths), over `seconds`.
	int16_t path = -1;
	// Impulse: `strength` is momentum (N s), shared out by what the target weighs, not a change of
	// velocity whatever it weighs.
	bool momentum = false;
};

// A direction turned as an effect says (MotionEffect::turned).
b3Vec3 MotionTurn( const MotionEffect& effect, b3Vec3 direction );

// What a probe may hold on to: the level (anything static: map pieces, a static entity, a wall that
// breaks), bodies (props, items lying about, rubble, ragdolls), players.
inline constexpr uint8_t kProbeHoldsLevel = 1;
inline constexpr uint8_t kProbeHoldsBodies = 2;
inline constexpr uint8_t kProbeHoldsPlayers = 4;
inline constexpr uint8_t kProbeHoldsAll = 7;

// A motion slot's untilTick for one that lasts until something ends it.
inline constexpr uint32_t kForever = 0xFFFFFFFFu;

// How many guards (a sheet's if / else) a step may have: a bit each in its slot.
inline constexpr int kMotionGuards = 16;
// How many steps a motion may have.
inline constexpr int kMotionSteps = 64;

// A find (a sheet's "find SHAPE"): everything the shape overlaps when its step starts, each in turn,
// nearest first. Its shape is in the player's frame (its feet, forward +Z), turned as `frame` says.
struct MotionFind
{
	// A convex piece of the shape: points (a sphere one, a capsule two, a box eight, a hull its own)
	// and a radius around them.
	struct Piece
	{
		std::vector<b3Vec3> points;
		float radius = 0.0f;
	};
	std::vector<Piece> pieces;
	MotionFrame frame = MotionFrame::Facing; // Facing, Look (with the pitch) or World (not turned)
	uint8_t who = kProbeHoldsPlayers | kProbeHoldsBodies; // players, bodies (kProbeHolds bits)
	bool self = false;								   // the player itself may be found
	uint8_t most = 16;								   // at most this many, the nearest
	int8_t guard = -1;
	// What happens to each: impulses (on what it found, unless they say), emits (with what it found,
	// at it), and movement parameters given to a player it found for their seconds.
	std::vector<MotionEffect> effects;
	std::vector<int> emits;
	struct Move
	{
		uint8_t param = 0;
		float value = 0.0f;
		float seconds = 0.0f;
	};
	std::vector<Move> moves;
	// Changes of the owner's fields, once for each thing found (a count of what was hit).
	struct Change
	{
		uint16_t slot = 0; // board slot of the owner's field
		BoardType type = BoardType::Int;
		uint8_t op = 0; // MotionStep::ChangeOp: set, add, subtract
		float value = 0.0f;
	};
	std::vector<Change> changes;
	// How far out along the ground its shape reaches from the player (its points and radius): what
	// "reach" is a share of.
	float extent = 0.0f;
};
inline constexpr int kFindMost = 16;

// A number a statement says: as written, or an expression read when it acts (Motion::exprs).
struct MotionValue
{
	float number = 0.0f;
	int16_t expr = -1;
};

struct MotionStep
{
	enum class ChangeOp : uint8_t
	{
		Set = 0,
		Add = 1,
		Sub = 2,
	};
	struct Change
	{
		uint16_t slot = 0; // board slot of the player's field
		BoardType type = BoardType::Int;
		ChangeOp op = ChangeOp::Set;
		float value = 0.0f;
		int8_t guard = -1;
	};
	struct Param
	{
		uint8_t param = 0; // MoveParam
		float value = 0.0f;
		float seconds = 0.0f; // from when the step starts; 0: while it is the step
		int8_t guard = -1;
	};
	struct Emit
	{
		int event = -1; // schema event; -1: no mod declared it (nothing is emitted)
		int8_t guard = -1;
	};

	// How it is reached (not the first step): `after` seconds since the step before started, then
	// when `wait` holds. `within`: seconds after which the wait gives up (0: never).
	AnimExpr wait;
	float after = 0.0f;
	float within = 0.0f;
	bool timeout = false; // the step a wait that gave up runs: once statements, then the motion ends
	int next = -1;		  // the step after this one (-1: the last)
	int onTimeout = -1;	  // this step's timeout step, when its wait gives up (-1: the motion ends)
	float length = 0.0f;  // its longest parameter's seconds: how long a last step keeps the motion on

	std::vector<AnimExpr> guards; // decided when the step starts: a bit each in the slot
	std::vector<Param> params;
	std::vector<Change> changes;
	std::vector<Emit> emits;
	std::vector<int8_t> ends;		   // an "end" each, under a guard (-1: none)
	std::vector<MotionEffect> effects; // in the order of the nodes
	std::vector<MotionFind> finds;	   // after its effects
	// Walls broken where the owner is (a sheet's "break"): those within `radius` (metres) lose the
	// pieces that near, flung at `speed`, staying `seconds` (0: for good).
	struct Break
	{
		MotionValue radius, speed, seconds;
		int8_t guard = -1;
	};
	std::vector<Break> breaks;
	std::vector<int8_t> removes; // an item's "remove self" each, under a guard
	// What it does again and again (a sheet's "every S": the lines after it): once when the step
	// starts and every `every` seconds while it is the step, 64 times at most. Its once statements
	// only (changes, emits, impulses, launches, finds, breaks); its guards are this step's.
	float every = 0.0f;
	std::vector<MotionStep> repeats; // empty, or one
};
inline constexpr uint32_t kMotionRepeats = 64;
inline constexpr float kMotionMomentumLimit = 100000.0f; // N s

// Whether a step's repeat happens on `tick` (its step started at `stepStart`).
bool MotionRepeatsNow( const MotionStep& step, uint32_t stepStart, uint32_t tick, uint32_t tickRate );

struct Motion
{
	enum class When : uint8_t
	{
		Press = 0, // `action` goes down
		While = 1, // every tick `condition` holds
		Event = 2, // the mod event `trigger` was recorded at the player this tick
		Edge = 3,  // once, on the tick `edge` turns true (it was false on the tick before)
	};
	using ChangeOp = MotionStep::ChangeOp;
	using Change = MotionStep::Change;
	using Param = MotionStep::Param;

	std::string name; // "dash.moves/Dash": the set and the node
	When when = When::Press;
	int action = -1;  // When::Press. -1: no mod declared it, so it never happens
	int trigger = -1; // When::Event: the schema event. -1: no mod declared it, so it never happens
	AnimExpr edge;	  // When::Edge
	AnimExpr condition;
	float cooldown = 0.0f;
	uint32_t uses = 0;			 // 0: no limit
	bool refillOnGround = false; // uses come back when the player stands
	float refillSeconds = 0.0f;	 // ... or this long after the last use (0: never by time)
	float duration = 0.0f;
	AnimExpr until; // ends it; with one (or a probe) a press keeps it on until then
	// A probe (When::Press and When::Event): thrown when the motion happens.
	bool probe = false;
	float probeRange = 0.0f;  // metres the line reaches
	float probeTravel = 0.0f; // m/s it flies at, traced tick by tick (a projectile); 0: it holds at once
	int holdEvent = -1;		  // schema event said where and when the probe takes hold; -1: none
	// What it may hold on to (kProbeHolds bits). Something else in its way stops it: it holds nothing.
	uint8_t probeHolds = kProbeHoldsAll;
	// Its shape (a sheet's cast of a shape): points around the line's tip in the frame of the line
	// (forward +Z), and a radius around them. None: a line (a ray).
	std::vector<b3Vec3> probePoints;
	float probeRadius = 0.0f;
	// What it does, step by step: the first is what happens when it starts. Never empty.
	std::vector<MotionStep> steps = std::vector<MotionStep>( 1 );
	// The curves its expressions read ("curve(Falloff, reach)"), and the expressions its effects'
	// strengths are (MotionEffect::strengthExpr).
	std::vector<AnimCurve> curves;
	std::vector<AnimExpr> exprs;
	// The paths its follows carry the player along: points at even steps along them from where it
	// starts (0, 0, 0), in the player's frame (forward +Z).
	struct Path
	{
		std::string name;
		std::vector<b3Vec3> points;
	};
	std::vector<Path> paths;
	// Run by an item lying in the world, not by players (a sheet's "owner item"): its kind (schema
	// index; -1: the set is no item's, and it never runs). One step, once, when its event is said at
	// the item (its own expiry: "<kind>.expired").
	bool ownerItem = false;
	int itemKind = -1;

	// On from when it starts until something ends it (its condition, what its probe holds going away,
	// a wait for its next step).
	bool Held() const
	{
		return when != When::While && ( probe || until.Empty() == false || steps.size() > 1 );
	}
	// What it does when it starts (and all it does, without steps).
	const MotionStep& First() const
	{
		return steps.front();
	}
};

// Every motion of a server, in the order of its sets and of the nodes in them: at most kMaxMotions
// (components.h MotionState has a slot for each).
struct Motions
{
	std::vector<Motion> list;
};

// Compiles one set's text against the schema, appending to `out`. An action, a field or an event
// that no mod declares is listed in `warnings` and does nothing (the motion never happens, the
// change is skipped, nothing is emitted); a malformed file fails with `error`. The editor's bake
// calls it with an empty schema to check the text.
bool CompileMotionSet( const std::string& set, const std::string& text, const ModSchema& schema, std::vector<Motion>& out,
					   std::string& error, std::string& warnings );

// The names of a schema's motions, in the order CompileMotions gives them ("dash.moves/Dash"): what
// "motion.<name>" in a condition is looked up in.
std::vector<std::string> MotionNames( const ModSchema& schema );
// The place of the motion a condition names ("dash.moves.Dash", or "Dash" when one motion has that
// node name). -1: none; -2: several have the node name.
int FindMotionByName( const ModSchema& schema, const std::string& name );
// True while MotionNames is at work (a condition compiled then is not to look a motion up).
bool ListingMotionNames();
// The motions of a player that are in effect at `tick`, a bit each (the first 64).
uint64_t MotionsOnAt( const MotionState& state, uint32_t tick );

// Every set of a schema. A set that does not compile is left out; motions past kMaxMotions too.
// Null when there are none.
std::shared_ptr<const Motions> CompileMotions( const ModSchema& schema, std::string& warnings );

// What one tick of motions needs to know about a player, besides its components.
struct MotionInputs
{
	const PlayerInput* input = nullptr;
	uint8_t pressedButtons = 0; // engine buttons that went down this tick
	bool canAct = true;			// false: frozen. Nothing starts, but held keys are remembered
	uint32_t netId = 0;			// the player's: whose events a When::Event motion answers
	uint32_t tick = 0;
	uint32_t tickRate = 60;
	// What conditions read (the same values a state machine reads, as they are before this tick's
	// movement).
	const AnimGraphInputs* values = nullptr;
	// The motion whose probe holds on to something for this player (-1: none), and whether what it
	// holds on to is still there. The simulation's: it owns the ray and the hold.
	int held = -1;
	bool holdAlive = false;
	// Throws a motion's probe for this player: false when the line finds nothing (the motion then
	// does not happen). `holdTick` is when it takes hold.
	bool ( *attach )( void* user, const Motion& motion, size_t index, uint32_t& holdTick ) = nullptr;
	void* user = nullptr;
};

// A motion that is on this tick, for the simulation to apply its effects.
struct MotionActive
{
	uint16_t index = 0;
	bool started = false; // this is the tick its step took effect: impulses happen now
};

// The tick a motion's current step took effect (the first: when the motion did, or its probe held).
uint32_t MotionStepStart( const MotionSlot& slot );
// Whether a statement under `guard` counts in the slot's step.
bool MotionGuardHolds( const MotionSlot& slot, int8_t guard );
// A step's guards as they are now: a bit each (what a step's start decides).
uint16_t MotionGuardBits( const MotionStep& step, const AnimGraphInputs& values );
// A statement's number now, held within lo and hi.
float MotionValueOf( const Motion& m, const MotionValue& value, const AnimGraphInputs& values, float lo, float hi );
// An effect's strength now: its number, or its expression read against `values`, held within `limit`.
float MotionStrength( const Motion& m, const MotionEffect& effect, const AnimGraphInputs& values, float limit );
// Limits of an effect's strength: an impulse's m/s, a force's newtons or m/s^2.
inline constexpr float kMotionImpulseLimit = 200.0f;
float MotionForceLimit( const MotionEffect& effect );

// Decides the player's motions for one tick, before the mover: which start, which go on, which
// end. Changes `board` and appends the events they emit (`point` and `tick` are the caller's to
// fill in); `boardChanged` is set when a field was written. `active` gets the motions whose
// effects apply this tick, in order.
void RunMotions( const Motions& motions, const MotionInputs& in, MotionState& state, const Character& c, Blackboard& board,
				 bool& boardChanged, std::vector<ModEventRecord>& events, std::vector<MotionActive>& active );

// A find's change of its owner's field (once for each thing found), on `board`.
void ApplyFindChange( const MotionFind::Change& change, Blackboard& board );

// Lays the movement parameters of the motions that are on at `tick` over `params`, in order.
void ApplyMotionParams( const Motions& motions, const MotionState& state, uint32_t tick, uint32_t tickRate, MoveParams& params );

// A frame's direction for a player (unit length). MotionFrame::To is the caller's: it is toward a
// target only the simulation can find. World and Marker take `world`: as it is, or turned with the look.
b3Vec3 MotionDirection( MotionFrame frame, b3Vec3 world, const Character& c, const PlayerInput& in );

// Where a path is a share `u` (0 to 1) of the way along it.
b3Vec3 MotionPathAt( const Motion::Path& path, float u );

// How much of a force is there `ticksOn` ticks after its motion took effect: 0 to 1.
float MotionRamp( const MotionEffect& effect, uint32_t ticksOn, uint32_t tickRate );

} // namespace cb
