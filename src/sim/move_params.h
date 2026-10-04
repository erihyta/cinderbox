#pragma once

// Movement parameters: the numbers the character mover works with (mover.h). They are values, not
// constants, so that a server, a character and a mod can each say how players move:
//
//   the engine     the defaults below
//   the server     cb_server --move walk_speed=4          (SimConfig::move)
//   the character  CbCharacter.movement, baked as "move.walk_speed = 4" in its anim.cfg: its clips
//                  are made for a speed. The server lays it over its own (SimConfig::move again)
//   a mod          Context::SetMove( player, MoveParam::WalkSpeed, 1.5f ), a command: that player's
//                  value until the mod gives it back (MoveOverrides, components.h)
//
// A later one wins over an earlier one. Names are the stable identity (options, baked files, the
// editor); the order below is the simulation's and part of the protocol: append only.

#include <cstdint>

namespace cb
{

enum class MoveParam : uint8_t
{
	WalkSpeed = 0,	 // m/s on the ground, without sprint
	SprintSpeed = 1, // m/s on the ground, with sprint
	Accelerate = 2,	 // how fast the speed is reached: per second, times the speed asked for
	Friction = 3,	 // how fast a grounded character slows down, per second
	StopSpeed = 4,	 // m/s: below it friction acts as if the character moved this fast, so it stops
	AirControl = 5,	 // the share of Accelerate a character has in the air
	Gravity = 6,	 // m/s^2, downward
	JumpSpeed = 7,	 // m/s upward on the tick of a jump
	TurnRate = 8,	 // rad/s: how fast the body turns toward where it walks (freelook)
	MaxFall = 9,	 // m/s: the fastest a character falls; 0: no limit
};
inline constexpr int kMoveParams = 10;

struct MoveParams
{
	float values[kMoveParams] = { 3.0f, 6.5f, 12.0f, 6.0f, 1.0f, 0.3f, 18.0f, 6.5f, 12.0f, 0.0f };

	float operator[]( MoveParam param ) const
	{
		return values[int( param )];
	}
	bool operator==( const MoveParams& ) const = default;
};

struct MoveParamInfo
{
	const char* name; // "walk_speed"
	float min;
	float max;
	const char* doc; // one line, for the editor and --help
};

// `index` is a MoveParam; out of range gives an entry with an empty name.
const MoveParamInfo& MoveParamInfoOf( int index );
// The parameter of that name, or -1.
int MoveParamByName( const char* name );
// `value` inside the parameter's range. Not for NaN: callers refuse what is not finite first.
float ClampMoveParam( int index, float value );
// Finite and inside its parameter's range: what a baked file or a welcome may carry.
bool ValidMoveParams( const MoveParams& params );

} // namespace cb
