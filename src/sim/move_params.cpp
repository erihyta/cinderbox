#include "move_params.h"

#include <cstring>

namespace cb
{

namespace
{

const MoveParamInfo kInfo[kMoveParams] = {
	{ "walk_speed", 0.0f, 100.0f, "Speed on the ground without sprint, in m/s." },
	{ "sprint_speed", 0.0f, 100.0f, "Speed on the ground with sprint, in m/s." },
	{ "accelerate", 0.0f, 1000.0f, "How fast the speed is reached: per second, times the speed asked for." },
	{ "friction", 0.0f, 100.0f, "How fast a grounded character slows down, per second." },
	{ "stop_speed", 0.0f, 100.0f, "Below this speed (m/s) friction acts as if the character moved this fast." },
	{ "air_control", 0.0f, 10.0f, "The share of accelerate a character has in the air (1: as on the ground)." },
	{ "gravity", 0.0f, 200.0f, "Downward acceleration, in m/s^2." },
	{ "jump_speed", 0.0f, 100.0f, "Upward speed on the tick of a jump, in m/s." },
	{ "turn_rate", 0.0f, 100.0f, "How fast the body turns toward where it walks, in rad/s." },
	{ "max_fall", 0.0f, 1000.0f, "The fastest a character falls, in m/s. 0: no limit." },
};

const MoveParamInfo kNone = { "", 0.0f, 0.0f, "" };

} // namespace

const MoveParamInfo& MoveParamInfoOf( int index )
{
	return index >= 0 && index < kMoveParams ? kInfo[index] : kNone;
}

int MoveParamByName( const char* name )
{
	for ( int i = 0; i < kMoveParams; ++i )
	{
		if ( std::strcmp( kInfo[i].name, name ) == 0 )
		{
			return i;
		}
	}
	return -1;
}

float ClampMoveParam( int index, float value )
{
	const MoveParamInfo& info = MoveParamInfoOf( index );
	return value < info.min ? info.min : ( value > info.max ? info.max : value );
}

bool ValidMoveParams( const MoveParams& params )
{
	for ( int i = 0; i < kMoveParams; ++i )
	{
		float v = params.values[i];
		// NaN fails both comparisons.
		if ( ( v >= kInfo[i].min && v <= kInfo[i].max ) == false )
		{
			return false;
		}
	}
	return true;
}

} // namespace cb
