#pragma once

// Another player's character as a hybrid client is sent it (M253; ROADMAP step 3b): a puppet.
//
// Everything a client needs to draw a character it does not move, in a few numbers: where it is,
// where it faces and looks, a few flags, and where each layer of its state machine is (M254). The
// server makes one for every character of each state it keeps (Simulation::PuppetOf); a client
// sent one puts the character where it says (Simulation::ApplyPuppets) and runs its animation
// itself, taking each layer's state from it: what a puppet moves by (its Character, Transform,
// Velocity, AnimState, MotionState, its movement and driving) are then the client's own, and its
// record goes to the client without them (Simulation::PuppetView), so they never cost bytes.
//
// Positions are whole 1/256 m, angles whole steps of a turn: a change is a few small integers.

#include "types.h"

#include <cstdint>

namespace cb
{

// How many of a character's layers a puppet says where they are; those past it run on their own.
inline constexpr size_t kPuppetLayers = 12;
// Positions in a puppet, in steps per metre.
inline constexpr float kPuppetGrid = 256.0f;

enum PuppetFlags : uint8_t
{
	PuppetGrounded = 1,
	PuppetSprinting = 2,
	PuppetDead = 4,
	PuppetFrozen = 8,
	PuppetFacesCamera = 16, // its body faces where it looks (Character::faceCamera, or a first-person view)
	PuppetAiming = 32,		// AnimState::aiming
	PuppetUsing = 64,		// it uses what it holds this tick ("using.<kind>")
};

// One layer of its state machine (AnimGraphLayerState): the state it is in, the one it fades from
// and through which of that one's transitions (`via`: an index into its transitions, or with 0x80
// into this state's restarts; 0xFF none), its source, and the tick at whose end the state began. A
// OneShot layer: `via` 1 while its shot plays, `start` the tick it was fired. `state` 0xFF: the
// layer has not started.
struct PuppetLayer
{
	uint8_t state = 0xFF;
	uint8_t previous = 0;
	uint8_t via = 0xFF;
	uint8_t source = 0;
	uint32_t start = 0;

	bool operator==( const PuppetLayer& ) const = default;
};

struct Puppet
{
	int32_t x = 0, y = 0, z = 0; // its Transform's position, in 1/256 m
	uint16_t facing = 0;		 // Character::facingYaw, 65536 a turn
	int8_t aimYaw = 0;			 // AnimState::aimYaw (relative to the facing), 256 a turn
	int8_t aimPitch = 0;		 // AnimState::aimPitch, 256 a turn
	uint8_t flags = 0;			 // PuppetFlags
	uint8_t layerCount = 0;
	PlayerSlot slot = kNoPlayer; // Character::slot
	uint32_t lastJumpTick = 0;	 // Character::lastJumpTick
	uint32_t airSince = 0;		 // the tick it left the ground (0 on it)
	uint32_t stepCount = 0;		 // Character::stepCount
	uint64_t motionsOn = 0;		 // Simulation::EntityMotionsOn
	uint8_t stances[kPuppetLayers] = {};
	PuppetLayer layers[kPuppetLayers];
	uint32_t reserved = 0;

	bool operator==( const Puppet& ) const = default;
};
static_assert( sizeof( PuppetLayer ) == 8 && sizeof( Puppet ) == 152, "Puppet has padding: a server hashes it as bytes" );

// A puppet of one entity in a state (by NetId), and the state it was made in: a client keeps one
// it was last sent while the entity is left out of later ones (by distance).
struct PuppetEntry
{
	uint32_t netId = 0;
	uint32_t tick = 0;
	Puppet puppet;

	bool operator==( const PuppetEntry& ) const = default;
};

// On a client's stand-in of a puppet (never in a snapshot or a hash): its last two puppets, and
// the tick its animation has been run through here.
struct PuppetState
{
	Puppet now, before;
	uint32_t nowTick = 0, beforeTick = 0;
	uint32_t animThrough = 0;
	uint8_t started = 0;
};

} // namespace cb
