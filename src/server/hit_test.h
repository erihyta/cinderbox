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
	// Where a joint of a player is this tick (humanoid-profile name, "Head"), from the same pose
	// the hit tests use. False for a player that is not there or a rig without the joint.
	bool JointPosition( Simulation& sim, PlayerSlot slot, const char* joint, b3Vec3& out );
	// How high above its feet the character's first-person camera sits (anim::EyeHeight).
	float EyeHeight() const
	{
		return m_eyeHeight;
	}

	// The character's state machine, the one the simulation runs.
	void SetGraph( std::shared_ptr<const AnimGraph> graph, std::string& warnings )
	{
		m_pose.SetGraph( std::move( graph ), warnings );
	}
	void SetPacks( AnimGraphPacks packs, std::vector<std::shared_ptr<const anim::PackClips>> clips )
	{
		m_pose.SetPacks( std::move( packs ), std::move( clips ) );
	}

	const CharacterAsset& Get() const
	{
		return *m_character;
	}

private:
	std::shared_ptr<const CharacterAsset> m_character;
	anim::PoseEvaluator m_pose;
	float m_reach = 2.5f; // radius around the feet that holds every pose, with margin
	float m_eyeHeight = 1.7f;
};

} // namespace cb
