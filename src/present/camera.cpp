#include "camera.h"

#include <algorithm>
#include <cmath>

namespace cb::present
{

namespace
{

// Ray against an axis-aligned box centred on the origin. Returns the entry distance, 0 when the ray
// starts inside, or a negative number for a miss.
float RayBox( b3Vec3 o, b3Vec3 d, b3Vec3 half )
{
	float tMin = 0.0f;
	float tMax = 1e30f;
	const float origin[3] = { o.x, o.y, o.z };
	const float dir[3] = { d.x, d.y, d.z };
	const float h[3] = { half.x, half.y, half.z };
	for ( int i = 0; i < 3; ++i )
	{
		if ( std::fabs( dir[i] ) < 1e-8f )
		{
			if ( std::fabs( origin[i] ) > h[i] )
			{
				return -1.0f;
			}
			continue;
		}
		float inv = 1.0f / dir[i];
		float t1 = ( -h[i] - origin[i] ) * inv;
		float t2 = ( h[i] - origin[i] ) * inv;
		tMin = std::max( tMin, std::min( t1, t2 ) );
		tMax = std::min( tMax, std::max( t1, t2 ) );
		if ( tMin > tMax )
		{
			return -1.0f;
		}
	}
	return tMin;
}

// Ray against a sphere at `centre`. Same return convention.
float RaySphere( b3Vec3 o, b3Vec3 d, b3Vec3 centre, float radius )
{
	b3Vec3 m = b3Sub( o, centre );
	float b = b3Dot( m, d );
	float c = b3Dot( m, m ) - radius * radius;
	if ( c <= 0.0f )
	{
		return 0.0f;
	}
	if ( b > 0.0f )
	{
		return -1.0f;
	}
	float disc = b * b - c;
	return disc < 0.0f ? -1.0f : -b - std::sqrt( disc );
}

// Ray against an upright capsule centred on the origin: a cylinder of `halfHeight` with two caps.
float RayCapsule( b3Vec3 o, b3Vec3 d, float halfHeight, float radius )
{
	float best = -1.0f;
	auto take = [&best]( float t ) {
		if ( t >= 0.0f && ( best < 0.0f || t < best ) )
		{
			best = t;
		}
	};
	take( RaySphere( o, d, { 0.0f, halfHeight, 0.0f }, radius ) );
	take( RaySphere( o, d, { 0.0f, -halfHeight, 0.0f }, radius ) );
	// The side: a circle in XZ, kept where the hit is between the caps.
	float a = d.x * d.x + d.z * d.z;
	float c = o.x * o.x + o.z * o.z - radius * radius;
	if ( c <= 0.0f && std::fabs( o.y ) <= halfHeight )
	{
		return 0.0f;
	}
	if ( a > 1e-8f )
	{
		float b = o.x * d.x + o.z * d.z;
		float disc = b * b - a * c;
		if ( disc >= 0.0f )
		{
			float t = ( -b - std::sqrt( disc ) ) / a;
			if ( t >= 0.0f && std::fabs( o.y + t * d.y ) <= halfHeight )
			{
				take( t );
			}
		}
	}
	return best;
}

} // namespace

float CameraFreeDistance( const PresentationFrame& frame, b3Vec3 origin, b3Vec3 direction, float maxDistance, float radius )
{
	float free = maxDistance;
	for ( const FrameEntity& e : frame.entities )
	{
		if ( e.kind != VisualKind::Static )
		{
			continue;
		}
		// Into the shape's own space, where it is axis-aligned and centred.
		b3Vec3 o = b3InvRotateVector( e.transform.rotation, b3Sub( origin, e.transform.position ) );
		b3Vec3 d = b3InvRotateVector( e.transform.rotation, direction );
		float t = -1.0f;
		switch ( e.shape )
		{
			case ShapeKind::Box:
				t = RayBox( o, d, { e.halfExtents.x + radius, e.halfExtents.y + radius, e.halfExtents.z + radius } );
				break;
			case ShapeKind::Sphere:
				t = RaySphere( o, d, { 0.0f, 0.0f, 0.0f }, e.halfExtents.x + radius );
				break;
			case ShapeKind::Capsule:
				t = RayCapsule( o, d, e.halfExtents.y, e.halfExtents.x + radius );
				break;
		}
		if ( t >= 0.0f )
		{
			free = std::min( free, t );
		}
	}
	return std::max( free, 0.0f );
}

} // namespace cb::present
