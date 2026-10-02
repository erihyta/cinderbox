#pragma once

// Turns a simulation AnimState into a skeleton pose with ozz: each layer of the character's state
// machine samples its state's clips and blends them (sample -> blend -> local-to-model), then the
// legs turn, the body faces and the aim chain points. Uses only IEEE arithmetic and the scalar ozz
// build, so the same state gives the same pose on every platform: clients draw it and the server
// hit-tests it.

#include "anim_graph.h"
#include "anim_set.h"
#include "components.h"
#include "retarget.h"

#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"

#include <array>
#include <string>
#include <vector>

namespace cb::anim
{

// A clip that is playing, for what presentation plays alongside the pose: the animation's other
// tracks (VFX, sounds, lights, props), authored on the same timeline as the bones. One per layer
// of the state machine (channel = layer): the clip of its state that weighs most, by the Godot
// animation's name. Layers with no weight are left out.
struct ActiveClip
{
	int channel = 0;
	std::string name;
	float time = 0.0f; // seconds into the clip
	bool loops = false;
};
std::vector<ActiveClip> ActiveClips( const AnimState& state, const AnimGraph& graph, const AnimGraphPacks& packs = {} );

// Interpolate between two consecutive tick states for rendering between ticks.
AnimState InterpolateAnimState( const AnimState& from, const AnimState& to, float alpha );

class PoseEvaluator
{
public:
	explicit PoseEvaluator( const AnimSet& set );
	~PoseEvaluator();
	PoseEvaluator( const PoseEvaluator& ) = delete;
	PoseEvaluator& operator=( const PoseEvaluator& ) = delete;

	// The character's state machine, as the simulation runs it (the schema's, compiled): the pose
	// follows its layers. Clips it names that this character lacks drop out of the blend and are
	// listed in `warnings`. Without one the pose is the skeleton's rest.
	void SetGraph( std::shared_ptr<const AnimGraph> graph, std::string& warnings );
	// The mods' animation packs (as the simulation has them) and their clips fitted to this
	// character (FitPack), index for index: layers a player swapped to one play from it.
	void SetPacks( AnimGraphPacks packs, std::vector<std::shared_ptr<const PackClips>> clips );
	const AnimGraph* Graph() const
	{
		return m_graph.get();
	}

	void Evaluate( const AnimState& state );

	// Model-space joint matrices (skeleton space: feet at the origin, facing +Z), scale applied.
	const ozz::vector<ozz::math::Float4x4>& Models() const
	{
		return m_models;
	}
	const AnimSet& Set() const
	{
		return m_set;
	}

private:
	const AnimSet& m_set;
	ozz::vector<ozz::math::SoaTransform> m_blended;
	ozz::vector<ozz::math::Float4x4> m_models;

	void Finish( const AnimState& state );
	// Blends `pose` over m_blended by per-joint `mask` (empty: every joint) times `weight`.
	void BlendOver( const ozz::vector<ozz::math::SoaTransform>& pose, const ozz::vector<ozz::math::SimdFloat4>* mask, float weight );

	// State machine layers.
	void EvaluateGraph( const AnimState& state );
	std::shared_ptr<const AnimGraph> m_graph;
	AnimGraphPacks m_packs;
	std::vector<std::shared_ptr<const PackClips>> m_packClips;
	// Per graph that layers play from ([0] the character's, [n] pack n-1's): its clips, and per
	// layer its mask (empty: every joint) and how much of it covers the neck.
	struct GraphSource
	{
		const AnimGraph* graph = nullptr;
		std::vector<const ozz::animation::Animation*> clips;
		std::vector<ozz::vector<ozz::math::SimdFloat4>> masks;
		std::vector<float> neck;
	};
	std::vector<GraphSource> m_sources;
	void BindSource( GraphSource& source, const AnimGraph& graph, std::vector<const ozz::animation::Animation*> clips );
	std::vector<std::unique_ptr<ozz::animation::SamplingJob::Context>> m_graphContexts;
	std::vector<ozz::vector<ozz::math::SoaTransform>> m_graphLocals;
	ozz::vector<ozz::math::SoaTransform> m_layerPose;
	float m_neckCover = 0.0f;			// this pose: how much of the neck the upper layers set
	ozz::vector<ozz::math::SoaTransform> m_scratch;
	ozz::vector<ozz::math::SimdFloat4> m_keepWeights;
	ozz::vector<ozz::math::SimdFloat4> m_layerWeights;
};

} // namespace cb::anim
