#include "joint_math.h"

#include "ragdoll.h"
#include "types.h"

#include "profile.h"

#include "ozz/animation/runtime/skeleton.h"

#include <algorithm>
#include <cmath>
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

float EyeHeight( const AnimSet& set )
{
	int head = FindJoint( set, "Head" );
	if ( head < 0 || size_t( head ) >= set.RestModels().size() )
	{
		// A rig without a head: where the third-person camera's pivot is.
		return ragdoll::kFeetBelowCenter + kViewPivotHeight;
	}
	float v[4];
	ozz::math::StorePtrU( set.RestModels()[size_t( head )].cols[3], v );
	return v[1] + kEyeUp;
}

void RotateSubtree( const AnimSet& set, Models& models, int joint, b3Quat turn )
{
	if ( joint < 0 || joint >= int( models.size() ) )
	{
		return;
	}
	b3Vec3 pivot;
	b3Quat unused;
	float scale;
	Decompose( models[size_t( joint )], pivot, unused, scale );
	RotateSubtreeAbout( set, models, joint, pivot, turn );
}

void RotateSubtreeAbout( const AnimSet& set, Models& models, int joint, b3Vec3 pivot, b3Quat turn )
{
	const int joints = int( models.size() );
	if ( joint < 0 || joint >= joints )
	{
		return;
	}
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

void TranslateSubtree( const AnimSet& set, Models& models, int joint, b3Vec3 offset )
{
	const int joints = int( models.size() );
	if ( joint < 0 || joint >= joints )
	{
		return;
	}
	auto parents = set.Skeleton().joint_parents();
	std::vector<bool> below( size_t( joints ), false );
	below[size_t( joint )] = true;
	ozz::math::SimdFloat4 move = ozz::math::simd_float4::Load( offset.x, offset.y, offset.z, 0.0f );
	for ( int j = joint; j < joints; ++j )
	{
		int parent = parents[size_t( j )];
		below[size_t( j )] = j == joint || ( parent >= 0 && below[size_t( parent )] );
		if ( below[size_t( j )] )
		{
			models[size_t( j )].cols[3] = models[size_t( j )].cols[3] + move;
		}
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

void SolveGrip( const AnimSet& set, Models& models, const HandGrip& grip )
{
	const AnimSet::Arm& carrying = set.ArmJoints( grip.leftCarries );
	const AnimSet::Arm& arm = set.ArmJoints( grip.leftCarries == false );
	const int joints = int( models.size() );
	if ( carrying.hand < 0 || arm.upper < 0 || arm.lower < 0 || arm.hand < 0 || carrying.hand >= joints || arm.hand >= joints )
	{
		return;
	}
	// The item's frame: the carrying hand's socket.
	const AnimSet::HandSocket& socket = set.HandSocketOf( grip.leftCarries );
	const AnimSet::HandSocket& reaching = set.HandSocketOf( grip.leftCarries == false );
	b3Vec3 handAt;
	b3Quat handTurn;
	float unusedScale;
	Decompose( models[size_t( carrying.hand )], handAt, handTurn, unusedScale );
	b3Quat item = b3MulQuat( handTurn, socket.rotation );
	b3Vec3 target = b3Add( b3Add( handAt, b3RotateVector( handTurn, socket.position ) ), b3RotateVector( item, grip.position ) );
	// Aligned, the reaching hand's turn is known, and so is where its wrist is when its socket is
	// on the grip.
	b3Quat want = b3MulQuat( b3MulQuat( item, grip.rotation ),
							 b3Quat{ { -reaching.rotation.v.x, -reaching.rotation.v.y, -reaching.rotation.v.z }, reaching.rotation.s } );
	if ( grip.align )
	{
		target = b3Sub( target, b3RotateVector( want, reaching.position ) );
	}

	auto placeOf = [&]( int joint ) {
		b3Vec3 p;
		b3Quat q;
		float s;
		Decompose( models[size_t( joint )], p, q, s );
		return p;
	};
	b3Vec3 shoulder = placeOf( arm.upper );
	b3Vec3 elbow = placeOf( arm.lower );
	b3Vec3 wrist = placeOf( arm.hand );
	float upper = b3Distance( shoulder, elbow );
	float lower = b3Distance( elbow, wrist );
	if ( upper < 1e-4f || lower < 1e-4f )
	{
		return;
	}
	// As far as the arm reaches: not quite straight, not folded flat.
	b3Vec3 to = b3Sub( target, shoulder );
	float distance = b3Length( to );
	float reach = std::clamp( distance, std::fabs( upper - lower ) + 0.01f, ( upper + lower ) * 0.999f );
	if ( distance < 1e-4f )
	{
		return;
	}
	b3Vec3 along = b3MulSV( 1.0f / distance, to );
	target = b3MulAdd( shoulder, reach, along );

	// Where the elbow has to be: on the circle both bones allow, on the side it is on now (below
	// and outside the line to the grip, if the arm is straight along it).
	float alongShoulder = ( upper * upper - lower * lower + reach * reach ) / ( 2.0f * reach );
	float out = std::sqrt( std::max( upper * upper - alongShoulder * alongShoulder, 0.0f ) );
	b3Vec3 side = b3Sub( b3Sub( elbow, shoulder ), b3MulSV( b3Dot( b3Sub( elbow, shoulder ), along ), along ) );
	if ( b3LengthSquared( side ) < 1e-8f )
	{
		b3Vec3 down = { grip.leftCarries ? -0.5f : 0.5f, -1.0f, 0.0f };
		side = b3Sub( down, b3MulSV( b3Dot( down, along ), along ) );
		if ( b3LengthSquared( side ) < 1e-8f )
		{
			side = { 1.0f, 0.0f, 0.0f };
		}
	}
	side = b3Normalize( side );
	b3Vec3 bend = b3Add( shoulder, b3Add( b3MulSV( alongShoulder, along ), b3MulSV( out, side ) ) );

	RotateSubtreeAbout( set, models, arm.upper, shoulder, Arc( b3Normalize( b3Sub( elbow, shoulder ) ), b3Normalize( b3Sub( bend, shoulder ) ) ) );
	b3Vec3 moved = placeOf( arm.hand );
	RotateSubtreeAbout( set, models, arm.lower, bend, Arc( b3Normalize( b3Sub( moved, bend ) ), b3Normalize( b3Sub( target, bend ) ) ) );
	if ( grip.align )
	{
		// The hand as one carrying an item placed at the grip would be turned.
		b3Vec3 at;
		b3Quat now;
		Decompose( models[size_t( arm.hand )], at, now, unusedScale );
		RotateSubtreeAbout( set, models, arm.hand, at, b3MulQuat( want, b3Quat{ { -now.v.x, -now.v.y, -now.v.z }, now.s } ) );
	}
}

HandGrip AsAnimated( const AnimSet& set, const Models& models, bool leftCarries )
{
	HandGrip grip;
	grip.leftCarries = leftCarries;
	grip.align = true;
	const AnimSet::Arm& carrying = set.ArmJoints( leftCarries );
	const AnimSet::Arm& arm = set.ArmJoints( leftCarries == false );
	const int joints = int( models.size() );
	if ( carrying.hand < 0 || arm.hand < 0 || carrying.hand >= joints || arm.hand >= joints )
	{
		return grip;
	}
	// Both hands' sockets, and the other one's in the carrying one's frame (the item's).
	const AnimSet::HandSocket& socket = set.HandSocketOf( leftCarries );
	const AnimSet::HandSocket& reaching = set.HandSocketOf( leftCarries == false );
	b3Vec3 handAt, otherAt;
	b3Quat handTurn, otherTurn;
	float unusedScale;
	Decompose( models[size_t( carrying.hand )], handAt, handTurn, unusedScale );
	Decompose( models[size_t( arm.hand )], otherAt, otherTurn, unusedScale );
	b3Quat item = b3MulQuat( handTurn, socket.rotation );
	b3Quat back = { { -item.v.x, -item.v.y, -item.v.z }, item.s };
	b3Vec3 itemAt = b3Add( handAt, b3RotateVector( handTurn, socket.position ) );
	b3Vec3 palm = b3Add( otherAt, b3RotateVector( otherTurn, reaching.position ) );
	grip.position = b3RotateVector( back, b3Sub( palm, itemAt ) );
	grip.rotation = b3MulQuat( back, b3MulQuat( otherTurn, reaching.rotation ) );
	return grip;
}

} // namespace cb::anim
