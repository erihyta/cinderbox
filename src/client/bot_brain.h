#pragma once

// Scripted player input for headless bots: wanders, turns, sprints, jumps and spawns props.

#include "types.h"
#include "util.h"

namespace cb
{

struct BotBrain
{
	uint64_t rng = 1;
	PlayerInput held{};
	uint32_t spawnOneIn = 40; // chance per tick to press "spawn prop" (0 = never)
	uint32_t jumpOneIn = 50;

	explicit BotBrain( uint64_t seed = 1 )
		: rng( seed * 0x9E3779B97F4A7C15ull + 1 )
	{
	}

	PlayerInput Next()
	{
		uint64_t r = NextRandom( rng );
		if ( ( r & 31 ) == 0 )
		{
			held.moveForward = int8_t( int( ( r >> 8 ) % 255 ) - 127 );
			held.moveRight = int8_t( int( ( r >> 16 ) % 255 ) - 127 );
			held.cameraYaw = uint16_t( r >> 24 );
		}
		held.buttons = 0;
		if ( jumpOneIn != 0 && ( ( r >> 40 ) % jumpOneIn ) == 0 )
		{
			held.buttons |= BtnJump;
		}
		if ( spawnOneIn != 0 && ( ( r >> 48 ) % spawnOneIn ) == 0 )
		{
			held.buttons |= BtnSpawnProp;
		}
		if ( ( r >> 56 ) & 1 )
		{
			held.buttons |= BtnSprint;
		}
		return held;
	}
};

} // namespace cb
