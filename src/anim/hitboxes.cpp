#include "hitboxes.h"

#include "ozz/base/maths/simd_math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace cb::anim
{

namespace
{

const char* ShapeName( HitShape shape )
{
	switch ( shape )
	{
		case HitShape::Sphere:
			return "sphere";
		case HitShape::Capsule:
			return "capsule";
		case HitShape::Box:
			return "box";
	}
	return "sphere";
}

// An affine placement: x -> origin + axes * x (the axes carry the uniform scale).
struct Frame
{
	b3Vec3 origin;
	b3Vec3 axes[3];

	b3Vec3 Point( b3Vec3 p ) const
	{
		b3Vec3 r = origin;
		r = b3MulAdd( r, p.x, axes[0] );
		r = b3MulAdd( r, p.y, axes[1] );
		return b3MulAdd( r, p.z, axes[2] );
	}
	b3Vec3 Direction( b3Vec3 d ) const
	{
		return b3Add( b3Add( b3MulSV( d.x, axes[0] ), b3MulSV( d.y, axes[1] ) ), b3MulSV( d.z, axes[2] ) );
	}
};

b3Vec3 Column( const ozz::math::Float4x4& m, int c )
{
	float v[4];
	ozz::math::StorePtrU( m.cols[c], v );
	return { v[0], v[1], v[2] };
}

b3Vec3 SafeNormalize( b3Vec3 v, b3Vec3 fallback )
{
	float length = b3Length( v );
	return length > 1e-12f ? b3MulSV( 1.0f / length, v ) : fallback;
}

// Ray p(t) = o + t * d against a sphere; the entering t in [0, maxT].
bool RaySphere( b3Vec3 o, b3Vec3 d, b3Vec3 center, float radius, float maxT, float& t )
{
	b3Vec3 m = b3Sub( o, center );
	float a = b3Dot( d, d );
	float b = b3Dot( m, d );
	float c = b3Dot( m, m ) - radius * radius;
	if ( a <= 0.0f )
	{
		return false;
	}
	if ( c <= 0.0f )
	{
		t = 0.0f; // starts inside
		return true;
	}
	float disc = b * b - a * c;
	if ( b > 0.0f || disc < 0.0f )
	{
		return false;
	}
	float hit = ( -b - std::sqrt( disc ) ) / a;
	if ( hit < 0.0f || hit > maxT )
	{
		return false;
	}
	t = hit;
	return true;
}

// Ray against the capsule of segment [p, q] and `radius`.
bool RayCapsule( b3Vec3 o, b3Vec3 d, b3Vec3 p, b3Vec3 q, float radius, float maxT, float& t )
{
	b3Vec3 axis = b3Sub( q, p );
	float axisLengthSq = b3Dot( axis, axis );
	if ( axisLengthSq <= 1e-12f )
	{
		return RaySphere( o, d, p, radius, maxT, t );
	}

	// The infinite cylinder, then the part of it between the caps.
	b3Vec3 m = b3Sub( o, p );
	float md = b3Dot( m, axis );
	float nd = b3Dot( d, axis );
	float dd = b3Dot( d, d );
	float mn = b3Dot( m, d );
	float a = axisLengthSq * dd - nd * nd;
	float k = b3Dot( m, m ) - radius * radius;
	float c = axisLengthSq * k - md * md;

	float best = maxT;
	bool found = false;
	if ( std::fabs( a ) > 1e-12f )
	{
		float b = axisLengthSq * mn - nd * md;
		float disc = b * b - a * c;
		if ( disc >= 0.0f )
		{
			float hit = ( -b - std::sqrt( disc ) ) / a;
			float along = md + hit * nd;
			if ( hit >= 0.0f && hit <= best && along >= 0.0f && along <= axisLengthSq )
			{
				best = hit;
				found = true;
			}
		}
	}
	if ( c <= 0.0f )
	{
		float along = md;
		if ( along >= 0.0f && along <= axisLengthSq )
		{
			t = 0.0f; // starts inside the cylinder part
			return true;
		}
	}

	float capT;
	if ( RaySphere( o, d, p, radius, best, capT ) && ( found == false || capT < best ) )
	{
		best = capT;
		found = true;
	}
	if ( RaySphere( o, d, q, radius, best, capT ) && ( found == false || capT < best ) )
	{
		best = capT;
		found = true;
	}
	if ( found )
	{
		t = best;
	}
	return found;
}

// Ray against an axis-aligned box of `half` extents at the origin (slabs).
bool RayBox( b3Vec3 o, b3Vec3 d, b3Vec3 half, float maxT, float& t, int& axisHit, float& sign )
{
	float lo = 0.0f;
	float hi = maxT;
	const float oa[3] = { o.x, o.y, o.z };
	const float da[3] = { d.x, d.y, d.z };
	const float ha[3] = { half.x, half.y, half.z };
	axisHit = -1;
	sign = 1.0f;
	for ( int i = 0; i < 3; ++i )
	{
		if ( std::fabs( da[i] ) < 1e-12f )
		{
			if ( oa[i] < -ha[i] || oa[i] > ha[i] )
			{
				return false;
			}
			continue;
		}
		float inv = 1.0f / da[i];
		float t1 = ( -ha[i] - oa[i] ) * inv;
		float t2 = ( ha[i] - oa[i] ) * inv;
		float s = -1.0f;
		if ( t1 > t2 )
		{
			std::swap( t1, t2 );
			s = 1.0f;
		}
		if ( t1 > lo )
		{
			lo = t1;
			axisHit = i;
			sign = s;
		}
		hi = std::min( hi, t2 );
		if ( lo > hi )
		{
			return false;
		}
	}
	t = lo;
	return true;
}

} // namespace

bool ParseHitboxes( const std::string& text, HitboxSet& out, std::string& error )
{
	out.boxes.clear();
	std::istringstream in( text );
	std::string line;
	int number = 0;
	while ( std::getline( in, line ) )
	{
		++number;
		size_t hash = line.find( '#' );
		if ( hash != std::string::npos )
		{
			line.resize( hash );
		}
		std::istringstream fields( line );
		Hitbox box;
		std::string shape;
		if ( !( fields >> box.zone ) )
		{
			continue; // blank
		}
		b3Vec3& t = box.translation;
		b3Quat& q = box.rotation;
		if ( !( fields >> box.bone >> shape >> t.x >> t.y >> t.z >> q.v.x >> q.v.y >> q.v.z >> q.s ) )
		{
			error = "hitboxes.cfg line " + std::to_string( number ) + ": expected zone, bone, shape, position, rotation";
			return false;
		}
		bool ok = false;
		if ( shape == "sphere" )
		{
			box.shape = HitShape::Sphere;
			ok = bool( fields >> box.radius );
		}
		else if ( shape == "capsule" )
		{
			box.shape = HitShape::Capsule;
			ok = bool( fields >> box.radius >> box.height );
		}
		else if ( shape == "box" )
		{
			box.shape = HitShape::Box;
			ok = bool( fields >> box.halfExtents.x >> box.halfExtents.y >> box.halfExtents.z );
		}
		if ( ok == false )
		{
			error = "hitboxes.cfg line " + std::to_string( number ) + ": bad " + shape + " size";
			return false;
		}
		box.rotation = b3NormalizeQuat( box.rotation );
		out.boxes.push_back( std::move( box ) );
	}
	return true;
}

std::string FormatHitboxes( const HitboxSet& set )
{
	std::string text = "# zone bone shape  position (x y z)  rotation (x y z w)  size\n";
	char buf[512];
	for ( const Hitbox& b : set.boxes )
	{
		const b3Vec3& t = b.translation;
		const b3Quat& q = b.rotation;
		int n = std::snprintf( buf, sizeof( buf ), "%s %s %s  %.9g %.9g %.9g  %.9g %.9g %.9g %.9g ", b.zone.c_str(), b.bone.c_str(),
							   ShapeName( b.shape ), t.x, t.y, t.z, q.v.x, q.v.y, q.v.z, q.s );
		text.append( buf, size_t( n ) );
		switch ( b.shape )
		{
			case HitShape::Sphere:
				n = std::snprintf( buf, sizeof( buf ), " %.9g\n", b.radius );
				break;
			case HitShape::Capsule:
				n = std::snprintf( buf, sizeof( buf ), " %.9g %.9g\n", b.radius, b.height );
				break;
			case HitShape::Box:
				n = std::snprintf( buf, sizeof( buf ), " %.9g %.9g %.9g\n", b.halfExtents.x, b.halfExtents.y, b.halfExtents.z );
				break;
		}
		text.append( buf, size_t( n ) );
	}
	return text;
}

void BindHitboxes( HitboxSet& hitboxes, const AnimSet& set, std::string& warnings )
{
	const ozz::animation::Skeleton& skeleton = set.Skeleton();
	auto names = skeleton.joint_names();
	std::vector<Hitbox> kept;
	for ( Hitbox& box : hitboxes.boxes )
	{
		box.joint = -1;
		for ( int j = 0; j < skeleton.num_joints(); ++j )
		{
			if ( box.bone == names[size_t( j )] )
			{
				box.joint = j;
				break;
			}
		}
		if ( box.joint < 0 )
		{
			warnings += "hitbox '" + box.zone + "' is on bone '" + box.bone + "', which the skeleton does not have; ";
			continue;
		}
		kept.push_back( std::move( box ) );
	}
	hitboxes.boxes = std::move( kept );
}

HitboxSet DefaultHitboxes()
{
	// Sized to the procedural rig (anim_set.cpp): joints sit at the top of each segment, and the
	// limbs hang along -Y in the rest pose.
	auto capsule = []( const char* zone, const char* bone, float y, float radius, float height ) {
		Hitbox h;
		h.zone = zone;
		h.bone = bone;
		h.shape = HitShape::Capsule;
		h.translation = { 0.0f, y, 0.0f };
		h.radius = radius;
		h.height = height;
		return h;
	};
	HitboxSet set;
	Hitbox head;
	head.zone = "head";
	head.bone = "Head";
	head.shape = HitShape::Sphere;
	head.translation = { 0.0f, 0.12f, 0.0f };
	head.radius = 0.13f;
	set.boxes.push_back( head );
	set.boxes.push_back( capsule( "torso", "Spine", 0.14f, 0.18f, 0.70f ) );
	for ( const char* side : { "Left", "Right" } )
	{
		std::string s = side;
		set.boxes.push_back( capsule( "arm", ( s + "UpperArm" ).c_str(), -0.135f, 0.06f, 0.39f ) );
		set.boxes.push_back( capsule( "arm", ( s + "LowerArm" ).c_str(), -0.17f, 0.055f, 0.45f ) );
		set.boxes.push_back( capsule( "leg", ( s + "UpperLeg" ).c_str(), -0.21f, 0.09f, 0.54f ) );
		set.boxes.push_back( capsule( "leg", ( s + "LowerLeg" ).c_str(), -0.23f, 0.07f, 0.56f ) );
	}
	return set;
}

bool RayHitboxes( const HitboxSet& hitboxes, const ozz::vector<ozz::math::Float4x4>& models, b3Vec3 feet, b3Quat rotation,
				  b3Vec3 origin, b3Vec3 translation, float maxFraction, HitboxHit& hit )
{
	bool found = false;
	float best = maxFraction;
	for ( const Hitbox& box : hitboxes.boxes )
	{
		if ( box.joint < 0 || size_t( box.joint ) >= models.size() )
		{
			continue;
		}
		// world <- body (feet, rotation) <- joint (pose, scaled) <- hitbox (rigid)
		const ozz::math::Float4x4& jm = models[size_t( box.joint )];
		Frame joint;
		joint.origin = b3Add( feet, b3RotateVector( rotation, Column( jm, 3 ) ) );
		for ( int i = 0; i < 3; ++i )
		{
			joint.axes[i] = b3RotateVector( rotation, Column( jm, i ) );
		}
		Frame shape;
		shape.origin = joint.Point( box.translation );
		for ( int i = 0; i < 3; ++i )
		{
			b3Vec3 unit = { i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f };
			shape.axes[i] = joint.Direction( b3RotateVector( box.rotation, unit ) );
		}
		float scale = b3Length( shape.axes[0] );

		float t = 0.0f;
		b3Vec3 normal = { 0.0f, 1.0f, 0.0f };
		switch ( box.shape )
		{
			case HitShape::Sphere:
			{
				if ( RaySphere( origin, translation, shape.origin, box.radius * scale, best, t ) == false )
				{
					continue;
				}
				b3Vec3 point = b3MulAdd( origin, t, translation );
				normal = SafeNormalize( b3Sub( point, shape.origin ), normal );
				break;
			}
			case HitShape::Capsule:
			{
				float half = std::max( 0.5f * box.height - box.radius, 0.0f );
				b3Vec3 p = shape.Point( { 0.0f, -half, 0.0f } );
				b3Vec3 q = shape.Point( { 0.0f, half, 0.0f } );
				if ( RayCapsule( origin, translation, p, q, box.radius * scale, best, t ) == false )
				{
					continue;
				}
				b3Vec3 point = b3MulAdd( origin, t, translation );
				b3Vec3 axis = b3Sub( q, p );
				float lengthSq = b3Dot( axis, axis );
				float s = lengthSq > 0.0f ? std::clamp( b3Dot( b3Sub( point, p ), axis ) / lengthSq, 0.0f, 1.0f ) : 0.0f;
				normal = SafeNormalize( b3Sub( point, b3MulAdd( p, s, axis ) ), normal );
				break;
			}
			case HitShape::Box:
			{
				// Into the box's own unit frame: axes are orthogonal with length `scale`.
				b3Vec3 u[3];
				for ( int i = 0; i < 3; ++i )
				{
					u[i] = SafeNormalize( shape.axes[i], {} );
				}
				b3Vec3 rel = b3Sub( origin, shape.origin );
				b3Vec3 lo = { b3Dot( rel, u[0] ), b3Dot( rel, u[1] ), b3Dot( rel, u[2] ) };
				b3Vec3 ld = { b3Dot( translation, u[0] ), b3Dot( translation, u[1] ), b3Dot( translation, u[2] ) };
				int axisHit;
				float sign;
				if ( RayBox( lo, ld, b3MulSV( scale, box.halfExtents ), best, t, axisHit, sign ) == false )
				{
					continue;
				}
				normal = axisHit >= 0 ? b3MulSV( sign, u[axisHit] ) : b3MulSV( -1.0f, SafeNormalize( translation, normal ) );
				break;
			}
		}
		best = t;
		found = true;
		hit.fraction = t;
		hit.point = b3MulAdd( origin, t, translation );
		hit.normal = normal;
		hit.box = &box;
	}
	return found;
}

} // namespace cb::anim
