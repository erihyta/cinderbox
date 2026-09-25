#include "retarget.h"

#include "profile.h"

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/span.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace cb::anim
{

namespace
{

using ozz::math::Float3;
using ozz::math::Quaternion;
using ozz::math::Transform;

// Joint `j` of a SoA pose, as a plain transform.
Transform Extract( ozz::span<const ozz::math::SoaTransform> soa, int j )
{
	const ozz::math::SoaTransform& s = soa[size_t( j / 4 )];
	int lane = j % 4;
	auto pick = [lane]( ozz::math::SimdFloat4 v ) {
		float values[4];
		ozz::math::StorePtrU( v, values );
		return values[lane];
	};
	Transform t;
	t.translation = Float3( pick( s.translation.x ), pick( s.translation.y ), pick( s.translation.z ) );
	t.rotation = Quaternion( pick( s.rotation.x ), pick( s.rotation.y ), pick( s.rotation.z ), pick( s.rotation.w ) );
	t.scale = Float3( pick( s.scale.x ), pick( s.scale.y ), pick( s.scale.z ) );
	return t;
}

Quaternion Multiply( const Quaternion& a, const Quaternion& b )
{
	return Quaternion( a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
					   a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z );
}

Quaternion Inverse( const Quaternion& q )
{
	return Quaternion( -q.x, -q.y, -q.z, q.w ); // rests are unit quaternions
}

Quaternion Normalized( const Quaternion& q )
{
	float n = std::sqrt( q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w );
	return n > 0.0f ? Quaternion( q.x / n, q.y / n, q.z / n, q.w / n ) : Quaternion::identity();
}

// The rest height of the joint named `profile` (model space), 0 when absent.
float RestHeight( const ozz::animation::Skeleton& skeleton, const char* profile )
{
	ozz::vector<ozz::math::Float4x4> models( size_t( skeleton.num_joints() ) );
	ozz::animation::LocalToModelJob ltm;
	ltm.skeleton = &skeleton;
	ltm.input = skeleton.joint_rest_poses();
	ltm.output = ozz::make_span( models );
	if ( ltm.Run() == false )
	{
		return 0.0f;
	}
	auto names = skeleton.joint_names();
	for ( int j = 0; j < skeleton.num_joints(); ++j )
	{
		const char* name = ProfileName( names[size_t( j )] );
		if ( name != nullptr && std::strcmp( name, profile ) == 0 )
		{
			float v[4];
			ozz::math::StorePtrU( models[size_t( j )].cols[3], v );
			return v[1];
		}
	}
	return 0.0f;
}

} // namespace

std::shared_ptr<const PackClips> FitPack( std::shared_ptr<const AnimSet> packSet, const AnimGraph& graph, const AnimSet& character,
										  std::string& warnings )
{
	auto fitted = std::make_shared<PackClips>();
	fitted->pack = packSet;
	const AnimSet& pack = *packSet;
	bool same = SameSkeleton( pack.Skeleton(), character.Skeleton() );
	for ( const AnimGraphClip& clip : graph.clips )
	{
		const ozz::animation::Animation* source = pack.NamedClip( clip.name );
		if ( source == nullptr )
		{
			warnings += "the pack plays '" + clip.name + "', which it has no clip for; ";
			fitted->clips.push_back( nullptr );
			continue;
		}
		if ( same )
		{
			fitted->clips.push_back( source );
			continue;
		}
		std::string error;
		auto rebuilt = RetargetClip( *source, pack.Skeleton(), character.Skeleton(), 30.0f, error );
		if ( rebuilt == nullptr )
		{
			warnings += "clip '" + clip.name + "': " + error + "; ";
		}
		fitted->clips.push_back( rebuilt.get() );
		if ( rebuilt )
		{
			fitted->owned.push_back( std::move( rebuilt ) );
		}
	}
	return fitted;
}

bool SameSkeleton( const ozz::animation::Skeleton& a, const ozz::animation::Skeleton& b )
{
	if ( a.num_joints() != b.num_joints() )
	{
		return false;
	}
	for ( int j = 0; j < a.num_joints(); ++j )
	{
		if ( std::strcmp( a.joint_names()[size_t( j )], b.joint_names()[size_t( j )] ) != 0 ||
			 a.joint_parents()[size_t( j )] != b.joint_parents()[size_t( j )] )
		{
			return false;
		}
	}
	for ( int s = 0; s < a.num_soa_joints(); ++s )
	{
		if ( std::memcmp( &a.joint_rest_poses()[size_t( s )], &b.joint_rest_poses()[size_t( s )], sizeof( ozz::math::SoaTransform ) ) != 0 )
		{
			return false;
		}
	}
	return true;
}

ozz::unique_ptr<ozz::animation::Animation> RetargetClip( const ozz::animation::Animation& clip, const ozz::animation::Skeleton& from,
														const ozz::animation::Skeleton& to, float sampleRate, std::string& error )
{
	// Which source joint drives each target joint.
	const int targetJoints = to.num_joints();
	std::vector<int> source( size_t( targetJoints ), -1 );
	int matched = 0;
	int hipsTarget = -1;
	for ( int t = 0; t < targetJoints; ++t )
	{
		const char* name = ProfileName( to.joint_names()[size_t( t )] );
		if ( name == nullptr )
		{
			continue;
		}
		for ( int s = 0; s < from.num_joints(); ++s )
		{
			const char* other = ProfileName( from.joint_names()[size_t( s )] );
			if ( other != nullptr && std::strcmp( name, other ) == 0 )
			{
				source[size_t( t )] = s;
				++matched;
				break;
			}
		}
		if ( std::strcmp( name, "Hips" ) == 0 )
		{
			hipsTarget = t;
		}
	}
	if ( matched == 0 )
	{
		error = "the skeletons share no humanoid-profile joints";
		return nullptr;
	}
	float fromHips = RestHeight( from, "Hips" );
	float toHips = RestHeight( to, "Hips" );
	float hipsScale = fromHips > 0.0f && toHips > 0.0f ? toHips / fromHips : 1.0f;

	// Sample the source at a fixed rate and rebuild each target joint's track.
	ozz::animation::offline::RawAnimation raw;
	raw.duration = std::max( clip.duration(), 1.0f / sampleRate );
	raw.tracks.resize( size_t( targetJoints ) );
	ozz::animation::SamplingJob::Context context( from.num_joints() );
	ozz::vector<ozz::math::SoaTransform> locals( size_t( from.num_soa_joints() ) );
	auto fromRest = from.joint_rest_poses();
	auto toRest = to.joint_rest_poses();
	int samples = std::max( 2, int( std::ceil( raw.duration * sampleRate ) ) + 1 );
	std::vector<Quaternion> previous( size_t( targetJoints ), Quaternion::identity() );
	for ( int i = 0; i < samples; ++i )
	{
		float time = i == samples - 1 ? raw.duration : std::min( raw.duration, float( i ) / sampleRate );
		ozz::animation::SamplingJob sampling;
		sampling.animation = &clip;
		sampling.context = &context;
		sampling.ratio = clip.duration() > 0.0f ? std::clamp( time / clip.duration(), 0.0f, 1.0f ) : 0.0f;
		sampling.output = ozz::make_span( locals );
		if ( sampling.Run() == false )
		{
			error = "the clip could not be sampled";
			return nullptr;
		}
		for ( int t = 0; t < targetJoints; ++t )
		{
			Transform rest = Extract( toRest, t );
			Transform out = rest;
			int s = source[size_t( t )];
			if ( s >= 0 )
			{
				Transform posed = Extract( ozz::make_span( locals ), s );
				Transform sourceRest = Extract( fromRest, s );
				// The turn from rest, in the bone's own frame, onto this skeleton's rest.
				out.rotation = Normalized( Multiply( rest.rotation, Multiply( Inverse( sourceRest.rotation ), posed.rotation ) ) );
				if ( t == hipsTarget )
				{
					out.translation = Float3( rest.translation.x + ( posed.translation.x - sourceRest.translation.x ) * hipsScale,
											  rest.translation.y + ( posed.translation.y - sourceRest.translation.y ) * hipsScale,
											  rest.translation.z + ( posed.translation.z - sourceRest.translation.z ) * hipsScale );
				}
			}
			// Neighbouring keys in the same hemisphere, so interpolation takes the short way.
			Quaternion& last = previous[size_t( t )];
			if ( i > 0 && last.x * out.rotation.x + last.y * out.rotation.y + last.z * out.rotation.z + last.w * out.rotation.w < 0.0f )
			{
				out.rotation = Quaternion( -out.rotation.x, -out.rotation.y, -out.rotation.z, -out.rotation.w );
			}
			last = out.rotation;
			auto& track = raw.tracks[size_t( t )];
			track.translations.push_back( { time, out.translation } );
			track.rotations.push_back( { time, out.rotation } );
			track.scales.push_back( { time, out.scale } );
		}
	}
	if ( raw.Validate() == false )
	{
		error = "the retargeted clip is not valid";
		return nullptr;
	}
	ozz::unique_ptr<ozz::animation::Animation> built = ozz::animation::offline::AnimationBuilder()( raw );
	if ( built == nullptr )
	{
		error = "ozz could not build the retargeted clip";
	}
	return built;
}

} // namespace cb::anim
