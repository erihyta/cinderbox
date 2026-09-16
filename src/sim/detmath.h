#pragma once

// Deterministic math for simulation code.
//
// Rules for anything that runs inside Simulation::Step:
// - No <cmath> trig (sin/cos/atan2/pow/exp...). Their results differ between C runtimes.
//   Use the functions below, which are built on Box3D's cross-platform implementations.
// - sqrt, + - * / and comparisons are fine: IEEE 754 requires them to be correctly rounded.
// - No float -> int conversion of values outside the int range, no uninitialized floats.

#include "box3d/math_functions.h"

#include <cstdint>

namespace cb::detmath
{

inline constexpr float kPi = 3.14159265359f; // same literal as B3_PI
inline constexpr float kTwoPi = 2.0f * kPi;

// Quantized yaw (full turn = 65536) to radians. Multiplying by a power-of-two fraction is exact.
inline float YawToRadians( uint16_t yaw )
{
	return float( yaw ) * ( kTwoPi / 65536.0f );
}

// Radians to quantized yaw. Client side only (input sampling), so it may use any math.
inline uint16_t RadiansToYaw( float radians )
{
	float turns = radians / kTwoPi;
	turns -= float( int64_t( turns ) );
	if ( turns < 0.0f )
	{
		turns += 1.0f;
	}
	return uint16_t( uint32_t( turns * 65536.0f ) & 0xFFFF );
}

inline b3CosSin CosSin( float radians )
{
	return b3ComputeCosSin( radians );
}

inline float Atan2( float y, float x )
{
	return b3Atan2( y, x );
}

// Wrap into [-pi, pi]. Loops instead of remainder() so it does not touch the C runtime.
inline float WrapAngle( float a )
{
	while ( a > kPi )
	{
		a -= kTwoPi;
	}
	while ( a < -kPi )
	{
		a += kTwoPi;
	}
	return a;
}

// Yaw convention (Y up, right-handed): yaw 0 faces +Z, positive yaw turns toward +X.
inline b3Vec3 YawForward( float yaw )
{
	b3CosSin cs = CosSin( yaw );
	return { cs.sine, 0.0f, cs.cosine };
}

// Screen-right for a camera looking along YawForward(yaw) with +Y up.
inline b3Vec3 YawRight( float yaw )
{
	b3CosSin cs = CosSin( yaw );
	return { -cs.cosine, 0.0f, cs.sine };
}

inline b3Quat YawRotation( float yaw )
{
	return b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, yaw );
}

} // namespace cb::detmath
