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
	// Commands the way a server's mods would send them: props, kills with ragdolls, respawns,
	// fields, events and impulses. Off gives a pure movement scenario.
	bool commands = true;
};

// A float from 8 random bits, exactly representable, in [lo, lo + range).
inline float ScenarioFloat( uint64_t bits, float lo, float range )
{
	return lo + range * float( bits & 255 ) * ( 1.0f / 256.0f );
}

inline std::vector<InputFrame> MakeScenario( const ScenarioOptions& opt )
{
	std::vector<InputFrame> frames( opt.ticks );
	uint64_t rng = opt.seed;
	bool active[kMaxPlayers] = {};
	uint32_t deadSince[kMaxPlayers] = {}; // 0: alive
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
			deadSince[slot] = 0;
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
			in.buttons = buttons;
			in.cameraPitch = int16_t( int( ( r >> 44 ) % 8000 ) - 4000 );
			in.actions = uint16_t( ( r >> 56 ) & 3 );
			f.inputs[i] = in;

			if ( opt.commands == false || t == 0 )
			{
				continue;
			}
			uint64_t c = NextRandom( rng );
			if ( ( c % 25 ) == 0 && deadSince[i] == 0 )
			{
				SimCommand spawn;
				spawn.type = CommandType::SpawnProp;
				spawn.mode = uint8_t( ( c >> 8 ) & 1 );
				spawn.index = uint16_t( c >> 16 );
				spawn.target = SlotTarget( PlayerSlot( i ) );
				spawn.other = 600;
				spawn.value = -1;
				spawn.a = { -6.0f + 1.5f * float( i % 8 ), 3.0f, ScenarioFloat( c >> 24, 2.0f, 6.0f ) };
				spawn.b = { 0.0f, 2.0f, ScenarioFloat( c >> 32, -3.0f, 6.0f ) };
				float size = ScenarioFloat( c >> 40, 0.2f, 0.25f );
				spawn.c = { size, size, size };
				f.commands.push_back( spawn );
			}
			if ( ( ( c >> 48 ) % 400 ) == 0 && deadSince[i] == 0 )
			{
				SimCommand kill;
				kill.type = CommandType::Kill;
				kill.mode = 1;
				kill.target = SlotTarget( PlayerSlot( i ) );
				kill.other = 900;
				kill.value = 6;
				kill.a = { 0.0f, 1.0f, 0.0f };
				kill.b = { ScenarioFloat( c >> 52, -4.0f, 8.0f ), 2.0f, ScenarioFloat( c >> 56, -4.0f, 8.0f ) };
				f.commands.push_back( kill );

				SimCommand event;
				event.type = CommandType::Event;
				event.index = 1;
				event.target = SlotTarget( PlayerSlot( ( i + 1 ) % opt.maxPlayers ) );
				event.other = SlotTarget( PlayerSlot( i ) );
				event.value = int32_t( t );
				f.commands.push_back( event );
				deadSince[i] = t;
			}
			else if ( deadSince[i] != 0 && t - deadSince[i] >= 120 )
			{
				SimCommand respawn;
				respawn.type = CommandType::Respawn;
				respawn.target = SlotTarget( PlayerSlot( i ) );
				f.commands.push_back( respawn );
				deadSince[i] = 0;
			}
			if ( ( ( c >> 20 ) % 60 ) == 0 )
			{
				SimCommand field;
				field.type = CommandType::SetField;
				field.target = ( c >> 30 ) & 1 ? SlotTarget( PlayerSlot( i ) ) : 0;
				field.index = uint16_t( ( c >> 32 ) % kBoardSlots );
				field.value = int32_t( c >> 36 );
				f.commands.push_back( field );
			}
			if ( ( ( c >> 4 ) % 150 ) == 0 )
			{
				SimCommand freeze;
				freeze.type = CommandType::Freeze;
				freeze.mode = uint8_t( ( c >> 10 ) & 1 );
				freeze.target = SlotTarget( PlayerSlot( i ) );
				f.commands.push_back( freeze );
			}
			if ( ( ( c >> 6 ) % 120 ) == 0 )
			{
				SimCommand aim;
				aim.type = CommandType::Aim;
				aim.mode = uint8_t( ( c >> 11 ) & 1 );
				aim.target = SlotTarget( PlayerSlot( i ) );
				f.commands.push_back( aim );
			}
			if ( ( ( c >> 8 ) % 110 ) == 0 )
			{
				SimCommand facing;
				facing.type = CommandType::Facing;
				facing.mode = uint8_t( ( c >> 13 ) & 1 );
				facing.target = SlotTarget( PlayerSlot( i ) );
				f.commands.push_back( facing );
			}
			if ( ( ( c >> 12 ) % 90 ) == 0 )
			{
				SimCommand push;
				push.type = CommandType::Impulse;
				push.mode = ImpulseVelocity;
				push.target = SlotTarget( PlayerSlot( i ) );
				push.b = { 0.0f, 4.0f, 1.0f };
				f.commands.push_back( push );
			}
		}
	}
	return frames;
}

} // namespace cb::test
