#pragma once

// Server ray casts that hit players by their character's hitboxes instead of their capsules.
//
// The world (level, props, ragdolls) is cast against as usual with players left out; then every
// live player the ray passes near is posed from its AnimState and its hitboxes are tested, and the
// closest hit in front of the world's wins. Each body is posed and tested as its own character
// (Simulation::CharacterOf): its skeleton, its state machine, its hitboxes. Only the server does
// this (mods run there), so nothing here is part of the rolled-back simulation.

#include "character_item.h"
#include "pose.h"
#include "simulation.h"

#include <memory>
#include <vector>

namespace cb
{

class HitTester
{
public:
	// Character 0, the server's own.
	explicit HitTester( std::shared_ptr<const CharacterAsset> character );
	// The next character (ModSchema::characters, in its order). Returns its index.
	size_t AddCharacter( std::shared_ptr<const CharacterAsset> character );

	bool CastRay( Simulation& sim, b3Vec3 origin, b3Vec3 translation, uint32_t ignoreNetId, RayHit& hit );
	// Where a joint of a player is this tick (humanoid-profile name, "Head"), from the same pose
	// the hit tests use. False for a player that is not there or a rig without the joint.
	bool JointPosition( Simulation& sim, PlayerSlot slot, const char* joint, b3Vec3& out );
	// How high above its feet a character's first-person camera sits (anim::EyeHeight).
	float EyeHeight( size_t character = 0 ) const
	{
		return Kind( character ).eyeHeight;
	}

	// A character's state machine (the one the simulation runs for its bodies), and the animation
	// packs fitted to its skeleton.
	void SetGraph( std::shared_ptr<const AnimGraph> graph, std::string& warnings )
	{
		SetGraph( 0, std::move( graph ), warnings );
	}
	void SetGraph( size_t character, std::shared_ptr<const AnimGraph> graph, std::string& warnings );
	void SetPacks( AnimGraphPacks packs, std::vector<std::shared_ptr<const anim::PackClips>> clips )
	{
		SetPacks( 0, std::move( packs ), std::move( clips ) );
	}
	void SetPacks( size_t character, AnimGraphPacks packs, std::vector<std::shared_ptr<const anim::PackClips>> clips );

	const CharacterAsset& Get( size_t character = 0 ) const
	{
		return *Kind( character ).asset;
	}
	size_t Characters() const
	{
		return m_kinds.size();
	}

private:
	struct Entry
	{
		std::shared_ptr<const CharacterAsset> asset;
		std::unique_ptr<anim::PoseEvaluator> pose;
		float reach = 2.5f; // radius around the feet that holds every pose, with margin
		float eyeHeight = 1.7f;
	};
	const Entry& Kind( size_t character ) const
	{
		return m_kinds[character < m_kinds.size() ? character : 0];
	}
	Entry& Kind( size_t character )
	{
		return m_kinds[character < m_kinds.size() ? character : 0];
	}
	std::vector<Entry> m_kinds;
};

} // namespace cb
