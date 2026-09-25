#include "pose.h"

#include "anim_controller.h"
#include "detmath.h"
#include "joint_math.h"

#include "ozz/animation/runtime/blending_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/base/span.h"

#include <algorithm>
#include <cmath>

namespace cb::anim
{

using namespace anim_tuning;

namespace
{

constexpr float kMinWeight = 0.001f;

int ClipOf( AnimMode mode )
{
	switch ( mode )
	{
		case AnimMode::JumpStart:
			return ClipJumpStart;
		case AnimMode::Fall:
			return ClipFall;
		case AnimMode::Land:
			return ClipLand;
		case AnimMode::Locomotion:
		default:
			return -1;
	}
}

// Wrapped playback position. fmod is exact in IEEE 754, so this is deterministic.
float LoopRatio( float time, float duration )
{
	if ( duration <= 0.0f )
	{
		return 0.0f;
	}
	return std::fmod( time, duration ) / duration;
}

float OnceRatio( float time, float duration )
{
	if ( duration <= 0.0f )
	{
		return 1.0f;
	}
	return std::clamp( time / duration, 0.0f, 1.0f );
}

void AddModeWeight( ClipWeights& out, AnimMode mode, float weight, float groundSpeed )
{
	int clip = ClipOf( mode );
	if ( clip >= 0 )
	{
		out.weight[clip] += weight;
		return;
	}

	// Locomotion: 1D blend idle -> walk -> run.
	if ( groundSpeed <= kWalkSpeed )
	{
		float a = groundSpeed / kWalkSpeed;
		out.weight[ClipIdle] += weight * ( 1.0f - a );
		out.weight[ClipWalk] += weight * a;
	}
	else if ( groundSpeed < kRunSpeed )
	{
		float b = ( groundSpeed - kWalkSpeed ) / ( kRunSpeed - kWalkSpeed );
		out.weight[ClipWalk] += weight * ( 1.0f - b );
		out.weight[ClipRun] += weight * b;
	}
	else
	{
		out.weight[ClipRun] += weight;
	}
}

} // namespace

ClipWeights ComputeClipWeights( const AnimState& s, const AnimSet& set )
{
	ClipWeights w;
	float fade = std::min( s.modeTime / kModeFadeSeconds, 1.0f );
	AddModeWeight( w, s.mode, fade, s.groundSpeed );
	if ( fade < 1.0f )
	{
		AddModeWeight( w, s.previousMode, 1.0f - fade, s.groundSpeed );
	}

	w.ratio[ClipIdle] = LoopRatio( s.idleTime, set.Duration( ClipIdle ) );
	// Walking backwards: the same cycle, played in reverse.
	float phase = s.legsBackward != 0 && s.locomotionPhase > 0.0f ? 1.0f - s.locomotionPhase : s.locomotionPhase;
	w.ratio[ClipWalk] = phase;
	w.ratio[ClipRun] = phase;
	// A mode that is fading out holds its last pose.
	w.ratio[ClipJumpStart] = s.mode == AnimMode::JumpStart ? OnceRatio( s.modeTime, set.Duration( ClipJumpStart ) ) : 1.0f;
	w.ratio[ClipLand] = s.mode == AnimMode::Land ? OnceRatio( s.modeTime, set.Duration( ClipLand ) ) : 1.0f;
	w.ratio[ClipFall] = s.mode == AnimMode::Fall ? LoopRatio( s.modeTime, set.Duration( ClipFall ) ) : 0.0f;
	return w;
}

std::vector<ActiveClip> ActiveClips( const AnimState& state, const AnimSet& set, const StanceTable* stances )
{
	std::vector<ActiveClip> out;
	ClipWeights w = ComputeClipWeights( state, set );
	int dominant = 0;
	for ( int c = 1; c < ClipCount; ++c )
	{
		if ( w.weight[c] > w.weight[dominant] )
		{
			dominant = c;
		}
	}
	auto loops = []( int c ) { return c == ClipIdle || c == ClipWalk || c == ClipRun || c == ClipFall; };
	out.push_back( { 0, ClipName( Clip( dominant ) ), w.ratio[dominant] * set.Duration( Clip( dominant ) ), loops( dominant ) } );

	if ( stances == nullptr )
	{
		return out;
	}
	for ( int l = 0; l < kMaxAnimLayers && l < int( stances->masks.size() ); ++l )
	{
		int current = int( state.stances[l] ) - 1;
		if ( current < 0 || current >= int( stances->stances.size() ) || stances->masks[size_t( l )].empty() )
		{
			continue;
		}
		const StanceTable::Stance& st = stances->stances[size_t( current )];
		bool perClip = false;
		for ( const auto* clip : st.clips )
		{
			perClip |= clip != nullptr;
		}
		if ( perClip )
		{
			// The stance's own version of the dominant clip; if it has none, the base plays it.
			const ozz::animation::Animation* own = st.clips[size_t( dominant )];
			if ( own != nullptr )
			{
				out.push_back( { 1 + l, "stance_" + st.name + "_" + ClipName( Clip( dominant ) ), w.ratio[dominant] * own->duration(),
								 loops( dominant ) } );
			}
		}
		else if ( st.single != nullptr )
		{
			float duration = st.single->duration();
			out.push_back( { 1 + l, "stance_" + st.name, LoopRatio( state.layerTime[l], duration ) * duration, true } );
		}
	}
	return out;
}

std::vector<ActiveClip> ActiveGraphClips( const AnimState& state, const AnimGraph& graph )
{
	std::vector<ActiveClip> out;
	for ( size_t l = 0; l < graph.layers.size() && l < size_t( kMaxAnimLayers ); ++l )
	{
		const AnimGraphLayer& layer = graph.layers[l];
		const AnimGraphLayerState& L = state.graph[l];
		if ( L.state >= layer.states.size() || ( l > 0 && L.weight <= 0.0f ) )
		{
			continue;
		}
		const AnimGraphState& s = layer.states[L.state];
		AnimBlendWeights weights;
		AnimGraphWeights( s, L.blend, L.blendY, weights );
		size_t best = 0;
		for ( size_t p = 1; p < s.points.size(); ++p )
		{
			if ( weights[p] > weights[best] )
			{
				best = p;
			}
		}
		const AnimGraphClip& clip = graph.clips[size_t( s.points[best].clip )];
		float time = s.blend ? L.time * clip.length : L.time;
		if ( s.points[best].backward )
		{
			time = clip.length - time;
		}
		out.push_back( { int( l ), clip.name, time, clip.loops } );
	}
	return out;
}

AnimState InterpolateAnimState( const AnimState& from, const AnimState& to, float alpha )
{
	AnimState s = to;
	float t = std::clamp( alpha, 0.0f, 1.0f );
	s.groundSpeed = from.groundSpeed + ( to.groundSpeed - from.groundSpeed ) * t;
	s.moveForward = from.moveForward + ( to.moveForward - from.moveForward ) * t;
	s.moveRight = from.moveRight + ( to.moveRight - from.moveRight ) * t;

	// Extrapolate forward from `from` instead of lerping across wraps and mode switches.
	if ( from.mode == to.mode )
	{
		s.modeTime = from.modeTime + ( to.modeTime - from.modeTime ) * t;
	}
	float dPhase = to.locomotionPhase - from.locomotionPhase;
	if ( dPhase < 0.0f )
	{
		dPhase += 1.0f;
	}
	s.locomotionPhase = from.locomotionPhase + dPhase * t;
	if ( s.locomotionPhase >= 1.0f )
	{
		s.locomotionPhase -= 1.0f;
	}
	// Aim: the short way round, so a turn across the back does not swing through the front.
	s.aimYaw = detmath::WrapAngle( from.aimYaw + detmath::WrapAngle( to.aimYaw - from.aimYaw ) * t );
	s.aimPitch = from.aimPitch + ( to.aimPitch - from.aimPitch ) * t;
	s.legYaw = from.legYaw + ( to.legYaw - from.legYaw ) * t;
	for ( int l = 0; l < kMaxAnimLayers; ++l )
	{
		if ( from.stances[l] == to.stances[l] && to.layerTime[l] >= from.layerTime[l] )
		{
			s.layerTime[l] = from.layerTime[l] + ( to.layerTime[l] - from.layerTime[l] ) * t;
		}
	}
	for ( int l = 0; l < kMaxAnimLayers; ++l )
	{
		// A state machine layer: its clocks move on within a state; a switch shows the new state.
		const AnimGraphLayerState& a = from.graph[l];
		const AnimGraphLayerState& b = to.graph[l];
		AnimGraphLayerState& o = s.graph[l];
		o.weight = a.weight + ( b.weight - a.weight ) * t;
		if ( a.started == 0 || a.state != b.state || b.stateTime < a.stateTime )
		{
			continue;
		}
		o.stateTime = a.stateTime + ( b.stateTime - a.stateTime ) * t;
		o.blend = a.blend + ( b.blend - a.blend ) * t;
		o.blendY = a.blendY + ( b.blendY - a.blendY ) * t;
		if ( b.time >= a.time )
		{
			o.time = a.time + ( b.time - a.time ) * t;
		}
		if ( a.previous == b.previous && b.previousTime >= a.previousTime )
		{
			o.previousTime = a.previousTime + ( b.previousTime - a.previousTime ) * t;
		}
	}
	float dIdle = to.idleTime - from.idleTime;
	if ( dIdle < 0.0f )
	{
		dIdle += kTimeWrap;
	}
	s.idleTime = from.idleTime + dIdle * t;
	return s;
}

PoseEvaluator::PoseEvaluator( const AnimSet& set )
	: m_set( set )
{
	const auto& skeleton = set.Skeleton();
	int soaJoints = skeleton.num_soa_joints();
	for ( int c = 0; c < ClipCount; ++c )
	{
		m_contexts[c].Resize( skeleton.num_joints() );
		m_locals[c].resize( soaJoints );
	}
	m_blended.resize( soaJoints );
	m_models.resize( skeleton.num_joints() );
	m_stanceContext.Resize( skeleton.num_joints() );
	for ( auto& locals : m_stanceClipLocals )
	{
		locals.resize( soaJoints );
	}
	m_stanceLocals.resize( soaJoints );
	m_scratch.resize( soaJoints );
	m_keepWeights.resize( soaJoints );
	m_stanceWeights.resize( soaJoints );
	m_layerPose.resize( soaJoints );
}

void PoseEvaluator::SetGraph( std::shared_ptr<const AnimGraph> graph, std::string& warnings )
{
	m_graph = std::move( graph );
	m_graphClips.clear();
	m_graphMasks.clear();
	m_graphNeckMask.clear();
	if ( !m_graph )
	{
		return;
	}
	const auto& skeleton = m_set.Skeleton();
	for ( const AnimGraphClip& clip : m_graph->clips )
	{
		const ozz::animation::Animation* own = m_set.NamedClip( clip.name );
		if ( own == nullptr )
		{
			warnings += "the state machine plays '" + clip.name + "', which this character has no clip for; ";
		}
		m_graphClips.push_back( own );
	}
	// Masks: exactly the bones the layer's Blend2 filter lists, as Godot filters them.
	const int joints = skeleton.num_joints();
	auto names = skeleton.joint_names();
	for ( const AnimGraphLayer& layer : m_graph->layers )
	{
		ozz::vector<ozz::math::SimdFloat4> packed;
		if ( layer.mask.empty() == false )
		{
			std::vector<float> weights( size_t( joints ), 0.0f );
			for ( const std::string& bone : layer.mask )
			{
				int j = FindJoint( m_set, bone.c_str() );
				for ( int k = 0; k < joints && j < 0; ++k )
				{
					j = bone == names[size_t( k )] ? k : -1; // a bone the profile does not name
				}
				if ( j >= 0 )
				{
					weights[size_t( j )] = 1.0f;
				}
			}
			packed.resize( size_t( skeleton.num_soa_joints() ) );
			for ( int i = 0; i < skeleton.num_soa_joints(); ++i )
			{
				float lane[4];
				for ( int k = 0; k < 4; ++k )
				{
					int j = i * 4 + k;
					lane[k] = j < joints ? weights[size_t( j )] : 0.0f;
				}
				packed[size_t( i )] = ozz::math::simd_float4::Load( lane[0], lane[1], lane[2], lane[3] );
			}
		}
		m_graphMasks.push_back( std::move( packed ) );
		float neck = 1.0f;
		if ( layer.mask.empty() == false )
		{
			neck = 0.0f;
			for ( const std::string& bone : layer.mask )
			{
				neck = bone == "Neck" ? 1.0f : neck;
			}
		}
		m_graphNeckMask.push_back( neck );
	}
	// Sample buffers: every point of a state and of the one fading out.
	size_t most = 1;
	for ( const AnimGraphLayer& layer : m_graph->layers )
	{
		for ( const AnimGraphState& state : layer.states )
		{
			most = std::max( most, state.points.size() );
		}
	}
	while ( m_graphContexts.size() < 2 * most )
	{
		m_graphContexts.push_back( std::make_unique<ozz::animation::SamplingJob::Context>( joints ) );
		m_graphLocals.emplace_back( size_t( skeleton.num_soa_joints() ) );
	}
}

void PoseEvaluator::EvaluateGraph( const AnimState& state )
{
	const auto& skeleton = m_set.Skeleton();
	const AnimGraph& graph = *m_graph;
	std::vector<ozz::animation::BlendingJob::Layer> layers;
	float neckKept = 1.0f;
	for ( size_t l = 0; l < graph.layers.size() && l < size_t( kMaxAnimLayers ); ++l )
	{
		const AnimGraphLayer& layer = graph.layers[l];
		AnimGraphLayerState L = state.graph[l];
		if ( L.started == 0 || L.state >= layer.states.size() || L.previous >= layer.states.size() )
		{
			L = AnimGraphLayerState{};
			L.state = L.previous = uint8_t( layer.start );
			L.weight = l == 0 ? 1.0f : 0.0f;
		}
		if ( l > 0 && L.weight <= 0.0f )
		{
			continue;
		}

		// The state, crossfading from the one before it.
		float in = L.fadeLength > 0.0f ? std::min( L.stateTime / L.fadeLength, 1.0f ) : 1.0f;
		layers.clear();
		size_t buffer = 0;
		auto sample = [&]( const AnimGraphState& s, float time, float blend, float blendY, float weight ) {
			AnimBlendWeights weights;
			AnimGraphWeights( s, blend, blendY, weights );
			for ( size_t p = 0; p < s.points.size() && buffer < m_graphLocals.size(); ++p )
			{
				float w = weight * weights[p];
				const AnimGraphState::Point& point = s.points[p];
				const ozz::animation::Animation* clip = m_graphClips[size_t( point.clip )];
				if ( w < kMinWeight || clip == nullptr )
				{
					continue;
				}
				float ratio = s.blend ? time : OnceRatio( time, graph.clips[size_t( point.clip )].length );
				ozz::animation::SamplingJob sampling;
				sampling.animation = clip;
				sampling.context = m_graphContexts[buffer].get();
				sampling.ratio = std::clamp( point.backward ? 1.0f - ratio : ratio, 0.0f, 1.0f );
				sampling.output = ozz::make_span( m_graphLocals[buffer] );
				if ( sampling.Run() )
				{
					ozz::animation::BlendingJob::Layer out;
					out.weight = w;
					out.transform = ozz::make_span( m_graphLocals[buffer] );
					layers.push_back( out );
					++buffer;
				}
			}
		};
		sample( layer.states[L.state], L.time, L.blend, L.blendY, in );
		if ( in < 1.0f )
		{
			sample( layer.states[L.previous], L.previousTime, L.previousBlend, L.previousBlendY, 1.0f - in );
		}

		ozz::animation::BlendingJob blending;
		blending.threshold = 0.1f;
		blending.layers = ozz::span<const ozz::animation::BlendingJob::Layer>( layers.data(), layers.size() );
		blending.rest_pose = skeleton.joint_rest_poses();
		blending.output = ozz::make_span( l == 0 ? m_blended : m_layerPose );
		blending.Run();
		if ( l > 0 )
		{
			const auto& mask = m_graphMasks[l];
			BlendOver( m_layerPose, mask.empty() ? nullptr : &mask, L.weight );
			neckKept *= 1.0f - std::min( L.weight, 1.0f ) * m_graphNeckMask[l];
		}
	}
	m_neckCover = 1.0f - neckKept;
}

void PoseEvaluator::BlendOver( const ozz::vector<ozz::math::SoaTransform>& pose, const ozz::vector<ozz::math::SimdFloat4>* mask,
							   float weight )
{
	const auto& skeleton = m_set.Skeleton();
	const ozz::math::SimdFloat4 one = ozz::math::simd_float4::one();
	const ozz::math::SimdFloat4 w = ozz::math::simd_float4::Load1( std::min( weight, 1.0f ) );
	for ( size_t i = 0; i < m_stanceWeights.size(); ++i )
	{
		m_stanceWeights[i] = mask != nullptr ? w * ( *mask )[i] : w;
		m_keepWeights[i] = one - m_stanceWeights[i];
	}
	std::array<ozz::animation::BlendingJob::Layer, 2> layers;
	layers[0].weight = 1.0f;
	layers[0].transform = ozz::make_span( m_blended );
	layers[0].joint_weights = ozz::make_span( m_keepWeights );
	layers[1].weight = 1.0f;
	layers[1].transform = ozz::make_span( pose );
	layers[1].joint_weights = ozz::make_span( m_stanceWeights );
	ozz::animation::BlendingJob blending;
	blending.threshold = 0.001f;
	blending.layers = ozz::make_span( layers );
	blending.rest_pose = skeleton.joint_rest_poses();
	blending.output = ozz::make_span( m_scratch );
	if ( blending.Run() )
	{
		std::swap( m_blended, m_scratch );
	}
}

void PoseEvaluator::ApplyStance( int stance, int layer, float weight, float layerTime )
{
	if ( !m_stances || stance < 0 || stance >= int( m_stances->stances.size() ) || layer >= int( m_stances->masks.size() ) ||
		 weight <= 0.0f )
	{
		return;
	}
	const auto& mask = m_stances->masks[size_t( layer )];
	const StanceTable::Stance& st = m_stances->stances[size_t( stance )];
	bool perClip = false;
	for ( const auto* clip : st.clips )
	{
		perClip |= clip != nullptr;
	}
	if ( mask.empty() || ( perClip == false && st.single == nullptr ) )
	{
		return;
	}
	const auto& skeleton = m_set.Skeleton();

	// The stance's pose: its own clips where it has them (same blend as the base), else one loop.
	if ( perClip )
	{
		std::array<ozz::animation::BlendingJob::Layer, ClipCount> layers;
		int count = 0;
		for ( int c = 0; c < ClipCount; ++c )
		{
			float w = m_lastWeights.weight[c];
			if ( w < kMinWeight )
			{
				continue;
			}
			const ozz::animation::Animation* own = st.clips[size_t( c )];
			if ( own != nullptr )
			{
				ozz::animation::SamplingJob sampling;
				sampling.animation = own;
				sampling.context = &m_stanceContext;
				sampling.ratio = m_lastWeights.ratio[c];
				sampling.output = ozz::make_span( m_stanceClipLocals[c] );
				if ( sampling.Run() == false )
				{
					continue;
				}
				layers[count].transform = ozz::make_span( m_stanceClipLocals[c] );
			}
			else if ( m_set.Get( Clip( c ) ) != nullptr )
			{
				layers[count].transform = ozz::make_span( m_locals[c] ); // the default clip, already sampled
			}
			else
			{
				continue;
			}
			layers[count].weight = w;
			++count;
		}
		ozz::animation::BlendingJob blending;
		blending.threshold = 0.1f;
		blending.layers = ozz::span<const ozz::animation::BlendingJob::Layer>( layers.data(), size_t( count ) );
		blending.rest_pose = skeleton.joint_rest_poses();
		blending.output = ozz::make_span( m_stanceLocals );
		blending.Run();
	}
	else
	{
		ozz::animation::SamplingJob sampling;
		sampling.animation = st.single;
		sampling.context = &m_stanceContext;
		sampling.ratio = LoopRatio( layerTime, st.single->duration() );
		sampling.output = ozz::make_span( m_stanceLocals );
		if ( sampling.Run() == false )
		{
			return;
		}
	}

	// Over what is there so far, by the mask: per joint, (1 - w * mask) of it and w * mask of the stance.
	BlendOver( m_stanceLocals, &mask, weight );
}

PoseEvaluator::~PoseEvaluator() = default;

void PoseEvaluator::Evaluate( const AnimState& state )
{
	if ( m_graph )
	{
		EvaluateGraph( state );
	}
	else
	{
		EvaluateBuiltIn( state );
	}
	Finish( state );
}

void PoseEvaluator::EvaluateBuiltIn( const AnimState& state )
{
	m_neckCover = 0.0f;
	const auto& skeleton = m_set.Skeleton();
	m_lastWeights = ComputeClipWeights( state, m_set );

	std::array<ozz::animation::BlendingJob::Layer, ClipCount> layers;
	int layerCount = 0;
	for ( int c = 0; c < ClipCount; ++c )
	{
		const ozz::animation::Animation* clip = m_set.Get( Clip( c ) );
		float weight = m_lastWeights.weight[c];
		if ( clip == nullptr || weight < kMinWeight )
		{
			continue;
		}

		ozz::animation::SamplingJob sampling;
		sampling.animation = clip;
		sampling.context = &m_contexts[c];
		sampling.ratio = m_lastWeights.ratio[c];
		sampling.output = ozz::make_span( m_locals[c] );
		if ( sampling.Run() == false )
		{
			continue;
		}

		layers[layerCount].weight = weight;
		layers[layerCount].transform = ozz::make_span( m_locals[c] );
		++layerCount;
	}

	ozz::animation::BlendingJob blending;
	blending.threshold = 0.1f;
	blending.layers = ozz::span<const ozz::animation::BlendingJob::Layer>( layers.data(), size_t( layerCount ) );
	blending.rest_pose = skeleton.joint_rest_poses();
	blending.output = ozz::make_span( m_blended );
	blending.Run();

	// Stances, layer by layer in the schema's order: the one being replaced fades out while the
	// new one fades in.
	if ( m_stances )
	{
		for ( int l = 0; l < kMaxAnimLayers && l < int( m_stances->masks.size() ); ++l )
		{
			int current = int( state.stances[l] ) - 1;
			int previous = int( state.previousStances[l] ) - 1;
			float in = std::clamp( state.layerTime[l] / kStanceFadeSeconds, 0.0f, 1.0f );
			if ( previous >= 0 && previous != current && in < 1.0f )
			{
				ApplyStance( previous, l, 1.0f - in, state.layerTime[l] );
			}
			if ( current >= 0 )
			{
				ApplyStance( current, l, in, state.layerTime[l] );
			}
		}
	}
}

void PoseEvaluator::Finish( const AnimState& state )
{
	const auto& skeleton = m_set.Skeleton();
	if ( m_set.LockRootXZ() && skeleton.num_joints() > 0 )
	{
		// Clips exported with root motion would walk away from the capsule; pin the root's
		// horizontal position to its rest value (joint 0 is lane 0 of the first SoA element).
		ozz::math::SoaFloat3& t = m_blended[0].translation;
		const ozz::math::SoaFloat3& rest = skeleton.joint_rest_poses()[0].translation;
		t.x = ozz::math::SetX( t.x, rest.x );
		t.z = ozz::math::SetX( t.z, rest.z );
	}

	float scale = m_set.Scale();
	ozz::math::Float4x4 root = ozz::math::Float4x4::Scaling( ozz::math::simd_float4::Load( scale, scale, scale, 1.0f ) );

	ozz::animation::LocalToModelJob ltm;
	ltm.skeleton = &skeleton;
	ltm.root = &root;
	ltm.input = ozz::make_span( m_blended );
	ltm.output = ozz::make_span( m_models );
	ltm.Run();

	if ( state.legYaw != 0.0f && m_set.TurnLegs() && m_set.HipsJoint() >= 0 && m_set.SpineJoint() >= 0 )
	{
		// Hips toward the direction of travel, the spine back: the legs walk where the character
		// goes while the upper body keeps facing where it faces.
		b3Vec3 up = { 0.0f, 1.0f, 0.0f };
		RotateSubtree( m_set, m_models, m_set.HipsJoint(), b3MakeQuatFromAxisAngle( up, state.legYaw ) );
		RotateSubtree( m_set, m_models, m_set.SpineJoint(), b3MakeQuatFromAxisAngle( up, -state.legYaw ) );
	}

	if ( m_set.FaceForward() && m_set.HipsJoint() >= 0 && m_set.SpineJoint() >= 0 )
	{
		// How far the clips turned the hips from their rest, about the vertical.
		const ozz::math::Float4x4& hips = m_models[size_t( m_set.HipsJoint() )];
		const ozz::math::Float4x4& rest = m_set.RestModels()[size_t( m_set.HipsJoint() )];
		float r[3][4], h[3][4];
		for ( int i = 0; i < 3; ++i )
		{
			ozz::math::StorePtrU( rest.cols[i], r[i] );
			ozz::math::StorePtrU( hips.cols[i], h[i] );
		}
		auto length = []( const float* v ) { return std::sqrt( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] ); };
		// The rest's forward (+Z) in the hips' own frame, then where the posed hips point it.
		float local[3];
		for ( int i = 0; i < 3; ++i )
		{
			float n = length( r[i] );
			local[i] = n > 0.0f ? r[i][2] / n : 0.0f;
		}
		float f[3] = { 0.0f, 0.0f, 0.0f };
		for ( int i = 0; i < 3; ++i )
		{
			float n = length( h[i] );
			for ( int k = 0; k < 3; ++k )
			{
				f[k] += n > 0.0f ? h[i][k] / n * local[i] : 0.0f;
			}
		}
		float turn = detmath::Atan2( f[0], f[2] );
		b3Vec3 up = { 0.0f, 1.0f, 0.0f };
		RotateSubtree( m_set, m_models, m_set.SpineJoint(), b3MakeQuatFromAxisAngle( up, -turn ) );
		// The head: the clips that turned the hips already looked forward with it; an upper layer's
		// clip did not.
		float keep = 1.0f - m_neckCover;
		if ( m_set.NeckJoint() >= 0 && keep > 0.0f )
		{
			RotateSubtree( m_set, m_models, m_set.NeckJoint(), b3MakeQuatFromAxisAngle( up, turn * keep ) );
		}
	}

	if ( state.aiming != 0 && m_set.AimJoints().empty() == false )
	{
		// Where the player looks, in the body's frame (facing +Z). detmath's sine and cosine are
		// the same on every platform, like the rest of the pose.
		b3CosSin pitch = detmath::CosSin( state.aimPitch );
		b3CosSin yaw = detmath::CosSin( state.aimYaw );
		b3Vec3 direction = { yaw.sine * pitch.cosine, pitch.sine, yaw.cosine * pitch.cosine };
		AimChain( m_set, m_models, m_set.AimJoints(), m_set.AimTip(), direction );
	}
}

} // namespace cb::anim
