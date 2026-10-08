#pragma once

// Simulation components. These are snapshotted as raw bytes for rollback and hashing, so:
// - trivially copyable, no pointers (Box3D ids are plain indices and are fine),
// - no implicit padding (padding bytes are not guaranteed to be copied, which would corrupt the hash).
// Every component must be registered in Simulation::RegisterComponents().

#include "small_list.h"
#include "types.h"

#include "box3d/id.h"
#include "box3d/math_functions.h"

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace cb
{

// Stable identity across server, clients and rollbacks. Never use flecs entity ids for this.
struct NetId
{
	uint32_t value = 0;
};

struct Transform
{
	b3Vec3 position = { 0.0f, 0.0f, 0.0f };
	b3Quat rotation = { { 0.0f, 0.0f, 0.0f }, 1.0f };
};

struct Velocity
{
	b3Vec3 linear = { 0.0f, 0.0f, 0.0f };
	b3Vec3 angular = { 0.0f, 0.0f, 0.0f };
};

enum class ShapeKind : uint8_t
{
	Box = 0,
	Sphere = 1,
	Capsule = 2,
};

// Collision and presentation geometry.
// Box: halfExtents. Sphere: x = radius. Capsule: x = radius, y = half distance between centers (along Y).
struct Shape
{
	ShapeKind kind = ShapeKind::Box;
	uint8_t reserved[3] = {};
	b3Vec3 halfExtents = { 0.5f, 0.5f, 0.5f };
};

struct PhysicsBody
{
	b3BodyId body = {};
	b3ShapeId shape = {};
};

struct Character
{
	b3Vec3 velocity = { 0.0f, 0.0f, 0.0f };
	float pogoVelocity = 0.0f;
	float facingYaw = 0.0f;
	uint8_t slot = 0;
	uint8_t grounded = 0;
	uint8_t prevButtons = 0;
	uint8_t sprinting = 0;
	// Ticks since the character last left the ground (0 while grounded). Drives jump/fall/land.
	uint32_t airTicks = 0;
	// Ticks since landing. Lets presentation play a land clip deterministically.
	uint32_t groundTicks = 0;
	// Tick of the most recent jump, 0 if never.
	uint32_t lastJumpTick = 0;
	// Distance walked since the last footstep, and how many steps this character has taken.
	// Presentation plays a step whenever the count changes.
	float stepDistance = 0.0f;
	uint32_t stepCount = 0;
	// Set by a Kill command, cleared by Respawn. A dead character takes no input, has its body
	// disabled, and is not drawn (its ragdoll, if it left one, is a separate entity).
	uint8_t dead = 0;
	// Set by a Freeze command: movement and jump inputs are ignored (between rounds, in a cutscene).
	uint8_t frozen = 0;
	// Set by a Facing command: the body faces where the camera looks (a shooter's stance) instead
	// of turning toward where it walks (freelook, the default).
	uint8_t faceCamera = 0;
	uint8_t reserved = 0;
	// Times this character fell below the kill plane and was put back. Mods watch it to count the
	// fall as a death; the engine only rescues the character.
	uint32_t fallCount = 0;
};

struct Prop
{
	uint32_t owner = 0; // NetId of the spawning player, 0 for level props
	uint32_t spawnTick = 0;
	uint32_t despawnTick = 0; // 0 = never
};

// What the body is doing between the ground and the air. The game's "jumped" and "landed" cues are
// its changes; a character's pose is its own state machine's business.
enum class AnimMode : uint8_t
{
	Locomotion = 0, // on the ground
	JumpStart = 1,
	Fall = 2,
	Land = 3,
};

// One layer of a character's baked state machine (sim/anim_graph.h), for characters that have one.
struct AnimGraphLayerState
{
	uint8_t state = 0;
	uint8_t previous = 0; // faded out over the first fadeLength seconds of `state`
	uint8_t started = 0;  // 0 until the graph first runs (a new or respawned player)
	uint8_t source = 0;	  // 0: the character's own layer; n: animation pack n-1's layer of that name
	float time = 0.0f;		   // seconds into the state's clip, or the phase [0, 1) of a blend space
	float previousTime = 0.0f; // the same for `previous`, still advancing while it fades
	float stateTime = 0.0f;	   // seconds since `state` started
	float fadeLength = 0.0f;
	float weight = 0.0f;		// the layer's weight, eased toward its weight expression
	float blend = 0.0f;			// a blend space's input this tick (x of a 2D one)
	float previousBlend = 0.0f; // and the fading state's
	float blendY = 0.0f;		// a 2D blend space's y
	float previousBlendY = 0.0f;
};

// A player's animation, as the simulation decides it: what its state machine reads (how it moves,
// where it looks, the stances mods set) and where each layer of the machine is. Poses are sampled
// from it with ozz, on clients and on the server's hit tests alike. Times are in seconds and
// independent of clip data, so the simulation never depends on asset files.
struct AnimState
{
	AnimMode mode = AnimMode::Locomotion;
	// Set by an Aim command (a mod: "the pistol is out"): the pose turns the character's aim chain
	// toward aimYaw / aimPitch, on every client and on the server's hit tests alike.
	uint8_t aiming = 0;
	// The legs walk backwards: moving away from where it faces.
	uint8_t legsBackward = 0;
	// How much the upper body follows the camera's pitch, 0 to 255: it rises while the character
	// faces the camera (Character::faceCamera, a mod's FaceCamera) and falls back in freelook, so
	// drawing a weapon while looking down bows the character over a moment instead of at once. The
	// pose bends the character's look chain by aimPitch times this.
	uint8_t look = 0;
	float modeTime = 0.0f;	  // seconds since `mode` started
	float groundSpeed = 0.0f; // smoothed horizontal speed (m/s)
	float aimYaw = 0.0f;	  // where the player looks, relative to the body's facing (radians, [-pi, pi))
	float aimPitch = 0.0f;	  // radians, up is positive
	// How far the hips turn from the body's facing toward the direction of travel, in [-pi/2, pi/2]
	// (the spine turns back, so the upper body keeps facing). Zero when walking straight ahead.
	float legYaw = 0.0f;
	// Per layer: the stance a mod set (0 = none). A state machine reads them by name ("pistol").
	SmallList<uint8_t, 4> stances;
	// Smoothed ground velocity in the body's frame (m/s): along its facing, and to its right. Blend
	// spaces of directional clips (strafing) read them.
	float moveForward = 0.0f;
	float moveRight = 0.0f;
	// The state machine's layers.
	SmallList<AnimGraphLayerState, 4> graph;

	bool operator==( const AnimState& ) const = default;

	size_t Layers() const
	{
		return stances.size() > graph.size() ? stances.size() : graph.size();
	}
};

// A player has as many layers as the server says (SimConfig::layers), so in a simulation, and
// wherever it is sent or compared as words, an AnimState is a block of bytes for that many: what
// comes before the stances, a stance per layer (up to a whole word), the two speeds, then the
// layers. A whole number of 32-bit words.
inline constexpr size_t kAnimStateHead = 24;

inline size_t AnimStanceBytes( size_t layers )
{
	return ( layers + 3 ) & ~size_t( 3 );
}

inline size_t AnimStateBytes( size_t layers )
{
	return kAnimStateHead + AnimStanceBytes( layers ) + 8 + layers * sizeof( AnimGraphLayerState );
}

inline void PackAnimState( const AnimState& s, size_t layers, uint8_t* out )
{
	std::memset( out, 0, AnimStateBytes( layers ) );
	out[0] = uint8_t( s.mode );
	out[1] = s.aiming;
	out[2] = s.legsBackward;
	out[3] = s.look;
	const float head[5] = { s.modeTime, s.groundSpeed, s.aimYaw, s.aimPitch, s.legYaw };
	std::memcpy( out + 4, head, 20 );
	s.stances.copy_to( out + kAnimStateHead, layers );
	uint8_t* rest = out + kAnimStateHead + AnimStanceBytes( layers );
	std::memcpy( rest, &s.moveForward, 4 );
	std::memcpy( rest + 4, &s.moveRight, 4 );
	for ( size_t l = 0; l < layers; ++l )
	{
		const AnimGraphLayerState layer = s.graph[l];
		std::memcpy( rest + 8 + l * sizeof( AnimGraphLayerState ), &layer, sizeof( AnimGraphLayerState ) );
	}
}

inline void UnpackAnimState( const uint8_t* in, size_t layers, AnimState& s )
{
	s.mode = AnimMode( in[0] );
	s.aiming = in[1];
	s.legsBackward = in[2];
	s.look = in[3];
	float head[5];
	std::memcpy( head, in + 4, 20 );
	s.modeTime = head[0];
	s.groundSpeed = head[1];
	s.aimYaw = head[2];
	s.aimPitch = head[3];
	s.legYaw = head[4];
	s.stances.assign( in + kAnimStateHead, layers );
	const uint8_t* rest = in + kAnimStateHead + AnimStanceBytes( layers );
	std::memcpy( &s.moveForward, rest, 4 );
	std::memcpy( &s.moveRight, rest + 4, 4 );
	s.graph.resize( layers );
	if ( layers > 0 )
	{
		std::memcpy( static_cast<void*>( s.graph.data() ), rest + 8, layers * sizeof( AnimGraphLayerState ) );
	}
}

// Values a server mod published about an entity, for presentation to read by name. The schema
// (which slot is which field, and its type) travels to clients when they join; the simulation only
// stores what SetField commands write. Floats are stored as their bits.
//
// As many values as the server's mods declared fields (SimConfig::fields): in a simulation an
// entity's board is a block of that size (Simulation::GetBoard / SetBoard), and this is the value
// everything else passes around. A slot nobody wrote reads 0.
using BoardValues = SmallList<int32_t, 32>;

struct Blackboard
{
	BoardValues values;

	bool operator==( const Blackboard& ) const = default;
};

// A ragdoll left behind by a Kill command. One entity holds every body part: the bodies live in
// RagdollBodies, and their poses are mirrored into RagdollPose each tick like any other body.
inline constexpr int kRagdollParts = 11;

struct Ragdoll
{
	uint32_t owner = 0; // NetId of the player it came from (may no longer exist)
	PlayerSlot slot = 0;
	uint8_t reserved[3] = {};
	uint32_t spawnTick = 0;
	uint32_t despawnTick = 0; // 0 = never
	float yaw = 0.0f;		  // facing when it was created: the parts' rest orientation
};

struct RagdollBodies
{
	b3BodyId body[kRagdollParts] = {};
	b3ShapeId shape[kRagdollParts] = {};
};

struct RagdollPose
{
	Transform part[kRagdollParts];
	b3Vec3 linear[kRagdollParts] = {};
};

// The movement parameters a mod set for this player (SetMove commands): a bit of `mask` per
// MoveParam, and its value. The rest are the server's (SimConfig::move). A player nobody set one
// for has no such component.
struct MoveOverrides
{
	uint32_t mask = 0;
	float values[kMoveParams] = {};
};

// Where a player's motions are (sim/motions.h): a slot per motion of the server, in the schema's
// order. Players have it only on a server whose mods provide motions. As many slots as the
// server has motions (SimConfig::motions): in a simulation it is a block of that size
// (Simulation::GetMotionState / SetMotionState), and this is the value that is passed around.

struct MotionSlot
{
	uint32_t lastTick = 0;	// the tick of its last use, plus one; 0: never
	uint32_t untilTick = 0; // it is on (its movement parameters hold) while the tick is below this
	uint32_t used = 0;		// uses since the last refill
	uint32_t sinceTick = 0; // the tick it took effect (a probe: when it took hold): forces ramp from it
};

struct MotionState
{
	ActionBits prevActions = 0; // the mod actions held last tick: a press is one that was not
	SmallList<MotionSlot, 16> slots;
};

// What a motion's probe holds on to (sim/motions.h): a line the motion threw at what the player
// looked at. Its effects act on it ("hit"), and a link is a rope to it (a grappling hook). Players
// have the component once they have thrown one; `on` says whether one is out now.
struct MotionHold
{
	uint32_t anchor = 0;	// NetId of what it holds on to (a prop, a player); 0: a point of the world
	b3Vec3 point = {};		// the world point; on an anchor, the point in that body's frame
	float length = 0.0f;	// a link's rope, in metres; 0: not measured yet (or no link)
	uint32_t startTick = 0; // when it was thrown
	uint32_t holdTick = 0;	// when it reaches the point and takes hold (it flies until then)
	uint16_t motion = 0;	// the motion it belongs to
	uint8_t on = 0;
	uint8_t reserved = 0;
};

// Tag: part of the static level.
struct StaticGeometry
{
};

// "This entity did not come from a template."
inline constexpr uint32_t kNoTemplate = 0xFFFFFFFFu;

// Something a player holds (a sword, a torch): an entity of its own, with its own board and
// events, drawn in its holder's socket. The kind names its look (declared by a mod).
// An item: held in a socket, or (holder 0) lying in the world with a body of its kind's shape.
// A held item is in use (in a hand: mods and state machines see it there) or stowed (carried but
// put away: nothing asks about it; it is drawn in its socket if it has one, a holster).
inline constexpr uint8_t kNoSocket = 255;
struct HeldItem
{
	uint32_t holder = 0; // NetId of the player; 0: in the world
	uint16_t kind = 0;	 // schema item kind
	uint8_t socket = 0;	 // schema socket (kNoSocket: stowed out of sight)
	uint8_t stowed = 0;	 // 1: carried, not in use
	uint8_t slot = kNoSlot; // which of its holder's slots it is in (kNoSlot: none: a mod holds it)
	uint8_t reserved[3] = {};
};

// A player's slots (types.h): how many, which one is selected (kNoSlot: empty hands), and the
// last intent carried out. Only on players of a server that has slots (SimConfig::slots).
struct Slots
{
	uint8_t count = 0;
	uint8_t selected = kNoSlot;
	uint8_t seq = 0;
	uint8_t reserved = 0;
};

// The map template this entity was created from (see reflect.h). The simulation only carries it so
// that presentation can look up which prefab to draw.
struct TemplateRef
{
	uint32_t index = kNoTemplate;
};

#define CB_CHECK_COMPONENT( T, size )                                                                                            \
	static_assert( std::is_trivially_copyable_v<T> );                                                                            \
	static_assert( sizeof( T ) == size, #T " layout changed: check for padding" )

CB_CHECK_COMPONENT( NetId, 4 );
CB_CHECK_COMPONENT( Transform, 28 );
CB_CHECK_COMPONENT( Velocity, 24 );
CB_CHECK_COMPONENT( Shape, 16 );
CB_CHECK_COMPONENT( PhysicsBody, 16 );
CB_CHECK_COMPONENT( Character, 52 );
CB_CHECK_COMPONENT( Prop, 12 );
CB_CHECK_COMPONENT( AnimGraphLayerState, 40 );
CB_CHECK_COMPONENT( TemplateRef, 4 );
CB_CHECK_COMPONENT( HeldItem, 12 );
CB_CHECK_COMPONENT( Slots, 4 );
CB_CHECK_COMPONENT( Ragdoll, 20 );
CB_CHECK_COMPONENT( RagdollBodies, 16 * kRagdollParts );
CB_CHECK_COMPONENT( RagdollPose, 40 * kRagdollParts );
CB_CHECK_COMPONENT( MoveOverrides, 4 + 4 * kMoveParams );
CB_CHECK_COMPONENT( MotionSlot, 16 );
CB_CHECK_COMPONENT( MotionHold, 32 );

#undef CB_CHECK_COMPONENT

// Collision categories.
enum CollisionCategory : uint64_t
{
	CatStatic = 1 << 0,
	CatProp = 1 << 1,
	CatPlayer = 1 << 2,
	CatRagdoll = 1 << 3,
};

} // namespace cb
