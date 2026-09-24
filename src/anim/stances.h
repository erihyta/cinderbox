#pragma once

// Animation layers and stances, resolved for one character against what a server's mods declared.
//
// A layer is a bone mask the character defines (anim.cfg "mask.upper = Spine"; "full" is every bone
// unless the character says otherwise, and "upper" defaults to the Spine and everything above it).
// A stance is a named clip set the character may ship: "<stance>_idle", "<stance>_walk", ... for
// any of the six clips, or one looping "<stance>" clip used for all of them. Whatever a stance lacks
// falls back to the default clips, so a character without a stance simply keeps its normal
// animation there.
//
// Mods set a stance per layer (AnimState::stances); the pose blends each layer's stance over the
// layers below it, by the mask's per-joint weights, fading over kStanceFadeSeconds.

#include "anim_set.h"

#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace cb::anim
{

struct StanceTable
{
	struct Stance
	{
		std::string name; // the schema's
		// Per clip slot: the stance's own clip, or null to use the default one.
		std::array<const ozz::animation::Animation*, ClipCount> clips{};
		// A single looping clip for the whole stance (played by the layer's time), or null.
		const ozz::animation::Animation* single = nullptr;
	};

	// Per schema layer: per-joint weights packed like ozz's SoA transforms; empty when the
	// character has no mask for the layer (the layer then does nothing).
	std::vector<ozz::vector<ozz::math::SimdFloat4>> masks;
	// Per schema stance.
	std::vector<Stance> stances;
};

// `layers` and `stances` are the schema's names, in its order. Layers the character cannot mask and
// stances it has no clips for are reported in `warnings` (they fall back, nothing breaks).
std::shared_ptr<const StanceTable> BuildStanceTable( const AnimSet& set, const std::vector<std::string>& layers,
													 const std::vector<std::string>& stances, std::string& warnings );

// Per-joint weights of a mask description ("Spine", "Spine:0.5 RightShoulder:1"): each root sets
// its whole subtree, later roots override earlier ones. Joints by humanoid-profile name.
std::vector<float> MaskWeights( const AnimSet& set, const std::string& roots, std::string& warnings );

} // namespace cb::anim
