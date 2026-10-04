#pragma once

// Motions: what a mod adds to how players move (a dash, a double jump), run by the simulation.
//
// A mod's rules run on the server, and what they decide reaches a client a round trip later, as
// commands. That is too late for movement: a dash has to start on the tick of the press, in the
// player's own prediction. So a motion is not a rule that runs somewhere: it is data every
// simulation runs, the way a character's state machine is (anim_graph.h). It is authored in Godot
// (CbMotion nodes), baked to text (motions/<set>.cfg in the mod's item), read by the server, sent
// to every client in the schema, and compiled against it. A press is already in the input a
// client simulates ahead, so a motion is predicted and rolled back like walking is.
//
//   when        press: an action goes down, one a mod declared ("dash") or the engine's "jump" /
//               "sprint". while: for as long as its condition holds (flight, a glide, a jetpack).
//               event: a mod event recorded at the player this tick (a server mod's Emit: a stun)
//   if          a condition, in the expression language, over what a state machine reads: speed,
//               grounded, airborne_time, vertical_speed, board fields, item kinds, stances, events
//   cooldown    seconds between two uses
//   uses        how many before a refill: on the ground, or some seconds after the last use
//   impulse     a change of velocity along the look, the move input, the facing, up or a fixed
//               direction, added to the velocity or replacing its vertical / horizontal part / all.
//               While: metres per second, every second it is on (a thrust)
//   duration    how long it stays on: its movement parameters hold that long (a dash without
//               friction). A while motion is on while its condition holds
//   change      board fields of the player: dash.charges -= 1. While: += and -= are per second
//               (jetpack.fuel -= 20), and = is set when it starts
//   emit        a mod event at the player: looks react to it, state machines enter on it, server
//               mods hear it
//   tether      a line thrown along the look at what is under the crosshair, up to a range: it
//               flies there at a speed, then pulls the player toward the point and, with a rope,
//               keeps it within the rope's length (a swing), reeling the rope in. The motion
//               happens only if the line finds something. A point on a prop pulls the prop too
//   until       what lets the tether go (not held.grapple, or pressed.grapple for a second press);
//               its parameters hold while it is out. Conditions can ask "tethered"
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

struct Motion
{
	enum class When : uint8_t
	{
		Press = 0, // `action` goes down
		While = 1, // every tick `condition` holds
		Event = 2, // the mod event `trigger` was recorded at the player this tick
	};
	enum class Frame : uint8_t
	{
		Look = 0,	// where the camera looks, pitch included
		Move = 1,	// where the movement input points (camera-relative); the facing when there is none
		Facing = 2, // where the body faces
		Up = 3,
		World = 4, // `direction`, as given
	};
	enum class Replace : uint8_t
	{
		None = 0,		// the impulse is added to the velocity
		Vertical = 1,	// the vertical speed is cleared first (a jump in the air is the same jump)
		Horizontal = 2, // the horizontal velocity is cleared first
		All = 3,
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
	float impulse = 0.0f;
	Frame frame = Frame::Move;
	Replace replace = Replace::None;
	b3Vec3 direction = { 0.0f, 1.0f, 0.0f }; // Frame::World
	std::vector<Param> params;
	std::vector<Change> changes;
	int event = -1; // schema event, -1: none (or no mod declared it)
	// A tether (When::Press and When::Event): thrown when the motion happens.
	bool tether = false;
	float tetherRange = 0.0f;  // metres the line reaches
	float tetherTravel = 0.0f; // m/s it flies at; 0: it holds at once
	float tetherPull = 0.0f;   // m/s^2 toward the point while it holds
	float tetherReel = 0.0f;   // metres of rope taken in a second
	bool tetherRope = false;   // the distance when it takes hold is a rope's length
	AnimExpr tetherUntil;	   // lets it go when it holds; empty: only what it holds on to going away does
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
	bool canAct = true;			// false: frozen. Nothing happens, but held keys are remembered
	uint32_t netId = 0;			// the player's: whose events a When::Event motion answers
	uint32_t tick = 0;
	uint32_t tickRate = 60;
	// What conditions read (the same values a state machine reads, as they are before this tick's
	// movement).
	const AnimGraphInputs* values = nullptr;
	// Throws a motion's tether for this player: false when the line finds nothing (the motion then
	// does not happen). The simulation's: it owns the ray and the component.
	bool ( *attach )( void* user, const Motion& motion, size_t index ) = nullptr;
	void* user = nullptr;
};

// Runs the player's motions for one tick, before the mover: the ones whose press and condition
// hold change `c`'s velocity and `board`, and append the events they emit (`point` and `tick` are
// the caller's to fill in). `boardChanged` is set when a field was written.
void RunMotions( const Motions& motions, const MotionInputs& in, MotionState& state, Character& c, Blackboard& board, bool& boardChanged,
				 std::vector<ModEventRecord>& events );

// Lays the movement parameters of the motions that are on at `tick` over `params`, in order.
void ApplyMotionParams( const Motions& motions, const MotionState& state, uint32_t tick, MoveParams& params );

} // namespace cb
