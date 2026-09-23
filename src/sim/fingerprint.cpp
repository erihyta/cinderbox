#include "fingerprint.h"

#include "simulation.h"
#include "util.h"

namespace cb
{

uint64_t BuildFingerprint()
{
	static const uint64_t fingerprint = [] {
		SimConfig config;
		config.physicsArenaMB = 16;
		Simulation sim( config );

		InputFrame frame;
		for ( PlayerSlot slot = 0; slot < 4; ++slot )
		{
			frame.events.push_back( { PlayerEventType::Join, slot } );
		}

		uint64_t rng = 42;
		for ( uint32_t t = 0; t < 180; ++t )
		{
			frame.tick = t;
			for ( int i = 0; i < 4; ++i )
			{
				uint64_t r = NextRandom( rng );
				PlayerInput& in = frame.inputs[i];
				in.moveForward = int8_t( int( r % 255 ) - 127 );
				in.moveRight = int8_t( int( ( r >> 8 ) % 255 ) - 127 );
				in.cameraYaw = uint16_t( r >> 16 );
				in.buttons = uint8_t( ( r >> 32 ) & kEngineButtons );
			}

			// Every kind of command, so the fingerprint covers the ragdoll and the rest of what
			// mods can ask for, not just movement.
			if ( t % 10 == 5 )
			{
				SimCommand spawn;
				spawn.type = CommandType::SpawnProp;
				spawn.mode = uint8_t( ( t / 10 ) % 2 );
				spawn.target = SlotTarget( PlayerSlot( ( t / 10 ) % 4 ) );
				spawn.other = 300;
				spawn.value = -1;
				spawn.a = { float( t % 7 ) - 3.0f, 4.0f, 2.0f };
				spawn.b = { 0.0f, 1.0f, 3.0f };
				spawn.c = { 0.3f, 0.3f, 0.3f };
				frame.commands.push_back( spawn );
			}
			if ( t == 60 )
			{
				SimCommand kill;
				kill.type = CommandType::Kill;
				kill.mode = 1;
				kill.target = SlotTarget( 1 );
				kill.value = 4;
				kill.b = { 0.0f, 2.0f, -5.0f };
				frame.commands.push_back( kill );

				SimCommand field;
				field.type = CommandType::SetField;
				field.target = SlotTarget( 1 );
				field.index = 3;
				field.value = 7;
				frame.commands.push_back( field );

				SimCommand event;
				event.type = CommandType::Event;
				event.index = 2;
				event.target = SlotTarget( 0 );
				event.other = SlotTarget( 1 );
				frame.commands.push_back( event );
			}
			if ( t == 90 )
			{
				SimCommand push;
				push.type = CommandType::Impulse;
				push.mode = ImpulseVelocity;
				push.target = SlotTarget( 2 );
				push.b = { 0.0f, 3.0f, 0.0f };
				frame.commands.push_back( push );
			}
			if ( t == 150 )
			{
				SimCommand respawn;
				respawn.type = CommandType::Respawn;
				respawn.target = SlotTarget( 1 );
				frame.commands.push_back( respawn );
			}
			sim.Step( frame );
			frame.events.clear();
			frame.commands.clear();
		}
		return sim.ComputeHash();
	}();
	return fingerprint;
}

} // namespace cb
