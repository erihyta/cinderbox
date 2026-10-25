#include "hit_test.h"

#include "joint_math.h"
#include "mover.h"
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
		out.asAnimated = shape.grip == 3;
		out.position = { shape.gripPosition.x, shape.gripPosition.y, shape.gripPosition.z };
		out.rotation = { { shape.gripRotation[0], shape.gripRotation[1], shape.gripRotation[2] }, shape.gripRotation[3] };
		return true;
	}
	return false;
}

} // namespace

HitTester::HitTester( std::shared_ptr<const CharacterAsset> character )
{
	AddCharacter( std::move( character ) );
}

size_t HitTester::AddCharacter( std::shared_ptr<const CharacterAsset> character )
{
	Entry kind;
	kind.asset = std::move( character );
	kind.pose = std::make_unique<anim::PoseEvaluator>( *kind.asset->animations );
	// The rest pose's extent, doubled for the limbs' reach in motion, and the largest hitbox.
	float extent = 0.0f;
	for ( const ozz::math::Float4x4& m : kind.asset->animations->RestModels() )
	{
		float v[4];
		ozz::math::StorePtrU( m.cols[3], v );
		extent = std::max( extent, std::sqrt( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] ) );
	}
	float largest = 0.0f;
	for ( const anim::Hitbox& box : kind.asset->hitboxes.boxes )
	{
		largest = std::max( { largest, box.radius, box.height, box.halfExtents.x, box.halfExtents.y, box.halfExtents.z } );
	}
	kind.reach = extent * 1.5f + largest * kind.asset->animations->Scale() + 0.25f;
	kind.eyeHeight = anim::EyeHeight( *kind.asset->animations );
	m_kinds.push_back( std::move( kind ) );
	return m_kinds.size() - 1;
}

void HitTester::SetGraph( size_t character, std::shared_ptr<const AnimGraph> graph, std::string& warnings )
{
	if ( character < m_kinds.size() )
	{
		m_kinds[character].pose->SetGraph( std::move( graph ), warnings );
	}
}

void HitTester::SetPacks( size_t character, AnimGraphPacks packs, std::vector<std::shared_ptr<const anim::PackClips>> clips )
{
	if ( character < m_kinds.size() )
	{
		m_kinds[character].pose->SetPacks( std::move( packs ), std::move( clips ) );
	}
}

bool HitTester::JointPosition( Simulation& sim, PlayerSlot slot, const char* joint, b3Vec3& out )
{
	uint32_t netId = sim.PlayerNetId( slot );
	const Transform* transform = netId != 0 ? sim.EntityTransform( netId ) : nullptr;
	AnimStateCopy state = netId != 0 ? sim.EntityAnimState( netId ) : AnimStateCopy();
	const uint16_t character = netId != 0 ? sim.CharacterOf( sim.FindEntity( netId ) ) : uint16_t( 0 );
	Entry& kind = Kind( character );
	int index = anim::FindJoint( *kind.asset->animations, joint );
	if ( transform == nullptr || state == nullptr || index < 0 )
	{
		return false;
	}
	anim::HandGrip grip;
	kind.pose->Evaluate( *state, GripOf( sim, netId, grip ) ? &grip : nullptr );
	float v[4];
	ozz::math::StorePtrU( kind.pose->Models()[size_t( index )].cols[3], v );
	// Model space has the feet at the origin and faces +Z, like the hitboxes.
	const Simulation::CharacterBody& body = sim.Body( character );
	b3Vec3 feet = b3Sub( transform->position, b3Vec3{ 0.0f, mover::FeetBelowCenter( body.radius, body.halfHeight ), 0.0f } );
	out = b3Add( feet, b3RotateVector( transform->rotation, b3Vec3{ v[0], v[1], v[2] } ) );
	return true;
}

bool HitTester::BakedEyePosition( Simulation& sim, PlayerSlot slot, b3Vec3& out )
{
	uint32_t netId = sim.PlayerNetId( slot );
	const Transform* transform = netId != 0 ? sim.EntityTransform( netId ) : nullptr;
	AnimStateCopy state = netId != 0 ? sim.EntityAnimState( netId ) : AnimStateCopy();
	const uint16_t character = netId != 0 ? sim.CharacterOf( sim.FindEntity( netId ) ) : uint16_t( 0 );
	Entry& kind = Kind( character );
	if ( transform == nullptr || state == nullptr || kind.asset->animations->EyeJoint() < -1 )
	{
		return false;
	}
	anim::HandGrip grip;
	kind.pose->Evaluate( *state, GripOf( sim, netId, grip ) ? &grip : nullptr );
	b3Vec3 eye;
	if ( anim::EyePoint( *kind.asset->animations, kind.pose->Models(), eye ) == false )
	{
		return false;
	}
	const Simulation::CharacterBody& body = sim.Body( character );
	b3Vec3 feet = b3Sub( transform->position, b3Vec3{ 0.0f, mover::FeetBelowCenter( body.radius, body.halfHeight ), 0.0f } );
	out = b3Add( feet, b3RotateVector( transform->rotation, eye ) );
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
		AnimStateCopy state = sim.EntityAnimState( netId );
		if ( character == nullptr || character->dead || transform == nullptr || state == nullptr )
		{
			continue;
		}
		// As its own character: its feet, its reach, its pose, its hitboxes.
		const uint16_t which = sim.CharacterOf( sim.FindEntity( netId ) );
		Entry& kind = Kind( which );
		const Simulation::CharacterBody& body = sim.Body( which );
		b3Vec3 feet = b3Sub( transform->position, b3Vec3{ 0.0f, mover::FeetBelowCenter( body.radius, body.halfHeight ), 0.0f } );
		if ( NearRay( origin, translation, limit, feet, kind.reach ) == false )
		{
			continue;
		}
		anim::HandGrip grip;
		kind.pose->Evaluate( *state, GripOf( sim, netId, grip ) ? &grip : nullptr );
		anim::HitboxHit boxHit;
		if ( anim::RayHitboxes( kind.asset->hitboxes, kind.pose->Models(), feet, transform->rotation, origin, translation, limit, boxHit ) )
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
