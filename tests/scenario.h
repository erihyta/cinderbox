#pragma once

// Scripted input streams for tests. Integer-only generation so the stream is identical everywhere.

#include "types.h"
#include "util.h"

#include <vector>

namespace cb::test
{

struct ScenarioOptions
{
	uint32_t ticks = 1200;
	int initialPlayers = 8;
	int maxPlayers = 12;
	uint64_t seed = 1234;
	bool churn = true; // random joins/leaves
};

inline std::vector<InputFrame> MakeScenario( const ScenarioOptions& opt )
{
	std::vector<InputFrame> frames( opt.ticks );
	uint64_t rng = opt.seed;
	bool active[kMaxPlayers] = {};
	PlayerInput held[kMaxPlayers] = {};

	for ( uint32_t t = 0; t < opt.ticks; ++t )
	{
		InputFrame& f = frames[t];
		f.tick = t;

		if ( t == 0 )
		{
			for ( int i = 0; i < opt.initialPlayers; ++i )
			{
				f.events.push_back( { PlayerEventType::Join, PlayerSlot( i ) } );
				active[i] = true;
			}
		}
		else if ( opt.churn && ( NextRandom( rng ) % 97 ) == 0 )
		{
			int slot = int( NextRandom( rng ) % uint64_t( opt.maxPlayers ) );
			f.events.push_back( { active[slot] ? PlayerEventType::Leave : PlayerEventType::Join, PlayerSlot( slot ) } );
			active[slot] = !active[slot];
			held[slot] = {};
		}

		for ( int i = 0; i < kMaxPlayers; ++i )
		{
			if ( active[i] == false )
			{
				continue;
			}

			PlayerInput& in = held[i];
			// Inputs change in bursts, like real players: mostly held, sometimes changed.
			uint64_t r = NextRandom( rng );
			if ( ( r & 15 ) == 0 )
			{
				in.moveForward = int8_t( int( ( r >> 8 ) % 255 ) - 127 );
				in.moveRight = int8_t( int( ( r >> 16 ) % 255 ) - 127 );
			}
			if ( ( ( r >> 24 ) & 7 ) == 0 )
			{
				in.cameraYaw = uint16_t( in.cameraYaw + uint16_t( ( r >> 32 ) % 4096 ) - 2048 );
			}
			uint8_t buttons = 0;
			if ( ( ( r >> 40 ) % 40 ) == 0 )
			{
				buttons |= BtnJump;
			}
			if ( ( ( r >> 48 ) % 3 ) == 0 )
			{
				buttons |= BtnSprint;
			}
			if ( ( ( r >> 52 ) % 25 ) == 0 )
			{
				buttons |= BtnSpawnProp;
			}
			in.buttons = buttons;
			f.inputs[i] = in;
		}
	}
	return frames;
}

} // namespace cb::test
