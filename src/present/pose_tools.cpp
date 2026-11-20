#include "pose_tools.h"

#include "detmath.h"
#include "profile.h"
#include "ragdoll.h"

#include "ozz/animation/runtime/skeleton.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cb::present
{

using anim::Arc;
using anim::Compose;
using anim::Decompose;
using anim::Nlerp;
using ozz::math::Float4x4;

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
	std::vector<int> parentOf( static_cast<size_t>( joints ) );
	for ( int j = 0; j < joints; ++j )
	{
		parentOf[size_t( j )] = parents[size_t( j )];
	}
	FitRestToRagdoll( parentOf, rig );
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

void FitRestToRagdoll( const std::vector<int>& parents, RagdollRig& rig )
{
	const size_t joints = rig.restPosition.size();
	const float limit = std::cos( kRestFitDegrees * detmath::kPi / 180.0f );

	// Per part, the joint its bone ends at: the next part's joint furthest along it, else its own
	// joint's child furthest from it (a hand, a foot, the head).
	int boneEnd[ragdoll::PartCount];
	for ( int p = 0; p < ragdoll::PartCount; ++p )
	{
		boneEnd[p] = -1;
		const int a = rig.anchorJoint[size_t( p )];
		if ( a < 0 || ragdoll::kParts[p].parent < 0 )
		{
			continue;
		}
		const b3Vec3 along = b3Normalize( b3Sub( ragdoll::kParts[p].center, ragdoll::kParts[p].anchor ) );
		float best = 0.0f;
		for ( int q = 0; q < ragdoll::PartCount; ++q )
		{
			const int b = rig.anchorJoint[size_t( q )];
			const float ahead = b3Dot( b3Sub( ragdoll::kParts[q].anchor, ragdoll::kParts[p].anchor ), along );
			if ( ragdoll::kParts[q].parent == p && b >= 0 && ahead > best )
			{
				best = ahead;
				boneEnd[p] = b;
			}
		}
		if ( boneEnd[p] >= 0 )
		{
			continue;
		}
		float farthest = 0.0f;
		for ( size_t j = 0; j < joints; ++j )
		{
			const float d = b3Distance( rig.restPosition[j], rig.restPosition[size_t( a )] );
			if ( parents[j] == a && d > farthest )
			{
				farthest = d;
				boneEnd[p] = int( j );
			}
		}
	}

	// Each joint moved rigidly with its nearest turned ancestor: x -> turn x + shift.
	std::vector<b3Quat> turn( joints, b3Quat_identity );
	std::vector<b3Vec3> shift( joints, b3Vec3{ 0.0f, 0.0f, 0.0f } );
	for ( size_t j = 0; j < joints; ++j )
	{
		if ( parents[j] >= 0 )
		{
			turn[j] = turn[size_t( parents[j] )];
			shift[j] = shift[size_t( parents[j] )];
		}
		for ( int p = 0; p < ragdoll::PartCount; ++p )
		{
			if ( rig.anchorJoint[size_t( p )] != int( j ) || boneEnd[p] < 0 )
			{
				continue;
			}
			const b3Vec3 bone = b3Sub( rig.restPosition[size_t( boneEnd[p] )], rig.restPosition[j] );
			if ( b3Length( bone ) < 1e-4f )
			{
				continue;
			}
			const b3Vec3 now = b3RotateVector( turn[j], b3Normalize( bone ) );
			const b3Vec3 along = b3Normalize( b3Sub( ragdoll::kParts[p].center, ragdoll::kParts[p].anchor ) );
			if ( b3Dot( now, along ) >= limit )
			{
				continue;
			}
			// Turned about the joint where it is now.
			const b3Vec3 pivot = b3Add( b3RotateVector( turn[j], rig.restPosition[j] ), shift[j] );
			const b3Quat arc = anim::Arc( now, along );
			turn[j] = b3NormalizeQuat( b3MulQuat( arc, turn[j] ) );
			shift[j] = b3Add( b3RotateVector( arc, b3Sub( shift[j], pivot ) ), pivot );
		}
	}
	for ( size_t j = 0; j < joints; ++j )
	{
		rig.restPosition[j] = b3Add( b3RotateVector( turn[j], rig.restPosition[j] ), shift[j] );
		rig.restRotation[j] = b3NormalizeQuat( b3MulQuat( turn[j], rig.restRotation[j] ) );
	}
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

} // namespace cb::present
