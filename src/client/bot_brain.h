#pragma once

// Scripted player input for headless bots. Behaves roughly like a person: holds a direction and
// sprint for a while, turns the camera smoothly now and then, jumps and spawns props occasionally.

#include "types.h"
#include "util.h"

namespace cb
{

struct BotBrain
{
	uint64_t rng = 1;
	PlayerInput held{};
	int16_t turnRate = 0;	   // camera yaw units per tick while turning
	uint32_t turnTicksLeft = 0;
	uint32_t spawnOneIn = 40;  // chance per tick to press "spawn prop" (0 = never)
	uint32_t jumpOneIn = 50;
	bool chaotic = false; // worst case: every input field changes every tick

	explicit BotBrain( uint64_t seed = 1 )
		: rng( seed * 0x9E3779B97F4A7C15ull + 1 )
	{
	}

	PlayerInput Next()
	{
		uint64_t r = NextRandom( rng );
		if ( chaotic )
		{
			PlayerInput in;
			in.moveForward = int8_t( int( r % 255 ) - 127 );
			in.moveRight = int8_t( int( ( r >> 8 ) % 255 ) - 127 );
			in.cameraYaw = uint16_t( r >> 16 );
			in.buttons = uint8_t( ( r >> 32 ) & kEngineButtons );
			return in;
		}

		// Change direction and sprint about twice a second.
		if ( ( r & 31 ) == 0 )
		{
			held.moveForward = int8_t( int( ( r >> 8 ) % 255 ) - 127 );
			held.moveRight = int8_t( int( ( r >> 16 ) % 255 ) - 127 );
			held.buttons = ( ( r >> 24 ) & 1 ) ? BtnSprint : 0;
		}

		// Smooth mouse turns, about a third of the time.
		if ( turnTicksLeft == 0 && ( ( r >> 32 ) % 90 ) == 0 )
		{
			turnTicksLeft = uint32_t( 20 + ( r >> 40 ) % 60 );
			turnRate = int16_t( int( ( r >> 48 ) % 400 ) - 200 );
		}
		if ( turnTicksLeft > 0 )
		{
			held.cameraYaw = uint16_t( held.cameraYaw + uint16_t( turnRate ) );
			--turnTicksLeft;
		}

		PlayerInput out = held;
		uint64_t r2 = NextRandom( rng );
		if ( jumpOneIn != 0 && ( r2 % jumpOneIn ) == 0 )
		{
			out.buttons |= BtnJump;
		}
		return out;
	}
};

} // namespace cb
