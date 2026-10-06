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
//               is on from the moment it holds until what it holds on to is gone
//   impulse     a change of a target's velocity along a frame, added or replacing a part of it
//   change      board fields of the player: dash.charges -= 1 (in a while motion, += and -= are per
//               second and = is set when it starts)
//   emit        a mod event at the player: looks react to it, state machines enter on it, server
//               mods hear it
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
	};
	Kind kind = Kind::Self;
	uint8_t slot = 0; // Kind::Field: the board slot
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
};

struct MotionEffect
{
	enum class Kind : uint8_t
	{
		Impulse = 0, // once, when the motion starts
		Force = 1,	 // every tick it is on
		Link = 2,	 // every tick it is on
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
	bool react = false; // Force: the other end (the player, or what the probe found) is pushed back
	// Link. The rope's length in metres; 0: the distance when the motion's probe takes hold.
	float length = 0.0f;
	float reel = 0.0f; // metres of rope taken in a second
};

struct Motion
{
	enum class When : uint8_t
	{
		Press = 0, // `action` goes down
		While = 1, // every tick `condition` holds
		Event = 2, // the mod event `trigger` was recorded at the player this tick
	};
	enum class ChangeOp : uint8_t
	{
		Set = 0,
		Add = 1,
		Sub = 2,
	};
	struct Change
	{
		uint8_t slot = 0; // board slot of the player's field
		BoardType type = BoardType::Int;
		ChangeOp op = ChangeOp::Set;
		float value = 0.0f;
	};
	struct Param
	{
		uint8_t param = 0; // MoveParam
		float value = 0.0f;
	};

	std::string name; // "dash.moves/Dash": the set and the node
	When when = When::Press;
	int action = -1;  // When::Press. -1: no mod declared it, so it never happens
	int trigger = -1; // When::Event: the schema event. -1: no mod declared it, so it never happens
	AnimExpr condition;
	float cooldown = 0.0f;
	uint32_t uses = 0;			 // 0: no limit
	bool refillOnGround = false; // uses come back when the player stands
	float refillSeconds = 0.0f;	 // ... or this long after the last use (0: never by time)
	float duration = 0.0f;
	AnimExpr until; // ends it; with one (or a probe) a press keeps it on until then
	std::vector<Param> params;
	std::vector<Change> changes;
	int event = -1; // schema event, -1: none (or no mod declared it)
	// A probe (When::Press and When::Event): thrown when the motion happens.
	bool probe = false;
	float probeRange = 0.0f;  // metres the line reaches
	float probeTravel = 0.0f; // m/s it flies at; 0: it holds at once
	std::vector<MotionEffect> effects; // in the order of the nodes

	// On from when it starts until something ends it (its condition, what its probe holds going away).
	bool Held() const
	{
		return when != When::While && ( probe || until.Empty() == false );
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
	uint8_t index = 0;
	bool started = false; // this is the tick it took effect: impulses happen now
};

// Decides the player's motions for one tick, before the mover: which start, which go on, which
// end. Changes `board` and appends the events they emit (`point` and `tick` are the caller's to
// fill in); `boardChanged` is set when a field was written. `active` gets the motions whose
// effects apply this tick, in order.
void RunMotions( const Motions& motions, const MotionInputs& in, MotionState& state, const Character& c, Blackboard& board,
				 bool& boardChanged, std::vector<ModEventRecord>& events, std::vector<MotionActive>& active );

// Lays the movement parameters of the motions that are on at `tick` over `params`, in order.
void ApplyMotionParams( const Motions& motions, const MotionState& state, uint32_t tick, MoveParams& params );

// A frame's direction for a player (unit length). MotionFrame::To is the caller's: it is toward a
// target only the simulation can find.
b3Vec3 MotionDirection( MotionFrame frame, b3Vec3 world, const Character& c, const PlayerInput& in );

// How much of a force is there `ticksOn` ticks after its motion took effect: 0 to 1.
float MotionRamp( const MotionEffect& effect, uint32_t ticksOn, uint32_t tickRate );

} // namespace cb
