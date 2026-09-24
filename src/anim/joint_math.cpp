#include "joint_math.h"

#include "profile.h"

#include "ozz/animation/runtime/skeleton.h"

#include <algorithm>
#include <cstring>

namespace cb::anim
{

using ozz::math::Float4x4;
using ozz::math::SimdFloat4;

void Decompose( const Float4x4& m, b3Vec3& position, b3Quat& rotation, float& scale )
{
	SimdFloat4 t, q, s;
	if ( ozz::math::ToAffine( m, &t, &q, &s ) == false )
	{
		position = { 0.0f, 0.0f, 0.0f };
		rotation = b3Quat_identity;
		scale = 0.0f;
		return;
	}
	float tv[4], qv[4], sv[4];
	ozz::math::StorePtrU( t, tv );
	ozz::math::StorePtrU( q, qv );
	ozz::math::StorePtrU( s, sv );
	position = { tv[0], tv[1], tv[2] };
	rotation = { { qv[0], qv[1], qv[2] }, qv[3] };
	scale = sv[0];
}

Float4x4 Compose( b3Vec3 position, b3Quat rotation, float scale )
{
	return Float4x4::FromAffine( ozz::math::simd_float4::Load( position.x, position.y, position.z, 1.0f ),
								 ozz::math::simd_float4::Load( rotation.v.x, rotation.v.y, rotation.v.z, rotation.s ),
								 ozz::math::simd_float4::Load( scale, scale, scale, 1.0f ) );
}

b3Quat Nlerp( b3Quat a, b3Quat b, float t )
{
	if ( b3DotQuat( a, b ) < 0.0f )
	{
		b = b3NegateQuat( b );
	}
	return b3NLerp( a, b, t );
}

b3Quat Arc( b3Vec3 from, b3Vec3 to )
{
	float d = b3Dot( from, to );
	if ( d < -0.9999f )
	{
		// Opposite: any perpendicular axis will do.
		b3Vec3 axis = b3Cross( b3Vec3{ 1.0f, 0.0f, 0.0f }, from );
		if ( b3LengthSquared( axis ) < 1e-6f )
		{
			axis = b3Cross( b3Vec3{ 0.0f, 1.0f, 0.0f }, from );
		}
		axis = b3Normalize( axis );
		return { axis, 0.0f };
	}
	b3Vec3 c = b3Cross( from, to );
	b3Quat q = { c, 1.0f + d };
	return b3NormalizeQuat( q );
}

int FindJoint( const AnimSet& set, const char* profileName )
{
	auto names = set.Skeleton().joint_names();
	for ( int j = 0; j < int( names.size() ); ++j )
	{
		const char* profile = ProfileName( names[size_t( j )] );
		if ( profile != nullptr && std::strcmp( profile, profileName ) == 0 )
		{
			return j;
		}
	}
	return -1;
}

void RotateSubtree( const AnimSet& set, Models& models, int joint, b3Quat turn )
{
	const int joints = int( models.size() );
	if ( joint < 0 || joint >= joints )
	{
		return;
	}
	b3Vec3 pivot;
	b3Quat unused;
	float scale;
	Decompose( models[size_t( joint )], pivot, unused, scale );
	// Everything below the joint turns with it (ozz orders parents first).
	auto parents = set.Skeleton().joint_parents();
	std::vector<bool> below( size_t( joints ), false );
	below[size_t( joint )] = true;
	for ( int j = joint + 1; j < joints; ++j )
	{
		int parent = parents[size_t( j )];
		below[size_t( j )] = parent >= 0 && below[size_t( parent )];
	}
	for ( int j = joint; j < joints; ++j )
	{
		if ( below[size_t( j )] == false )
		{
			continue;
		}
		b3Vec3 p;
		b3Quat q;
		float s;
		Decompose( models[size_t( j )], p, q, s );
		p = b3Add( pivot, b3RotateVector( turn, b3Sub( p, pivot ) ) );
		q = b3MulQuat( turn, q );
		models[size_t( j )] = Compose( p, q, s );
	}
}

void AimChain( const AnimSet& set, Models& models, const std::vector<std::pair<int, float>>& chain, int tip, b3Vec3 direction )
{
	const int joints = int( models.size() );
	if ( tip < 0 || tip >= joints )
	{
		return;
	}
	for ( const auto& [joint, weight] : chain )
	{
		if ( joint < 0 || joint >= joints || weight <= 0.0f )
		{
			continue;
		}
		b3Vec3 root, tipPos;
		b3Quat unused;
		float scale;
		Decompose( models[size_t( joint )], root, unused, scale );
		Decompose( models[size_t( tip )], tipPos, unused, scale );
		b3Vec3 current = b3Sub( tipPos, root );
		if ( b3LengthSquared( current ) < 1e-8f )
		{
			continue;
		}
		b3Quat turn = Nlerp( b3Quat_identity, Arc( b3Normalize( current ), direction ), std::clamp( weight, 0.0f, 1.0f ) );
		RotateSubtree( set, models, joint, turn );
	}
}

} // namespace cb::anim
