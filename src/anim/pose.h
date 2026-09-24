#pragma once

// Turns a simulation AnimState into a skeleton pose with ozz (sample -> blend -> local-to-model).
// Uses only IEEE arithmetic and the scalar ozz build, so the same state gives the same pose on
// every platform; the server can run this too once gameplay needs bone positions.

#include "anim_set.h"
#include "components.h"
#include "stances.h"

#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"

#include <array>
#include <string>
#include <vector>

namespace cb::anim
{

struct ClipWeights
{
	std::array<float, ClipCount> weight{};
	std::array<float, ClipCount> ratio{}; // playback position in [0, 1]
};

// Blend weights and playback ratios for a state. Pure function of the state and clip lengths.
ClipWeights ComputeClipWeights( const AnimState& state, const AnimSet& set );

// A clip that is playing, for what presentation plays alongside the pose: companion tracks (VFX,
// sounds, lights, props) authored on the same timeline as the bones. One per channel: 0 is the base
// locomotion, 1 + l is stance layer l. Names are the baked clips': "walk", or "stance_<clip key>"
// ("stance_pistol", "stance_melee_walk"). Channels with nothing of their own playing are left out.
struct ActiveClip
{
	int channel = 0;
	std::string name;
	float time = 0.0f; // seconds into the clip
	bool loops = false;
};
std::vector<ActiveClip> ActiveClips( const AnimState& state, const AnimSet& set, const StanceTable* stances );

// Interpolate between two consecutive tick states for rendering between ticks.
AnimState InterpolateAnimState( const AnimState& from, const AnimState& to, float alpha );

class PoseEvaluator
{
public:
	explicit PoseEvaluator( const AnimSet& set );
	~PoseEvaluator();
	PoseEvaluator( const PoseEvaluator& ) = delete;
	PoseEvaluator& operator=( const PoseEvaluator& ) = delete;

	// Layers and stances for this character under a server's schema (BuildStanceTable); without
	// one, stances in the state are ignored.
	void SetStances( std::shared_ptr<const StanceTable> stances )
	{
		m_stances = std::move( stances );
	}

	void Evaluate( const AnimState& state );

	// Model-space joint matrices (skeleton space: feet at the origin, facing +Z), scale applied.
	const ozz::vector<ozz::math::Float4x4>& Models() const
	{
		return m_models;
	}
	const ClipWeights& LastWeights() const
	{
		return m_lastWeights;
	}
	const AnimSet& Set() const
	{
		return m_set;
	}

private:
	const AnimSet& m_set;
	std::array<ozz::animation::SamplingJob::Context, ClipCount> m_contexts;
	std::array<ozz::vector<ozz::math::SoaTransform>, ClipCount> m_locals;
	ozz::vector<ozz::math::SoaTransform> m_blended;
	ozz::vector<ozz::math::Float4x4> m_models;
	ClipWeights m_lastWeights;

	// Stance layers.
	void ApplyStance( int stance, int layer, float weight, float layerTime );
	std::shared_ptr<const StanceTable> m_stances;
	ozz::animation::SamplingJob::Context m_stanceContext;
	std::array<ozz::vector<ozz::math::SoaTransform>, ClipCount> m_stanceClipLocals;
	ozz::vector<ozz::math::SoaTransform> m_stanceLocals;
	ozz::vector<ozz::math::SoaTransform> m_scratch;
	ozz::vector<ozz::math::SimdFloat4> m_keepWeights;
	ozz::vector<ozz::math::SimdFloat4> m_stanceWeights;
};

} // namespace cb::anim
