#include "scripts.h"

#include "../presentation.h"

namespace cb::present::scripts
{

namespace
{
// Distance covered by one full walk cycle (two steps).
constexpr float kStrideLength = 1.6f;
} // namespace

void RegisterPlayerMotion( flecs::world& world )
{
	// Advance the walk cycle by distance travelled so feet roughly match ground speed.
	// M3 replaces the procedural body with ozz and uses this phase to sync the blended clips.
	world.system<PlayerMotion>( "PlayerWalkPhase" ).each( []( flecs::iter& it, size_t, PlayerMotion& m ) {
		if ( m.grounded && m.groundSpeed > 0.05f )
		{
			m.phase += m.groundSpeed * it.delta_time() / kStrideLength;
			m.phase -= float( int( m.phase ) );
		}
		else
		{
			// Ease back to the neutral pose.
			float target = m.phase < 0.5f ? 0.0f : 1.0f;
			m.phase += ( target - m.phase ) * 0.2f;
			if ( m.phase >= 0.999f )
			{
				m.phase = 0.0f;
			}
		}
	} );
}

} // namespace cb::present::scripts
