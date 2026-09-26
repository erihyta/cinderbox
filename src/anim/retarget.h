#pragma once

// Clips made for one humanoid-profile skeleton, played on another (a mod's animation pack on any
// character). Joints are matched by their profile names (Hips, LeftUpperArm, ...).
//
// Each joint's rotation is taken as a turn from its rest, in its own frame, and applied to the other
// skeleton's rest of the same joint: that is what Godot's retargeting (rest fixer, "overwrite axis")
// makes meaningful, since every profile skeleton then shares the bones' axes. The hips' movement is
// scaled by the ratio of the two hips' heights; every other bone keeps the target's own lengths.
// Joints the source lacks stay at rest. Plain float math, the same on every machine: the server's hit
// tests and every client pose the same body.

#include "anim_graph.h"
#include "anim_set.h"

#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/memory/unique_ptr.h"

#include <memory>
#include <string>
#include <vector>

namespace cb::anim
{

// True when the two skeletons are the same (names, hierarchy, rest): a clip fits as it is.
bool SameSkeleton( const ozz::animation::Skeleton& a, const ozz::animation::Skeleton& b );

// `clip` (made for `from`) rebuilt for `to`, sampled at `sampleRate`. Null, with `error`, when the
// skeletons share no profile joints.
ozz::unique_ptr<ozz::animation::Animation> RetargetClip( const ozz::animation::Animation& clip, const ozz::animation::Skeleton& from,
														const ozz::animation::Skeleton& to, float sampleRate, std::string& error );

// A mod's animation pack (loaded like a character: its skeleton, its clips by name, its graph) fitted to
// one character: its graph's clips as that character plays them, index for index. Retargeted unless
// the skeletons match. Made once per character and shared by every player's pose.
struct PackClips
{
	std::vector<const ozz::animation::Animation*> clips; // per pack graph clip, null when missing
	std::vector<ozz::unique_ptr<ozz::animation::Animation>> owned;
	std::shared_ptr<const AnimSet> pack; // the clips it shares as they are live there
};
std::shared_ptr<const PackClips> FitPack( std::shared_ptr<const AnimSet> pack, const AnimGraph& graph, const AnimSet& character,
										  std::string& warnings );

} // namespace cb::anim
