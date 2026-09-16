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
				in.buttons = uint8_t( ( r >> 32 ) & ( BtnJump | BtnSprint | BtnSpawnProp ) );
			}
			sim.Step( frame );
			frame.events.clear();
		}
		return sim.ComputeHash();
	}();
	return fingerprint;
}

} // namespace cb
