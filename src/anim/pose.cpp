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

AnimState InterpolateAnimState( const AnimState& from, const AnimState& to, float alpha )
{
	AnimState s = to;
	float t = std::clamp( alpha, 0.0f, 1.0f );
	s.groundSpeed = from.groundSpeed + ( to.groundSpeed - from.groundSpeed ) * t;

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
	const ozz::math::SimdFloat4 one = ozz::math::simd_float4::one();
	const ozz::math::SimdFloat4 w = ozz::math::simd_float4::Load1( std::min( weight, 1.0f ) );
	for ( size_t i = 0; i < mask.size(); ++i )
	{
		m_stanceWeights[i] = w * mask[i];
		m_keepWeights[i] = one - m_stanceWeights[i];
	}
	std::array<ozz::animation::BlendingJob::Layer, 2> layers;
	layers[0].weight = 1.0f;
	layers[0].transform = ozz::make_span( m_blended );
	layers[0].joint_weights = ozz::make_span( m_keepWeights );
	layers[1].weight = 1.0f;
	layers[1].transform = ozz::make_span( m_stanceLocals );
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

PoseEvaluator::~PoseEvaluator() = default;

void PoseEvaluator::Evaluate( const AnimState& state )
{
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

	if ( state.legYaw != 0.0f && m_set.HipsJoint() >= 0 && m_set.SpineJoint() >= 0 )
	{
		// Hips toward the direction of travel, the spine back: the legs walk where the character
		// goes while the upper body keeps facing where it faces.
		b3Vec3 up = { 0.0f, 1.0f, 0.0f };
		RotateSubtree( m_set, m_models, m_set.HipsJoint(), b3MakeQuatFromAxisAngle( up, state.legYaw ) );
		RotateSubtree( m_set, m_models, m_set.SpineJoint(), b3MakeQuatFromAxisAngle( up, -state.legYaw ) );
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
