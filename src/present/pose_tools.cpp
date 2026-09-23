#include "pose_tools.h"

#include "profile.h"
#include "ragdoll.h"

#include "ozz/animation/runtime/skeleton.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cb::present
{

namespace
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

// The shortest rotation taking unit vector `from` onto unit vector `to`.
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

} // namespace

int FindJoint( const anim::AnimSet& set, const char* profileName )
{
	auto names = set.Skeleton().joint_names();
	for ( int j = 0; j < int( names.size() ); ++j )
	{
		const char* profile = anim::ProfileName( names[size_t( j )] );
		if ( profile != nullptr && std::strcmp( profile, profileName ) == 0 )
		{
			return j;
		}
	}
	return -1;
}

RagdollRig BuildRagdollRig( const anim::AnimSet& set )
{
	const auto& skeleton = set.Skeleton();
	const auto& rest = set.RestModels();
	auto parents = skeleton.joint_parents();
	int joints = skeleton.num_joints();

	RagdollRig rig;
	rig.scale = set.Scale();
	rig.restRotation.resize( size_t( joints ) );
	rig.restPosition.resize( size_t( joints ) );
	for ( int j = 0; j < joints; ++j )
	{
		float scale;
		Decompose( rest[size_t( j )], rig.restPosition[size_t( j )], rig.restRotation[size_t( j )], scale );
	}

	rig.anchorJoint.assign( ragdoll::PartCount, -1 );
	std::vector<int> partOfJoint( size_t( joints ), -1 );
	for ( int p = 0; p < ragdoll::PartCount; ++p )
	{
		int j = FindJoint( set, ragdoll::kParts[p].rigJoint );
		rig.anchorJoint[size_t( p )] = j;
		if ( j >= 0 )
		{
			partOfJoint[size_t( j )] = p;
		}
	}

	// Parents come before children in an ozz skeleton, so one pass settles every joint.
	rig.partOf.assign( size_t( joints ), -1 );
	for ( int j = 0; j < joints; ++j )
	{
		if ( partOfJoint[size_t( j )] >= 0 )
		{
			rig.partOf[size_t( j )] = partOfJoint[size_t( j )];
		}
		else if ( parents[size_t( j )] >= 0 )
		{
			rig.partOf[size_t( j )] = rig.partOf[size_t( parents[size_t( j )] )];
		}
		else if ( rig.anchorJoint[ragdoll::Pelvis] >= 0 )
		{
			rig.partOf[size_t( j )] = ragdoll::Pelvis;
		}
	}
	return rig;
}

Transform RagdollFrame( const Transform& pelvis, float yaw )
{
	b3Quat facing = b3MakeQuatFromAxisAngle( b3Vec3{ 0.0f, 1.0f, 0.0f }, yaw );
	b3Vec3 origin = b3Sub( pelvis.position, b3RotateVector( facing, ragdoll::kParts[ragdoll::Pelvis].center ) );
	return { origin, facing };
}

void RagdollModels( const RagdollRig& rig, const Transform* parts, const Transform& frame, Models& out )
{
	size_t joints = rig.partOf.size();
	out.resize( joints );
	b3Quat toModel = b3Conjugate( frame.rotation );

	// Per part: its rotation away from rest, and where its joint is now, both in model space.
	b3Quat delta[ragdoll::PartCount];
	b3Vec3 anchor[ragdoll::PartCount];
	for ( int p = 0; p < ragdoll::PartCount; ++p )
	{
		delta[p] = b3MulQuat( toModel, parts[p].rotation );
		int a = rig.anchorJoint[size_t( p )];
		if ( a < 0 )
		{
			continue;
		}
		b3Vec3 local = b3Sub( rig.restPosition[size_t( a )], ragdoll::kParts[p].center );
		b3Vec3 world = b3Add( parts[p].position, b3RotateVector( parts[p].rotation, local ) );
		anchor[p] = b3RotateVector( toModel, b3Sub( world, frame.position ) );
	}

	for ( size_t j = 0; j < joints; ++j )
	{
		int p = rig.partOf[j];
		if ( p < 0 )
		{
			out[j] = Compose( rig.restPosition[j], rig.restRotation[j], rig.scale );
			continue;
		}
		int a = rig.anchorJoint[size_t( p )];
		b3Vec3 offset = b3Sub( rig.restPosition[j], rig.restPosition[size_t( a )] );
		b3Vec3 position = b3Add( anchor[p], b3RotateVector( delta[p], offset ) );
		b3Quat rotation = b3MulQuat( delta[p], rig.restRotation[j] );
		out[j] = Compose( position, rotation, rig.scale );
	}
}

void BlendModels( const Models& a, const Models& b, float t, Models& out )
{
	size_t n = std::min( a.size(), b.size() );
	out.resize( n );
	for ( size_t j = 0; j < n; ++j )
	{
		b3Vec3 pa, pb;
		b3Quat qa, qb;
		float sa, sb;
		Decompose( a[j], pa, qa, sa );
		Decompose( b[j], pb, qb, sb );
		out[j] = Compose( b3Lerp( pa, pb, t ), Nlerp( qa, qb, t ), sa + ( sb - sa ) * t );
	}
}

void AimChain( const anim::AnimSet& set, Models& models, int joint, int tip, b3Vec3 direction, float weight )
{
	int joints = int( models.size() );
	if ( joint < 0 || tip < 0 || joint >= joints || tip >= joints || weight <= 0.0f )
	{
		return;
	}
	b3Vec3 root, tipPos;
	b3Quat unused;
	float scale;
	Decompose( models[size_t( joint )], root, unused, scale );
	Decompose( models[size_t( tip )], tipPos, unused, scale );
	b3Vec3 current = b3Sub( tipPos, root );
	if ( b3LengthSquared( current ) < 1e-8f )
	{
		return;
	}
	b3Quat turn = Nlerp( b3Quat_identity, Arc( b3Normalize( current ), direction ), std::clamp( weight, 0.0f, 1.0f ) );

	// Everything below the joint turns with it, about the joint.
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
		p = b3Add( root, b3RotateVector( turn, b3Sub( p, root ) ) );
		q = b3MulQuat( turn, q );
		models[size_t( j )] = Compose( p, q, s );
	}
}

} // namespace cb::present
