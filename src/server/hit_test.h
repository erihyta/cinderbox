#pragma once

// Server ray casts that hit players by their character's hitboxes instead of their capsules.
//
// The world (level, props, ragdolls) is cast against as usual with players left out; then every
// live player the ray passes near is posed from its AnimState and its hitboxes are tested, and the
// closest hit in front of the world's wins. Only the server does this (mods run there), so nothing
// here is part of the rolled-back simulation.

#include "character_item.h"
#include "pose.h"
#include "simulation.h"

#include <memory>

namespace cb
{

class HitTester
{
public:
	explicit HitTester( std::shared_ptr<const CharacterAsset> character );

	bool CastRay( Simulation& sim, b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit );

	// The mods' layers and stances, resolved for the character; warnings for what it lacks.
	void SetStances( const std::vector<std::string>& layers, const std::vector<std::string>& stances, std::string& warnings );
	// The character's state machine, the one the simulation runs.
	void SetGraph( std::shared_ptr<const AnimGraph> graph, std::string& warnings )
	{
		m_pose.SetGraph( std::move( graph ), warnings );
	}

	const CharacterAsset& Get() const
	{
		return *m_character;
	}

private:
	std::shared_ptr<const CharacterAsset> m_character;
	anim::PoseEvaluator m_pose;
	float m_reach = 2.5f; // radius around the feet that holds every pose, with margin
};

} // namespace cb
