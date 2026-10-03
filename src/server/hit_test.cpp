#include "hit_test.h"

#include "joint_math.h"
#include "ragdoll.h"

#include "ozz/base/maths/simd_math.h"

#include <algorithm>
#include <cmath>

namespace cb
{

namespace
{

// Does the ray come within `radius` of `center` before `maxFraction`?
bool NearRay( b3Vec3 origin, b3Vec3 translation, float maxFraction, b3Vec3 center, float radius )
{
	float lengthSq = b3Dot( translation, translation );
	if ( lengthSq <= 0.0f )
	{
		return false;
	}
	b3Vec3 toCenter = b3Sub( center, origin );
	float t = std::clamp( b3Dot( toCenter, translation ) / lengthSq, 0.0f, maxFraction );
	b3Vec3 closest = b3MulAdd( origin, t, translation );
	b3Vec3 gap = b3Sub( center, closest );
	return b3Dot( gap, gap ) <= radius * radius;
}

// The item a player carries in one hand and holds with the other too, if it has one.
bool GripOf( Simulation& sim, uint32_t netId, anim::HandGrip& out )
{
	for ( uint32_t socket : { uint32_t( kSocketRightHand ), uint32_t( kSocketLeftHand ) } )
	{
		uint32_t other = socket == kSocketRightHand ? uint32_t( kSocketLeftHand ) : uint32_t( kSocketRightHand );
		uint32_t item = sim.HeldItemOf( netId, socket );
		flecs::entity e = item != 0 && sim.HeldItemOf( netId, other ) == 0 ? sim.FindEntity( item ) : flecs::entity();
		if ( e.is_valid() == false || e.has<HeldItem>() == false )
		{
			continue;
		}
		ItemShape shape = sim.ItemShapeOf( e.get<HeldItem>().kind );
		if ( shape.grip == 0 )
		{
			continue;
		}
		out.leftCarries = socket == kSocketLeftHand;
		out.align = shape.grip == 2;
		out.position = { shape.gripPosition.x, shape.gripPosition.y, shape.gripPosition.z };
		out.rotation = { { shape.gripRotation[0], shape.gripRotation[1], shape.gripRotation[2] }, shape.gripRotation[3] };
		return true;
	}
	return false;
}

} // namespace

HitTester::HitTester( std::shared_ptr<const CharacterAsset> character )
	: m_character( std::move( character ) )
	, m_pose( *m_character->animations )
{
	// The rest pose's extent, doubled for the limbs' reach in motion, and the largest hitbox.
	float extent = 0.0f;
	for ( const ozz::math::Float4x4& m : m_character->animations->RestModels() )
	{
		float v[4];
		ozz::math::StorePtrU( m.cols[3], v );
		extent = std::max( extent, std::sqrt( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] ) );
	}
	float largest = 0.0f;
	for ( const anim::Hitbox& box : m_character->hitboxes.boxes )
	{
		largest = std::max( { largest, box.radius, box.height, box.halfExtents.x, box.halfExtents.y, box.halfExtents.z } );
	}
	m_reach = extent * 1.5f + largest * m_character->animations->Scale() + 0.25f;
	m_eyeHeight = anim::EyeHeight( *m_character->animations );
}

bool HitTester::JointPosition( Simulation& sim, PlayerSlot slot, const char* joint, b3Vec3& out )
{
	uint32_t netId = sim.PlayerNetId( slot );
	const Transform* transform = netId != 0 ? sim.EntityTransform( netId ) : nullptr;
	const AnimState* state = netId != 0 ? sim.EntityAnimState( netId ) : nullptr;
	int index = anim::FindJoint( *m_character->animations, joint );
	if ( transform == nullptr || state == nullptr || index < 0 )
	{
		return false;
	}
	anim::HandGrip grip;
	m_pose.Evaluate( *state, GripOf( sim, netId, grip ) ? &grip : nullptr );
	float v[4];
	ozz::math::StorePtrU( m_pose.Models()[size_t( index )].cols[3], v );
	// Model space has the feet at the origin and faces +Z, like the hitboxes.
	b3Vec3 feet = b3Sub( transform->position, b3Vec3{ 0.0f, ragdoll::kFeetBelowCenter, 0.0f } );
	out = b3Add( feet, b3RotateVector( transform->rotation, b3Vec3{ v[0], v[1], v[2] } ) );
	return true;
}

bool HitTester::CastRay( Simulation& sim, b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit )
{
	bool found = sim.CastRay( origin, translation, ignoreNetId, hit, true );
	float limit = found ? hit.fraction : 1.0f;
	for ( int i = 0; i < kMaxPlayers; ++i )
	{
		PlayerSlot slot = PlayerSlot( i );
		uint32_t netId = sim.PlayerNetId( slot );
		if ( netId == 0 || netId == ignoreNetId )
		{
			continue;
		}
		const Character* character = sim.PlayerCharacter( slot );
		const Transform* transform = sim.EntityTransform( netId );
		const AnimState* state = sim.EntityAnimState( netId );
		if ( character == nullptr || character->dead || transform == nullptr || state == nullptr )
		{
			continue;
		}
		b3Vec3 feet = b3Sub( transform->position, b3Vec3{ 0.0f, ragdoll::kFeetBelowCenter, 0.0f } );
		if ( NearRay( origin, translation, limit, feet, m_reach ) == false )
		{
			continue;
		}
		anim::HandGrip grip;
		m_pose.Evaluate( *state, GripOf( sim, netId, grip ) ? &grip : nullptr );
		anim::HitboxHit boxHit;
		if ( anim::RayHitboxes( m_character->hitboxes, m_pose.Models(), feet, transform->rotation, origin, translation, limit, boxHit ) )
		{
			limit = boxHit.fraction;
			hit.netId = netId;
			hit.point = boxHit.point;
			hit.normal = boxHit.normal;
			hit.fraction = boxHit.fraction;
			hit.zone = boxHit.box->zone.c_str();
			found = true;
		}
	}
	return found;
}

} // namespace cb
