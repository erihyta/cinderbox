#include "hit_test.h"

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
}

void HitTester::SetStances( const std::vector<std::string>& layers, const std::vector<std::string>& stances, std::string& warnings )
{
	m_pose.SetStances( anim::BuildStanceTable( *m_character->animations, layers, stances, warnings ) );
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
		m_pose.Evaluate( *state );
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
