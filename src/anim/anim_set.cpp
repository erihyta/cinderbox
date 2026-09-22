#include "anim_set.h"

#include "anim_controller.h"
#include "detmath.h"

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/base/io/archive.h"
#include "ozz/base/io/stream.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/span.h"

namespace cb::anim
{

using ozz::animation::offline::RawAnimation;
using ozz::animation::offline::RawSkeleton;
using ozz::math::Float3;
using ozz::math::Quaternion;

const char* ClipName( Clip clip )
{
	switch ( clip )
	{
		case ClipIdle:
			return "idle";
		case ClipWalk:
			return "walk";
		case ClipRun:
			return "run";
		case ClipJumpStart:
			return "jump_start";
		case ClipFall:
			return "fall";
		case ClipLand:
			return "land";
		default:
			return "?";
	}
}

namespace
{

// ---------------------------------------------------------------------------------------------
// Procedural rig. Godot's SkeletonProfileHumanoid joint names, Y up, facing +Z, character's left
// on +X, feet at y = 0. Those names are what Godot retargets any imported character onto, so a rig
// that uses them can be driven without a per-character mapping. Two tip joints (HeadTop, ToesEnd)
// are not in the profile; they only give the box renderer something to measure.
// Everything uses detmath trig so the generated data is identical on every platform.

enum Joint : int
{
	Hips,
	Spine,
	Chest,
	UpperChest,
	Neck,
	Head,
	HeadTopEnd,
	LeftShoulder,
	LeftUpperArm,
	LeftLowerArm,
	LeftHand,
	LeftHandEnd,
	RightShoulder,
	RightUpperArm,
	RightLowerArm,
	RightHand,
	RightHandEnd,
	LeftUpperLeg,
	LeftLowerLeg,
	LeftFoot,
	LeftToes,
	LeftToesEnd,
	RightUpperLeg,
	RightLowerLeg,
	RightFoot,
	RightToes,
	RightToesEnd,
	JointCount,
};

struct JointDef
{
	const char* name;
	int parent;
	Float3 offset;
};

const JointDef kJoints[JointCount] = {
	{ "Hips", -1, { 0.0f, 0.95f, 0.0f } },
	{ "Spine", Hips, { 0.0f, 0.10f, 0.0f } },
	{ "Chest", Spine, { 0.0f, 0.12f, 0.0f } },
	{ "UpperChest", Chest, { 0.0f, 0.12f, 0.0f } },
	{ "Neck", UpperChest, { 0.0f, 0.14f, 0.0f } },
	{ "Head", Neck, { 0.0f, 0.08f, 0.0f } },
	{ "HeadTop", Head, { 0.0f, 0.24f, 0.0f } },
	{ "LeftShoulder", UpperChest, { 0.06f, 0.10f, 0.0f } },
	{ "LeftUpperArm", LeftShoulder, { 0.12f, 0.0f, 0.0f } },
	{ "LeftLowerArm", LeftUpperArm, { 0.0f, -0.27f, 0.0f } },
	{ "LeftHand", LeftLowerArm, { 0.0f, -0.25f, 0.0f } },
	{ "LeftMiddleProximal", LeftHand, { 0.0f, -0.09f, 0.0f } },
	{ "RightShoulder", UpperChest, { -0.06f, 0.10f, 0.0f } },
	{ "RightUpperArm", RightShoulder, { -0.12f, 0.0f, 0.0f } },
	{ "RightLowerArm", RightUpperArm, { 0.0f, -0.27f, 0.0f } },
	{ "RightHand", RightLowerArm, { 0.0f, -0.25f, 0.0f } },
	{ "RightMiddleProximal", RightHand, { 0.0f, -0.09f, 0.0f } },
	{ "LeftUpperLeg", Hips, { 0.10f, -0.05f, 0.0f } },
	{ "LeftLowerLeg", LeftUpperLeg, { 0.0f, -0.42f, 0.0f } },
	{ "LeftFoot", LeftLowerLeg, { 0.0f, -0.42f, 0.0f } },
	{ "LeftToes", LeftFoot, { 0.0f, -0.06f, 0.12f } },
	{ "LeftToesEnd", LeftToes, { 0.0f, 0.0f, 0.07f } },
	{ "RightUpperLeg", Hips, { -0.10f, -0.05f, 0.0f } },
	{ "RightLowerLeg", RightUpperLeg, { 0.0f, -0.42f, 0.0f } },
	{ "RightFoot", RightLowerLeg, { 0.0f, -0.42f, 0.0f } },
	{ "RightToes", RightFoot, { 0.0f, -0.06f, 0.12f } },
	{ "RightToesEnd", RightToes, { 0.0f, 0.0f, 0.07f } },
};

constexpr float kKeyRate = 30.0f;

Quaternion AxisAngle( float x, float y, float z, float angle )
{
	b3CosSin cs = detmath::CosSin( 0.5f * angle );
	return Quaternion( x * cs.sine, y * cs.sine, z * cs.sine, cs.cosine );
}

Quaternion RotX( float a )
{
	return AxisAngle( 1.0f, 0.0f, 0.0f, a );
}
Quaternion RotY( float a )
{
	return AxisAngle( 0.0f, 1.0f, 0.0f, a );
}
Quaternion RotZ( float a )
{
	return AxisAngle( 0.0f, 0.0f, 1.0f, a );
}

float Sin01( float phase )
{
	return detmath::CosSin( detmath::kTwoPi * phase ).sine;
}
float Cos01( float phase )
{
	return detmath::CosSin( detmath::kTwoPi * phase ).cosine;
}
float Lerp( float a, float b, float t )
{
	return a + ( b - a ) * t;
}
float Smooth( float t )
{
	return t * t * ( 3.0f - 2.0f * t );
}

// Pose at one instant: per-joint local rotation (on top of identity rest) and hips height.
struct Pose
{
	Quaternion rotation[JointCount];
	float hipsHeight = 0.95f;

	Pose()
	{
		for ( Quaternion& q : rotation )
		{
			q = Quaternion::identity();
		}
	}
};

// Limb conventions (rotation about local X): legs and arms swing forward with a negative angle,
// knees bend with a positive angle, the spine leans forward with a positive angle.
using PoseFn = Pose ( * )( float time, float duration );

Pose IdlePose( float time, float duration )
{
	Pose p;
	float breath = Sin01( time / duration );
	p.rotation[Chest] = RotX( 0.02f * breath );
	p.rotation[Neck] = RotX( -0.02f * breath );
	p.rotation[LeftUpperArm] = RotZ( 0.12f + 0.02f * breath );
	p.rotation[RightUpperArm] = RotZ( -0.12f - 0.02f * breath );
	p.rotation[LeftLowerArm] = RotX( -0.15f );
	p.rotation[RightLowerArm] = RotX( -0.15f );
	p.hipsHeight = 0.94f + 0.005f * breath;
	return p;
}

Pose CyclePose( float phase, float legSwing, float kneeBend, float armSwing, float elbow, float lean, float bob, float height )
{
	Pose p;
	float s = Sin01( phase );
	float kneeL = kneeBend * std::max( 0.0f, Sin01( phase + 0.25f ) ) + 0.1f;
	float kneeR = kneeBend * std::max( 0.0f, Sin01( phase + 0.75f ) ) + 0.1f;

	p.rotation[Hips] = RotY( 0.08f * s );
	p.rotation[Spine] = RotX( lean );
	p.rotation[Chest] = RotY( -0.1f * s );
	p.rotation[LeftUpperLeg] = RotX( -legSwing * s );
	p.rotation[RightUpperLeg] = RotX( legSwing * s );
	p.rotation[LeftLowerLeg] = RotX( kneeL );
	p.rotation[RightLowerLeg] = RotX( kneeR );
	p.rotation[LeftFoot] = RotX( -0.3f * kneeL );
	p.rotation[RightFoot] = RotX( -0.3f * kneeR );
	p.rotation[LeftUpperArm] = RotZ( 0.1f ) * RotX( armSwing * s );
	p.rotation[RightUpperArm] = RotZ( -0.1f ) * RotX( -armSwing * s );
	p.rotation[LeftLowerArm] = RotX( -elbow );
	p.rotation[RightLowerArm] = RotX( -elbow );
	// Two bobs per cycle (one per step).
	float c2 = Cos01( 2.0f * phase );
	p.hipsHeight = height - bob * 0.5f * ( 1.0f + c2 );
	return p;
}

Pose WalkPose( float time, float duration )
{
	return CyclePose( time / duration, 0.45f, 0.55f, 0.35f, 0.25f, 0.05f, 0.03f, 0.95f );
}

Pose RunPose( float time, float duration )
{
	return CyclePose( time / duration, 0.8f, 1.1f, 0.8f, 1.3f, 0.25f, 0.06f, 0.92f );
}

Pose JumpStartPose( float time, float duration )
{
	Pose p;
	float t = Smooth( std::min( time / duration, 1.0f ) );
	float knee = Lerp( 1.0f, 0.15f, t );
	p.rotation[LeftUpperLeg] = RotX( -0.5f * knee );
	p.rotation[RightUpperLeg] = RotX( -0.5f * knee );
	p.rotation[LeftLowerLeg] = RotX( knee );
	p.rotation[RightLowerLeg] = RotX( knee );
	p.rotation[Spine] = RotX( Lerp( 0.35f, 0.0f, t ) );
	p.rotation[LeftUpperArm] = RotZ( 0.2f ) * RotX( Lerp( 0.5f, -2.6f, t ) );
	p.rotation[RightUpperArm] = RotZ( -0.2f ) * RotX( Lerp( 0.5f, -2.6f, t ) );
	p.rotation[LeftLowerArm] = RotX( -0.2f );
	p.rotation[RightLowerArm] = RotX( -0.2f );
	p.hipsHeight = Lerp( 0.78f, 0.97f, t );
	return p;
}

Pose FallPose( float time, float duration )
{
	Pose p;
	float s = Sin01( time / duration );
	p.rotation[LeftUpperLeg] = RotX( -0.5f - 0.15f * s );
	p.rotation[RightUpperLeg] = RotX( -0.15f + 0.15f * s );
	p.rotation[LeftLowerLeg] = RotX( 0.7f );
	p.rotation[RightLowerLeg] = RotX( 0.35f );
	p.rotation[LeftUpperArm] = RotZ( 1.3f + 0.2f * s );
	p.rotation[RightUpperArm] = RotZ( -1.3f - 0.2f * s );
	p.rotation[LeftLowerArm] = RotZ( 0.3f );
	p.rotation[RightLowerArm] = RotZ( -0.3f );
	p.rotation[Spine] = RotX( -0.1f );
	p.hipsHeight = 0.93f;
	return p;
}

Pose LandPose( float time, float duration )
{
	Pose p;
	float t = Smooth( std::min( time / duration, 1.0f ) );
	float crouch = 1.0f - t;
	p.rotation[LeftUpperLeg] = RotX( -0.9f * crouch );
	p.rotation[RightUpperLeg] = RotX( -0.9f * crouch );
	p.rotation[LeftLowerLeg] = RotX( 1.3f * crouch + 0.1f );
	p.rotation[RightLowerLeg] = RotX( 1.3f * crouch + 0.1f );
	p.rotation[LeftFoot] = RotX( -0.4f * crouch );
	p.rotation[RightFoot] = RotX( -0.4f * crouch );
	p.rotation[Spine] = RotX( 0.4f * crouch );
	p.rotation[LeftUpperArm] = RotZ( 0.3f ) * RotX( -0.7f * crouch );
	p.rotation[RightUpperArm] = RotZ( -0.3f ) * RotX( -0.7f * crouch );
	p.hipsHeight = Lerp( 0.7f, 0.94f, t );
	return p;
}

ozz::unique_ptr<ozz::animation::Animation> BuildClip( const char* name, float duration, PoseFn fn )
{
	RawAnimation raw;
	raw.name = name;
	raw.duration = duration;
	raw.tracks.resize( JointCount );

	int keys = std::max( 2, int( duration * kKeyRate + 0.5f ) + 1 );
	for ( int k = 0; k < keys; ++k )
	{
		// Last key lands exactly on the duration so loops close.
		float time = k == keys - 1 ? duration : duration * float( k ) / float( keys - 1 );
		Pose pose = fn( time, duration );
		for ( int j = 0; j < JointCount; ++j )
		{
			RawAnimation::JointTrack& track = raw.tracks[j];
			track.rotations.push_back( { time, pose.rotation[j] } );
			if ( j == Hips )
			{
				track.translations.push_back( { time, Float3( 0.0f, pose.hipsHeight, 0.0f ) } );
			}
			else if ( k == 0 )
			{
				track.translations.push_back( { 0.0f, kJoints[j].offset } );
			}
		}
	}
	for ( RawAnimation::JointTrack& track : raw.tracks )
	{
		track.scales.push_back( { 0.0f, Float3::one() } );
	}

	ozz::animation::offline::AnimationBuilder builder;
	return builder( raw );
}

ozz::unique_ptr<ozz::animation::Skeleton> BuildSkeleton()
{
	RawSkeleton raw;
	// Build the hierarchy by attaching each joint to its parent. Parents always come first.
	std::vector<RawSkeleton::Joint*> nodes( JointCount, nullptr );
	// Reserve children vectors up front: pointers into vectors must stay valid while building.
	std::vector<int> childCount( JointCount, 0 );
	int rootCount = 0;
	for ( int j = 0; j < JointCount; ++j )
	{
		if ( kJoints[j].parent < 0 )
			++rootCount;
		else
			++childCount[kJoints[j].parent];
	}
	raw.roots.reserve( rootCount );

	for ( int j = 0; j < JointCount; ++j )
	{
		RawSkeleton::Joint joint;
		joint.name = kJoints[j].name;
		joint.transform = ozz::math::Transform::identity();
		joint.transform.translation = kJoints[j].offset;
		joint.children.reserve( childCount[j] );

		auto& siblings = kJoints[j].parent < 0 ? raw.roots : nodes[kJoints[j].parent]->children;
		siblings.push_back( std::move( joint ) );
		nodes[j] = &siblings.back();
	}

	ozz::animation::offline::SkeletonBuilder builder;
	return builder( raw );
}

// Tiny "key = value" config reader.
std::map<std::string, std::string> ReadConfig( const std::string& path, bool& ok )
{
	std::map<std::string, std::string> values;
	std::ifstream in( path );
	ok = in.good();
	std::string line;
	while ( std::getline( in, line ) )
	{
		size_t hash = line.find( '#' );
		if ( hash != std::string::npos )
		{
			line.resize( hash );
		}
		size_t eq = line.find( '=' );
		if ( eq == std::string::npos )
		{
			continue;
		}
		auto trim = []( std::string s ) {
			size_t a = s.find_first_not_of( " \t\r\n" );
			size_t b = s.find_last_not_of( " \t\r\n" );
			return a == std::string::npos ? std::string() : s.substr( a, b - a + 1 );
		};
		values[trim( line.substr( 0, eq ) )] = trim( line.substr( eq + 1 ) );
	}
	return values;
}

template <typename T>
ozz::unique_ptr<T> LoadArchive( const std::string& path )
{
	ozz::io::File file( path.c_str(), "rb" );
	if ( file.opened() == false )
	{
		return nullptr;
	}
	ozz::io::IArchive archive( &file );
	if ( archive.TestTag<T>() == false )
	{
		return nullptr;
	}
	auto object = ozz::make_unique<T>();
	archive >> *object;
	return object;
}

template <typename T>
bool SaveArchive( const std::string& path, const T& object )
{
	ozz::io::File file( path.c_str(), "wb" );
	if ( file.opened() == false )
	{
		return false;
	}
	ozz::io::OArchive archive( &file );
	archive << object;
	return true;
}

} // namespace

// Model-space rest, scaled like the poses are. Retargeting reads a pose as a deviation from this.
void ComputeRestModels( AnimSet& set, ozz::vector<ozz::math::Float4x4>& out, float scale )
{
	const ozz::animation::Skeleton& skeleton = set.Skeleton();
	out.resize( size_t( skeleton.num_joints() ) );
	ozz::animation::LocalToModelJob ltm;
	ltm.skeleton = &skeleton;
	ltm.input = skeleton.joint_rest_poses();
	ltm.output = ozz::make_span( out );
	if ( ltm.Run() == false )
	{
		return;
	}
	if ( scale != 1.0f )
	{
		for ( ozz::math::Float4x4& m : out )
		{
			m = ozz::math::Float4x4::Scaling( ozz::math::simd_float4::Load1( scale ) ) * m;
		}
	}
}

std::unique_ptr<AnimSet> AnimSet::CreateProcedural()
{
	auto set = std::make_unique<AnimSet>();
	set->m_skeleton = BuildSkeleton();
	set->m_clips[ClipIdle] = BuildClip( "idle", 2.0f, IdlePose );
	set->m_clips[ClipWalk] = BuildClip( "walk", anim_tuning::kWalkCycleSeconds, WalkPose );
	set->m_clips[ClipRun] = BuildClip( "run", anim_tuning::kRunCycleSeconds, RunPose );
	set->m_clips[ClipJumpStart] = BuildClip( "jump_start", anim_tuning::kJumpStartSeconds, JumpStartPose );
	set->m_clips[ClipFall] = BuildClip( "fall", 1.0f, FallPose );
	set->m_clips[ClipLand] = BuildClip( "land", anim_tuning::kLandSeconds, LandPose );
	set->m_description = "procedural placeholder rig";
	ComputeRestModels( *set, set->m_restModels, set->m_scale );
	return set;
}

std::unique_ptr<AnimSet> AnimSet::Load( const std::string& dir, std::string& error, std::string& warnings )
{
	bool ok = false;
	auto cfg = ReadConfig( dir + "/anim.cfg", ok );
	if ( ok == false )
	{
		error = "no anim.cfg in " + dir;
		return nullptr;
	}

	auto set = std::make_unique<AnimSet>();
	std::string skeletonFile = cfg.count( "skeleton" ) ? cfg["skeleton"] : "skeleton.ozz";
	set->m_skeleton = LoadArchive<ozz::animation::Skeleton>( dir + "/" + skeletonFile );
	if ( set->m_skeleton == nullptr )
	{
		error = "cannot load skeleton " + dir + "/" + skeletonFile;
		return nullptr;
	}

	if ( cfg.count( "scale" ) )
	{
		set->m_scale = float( std::atof( cfg["scale"].c_str() ) );
	}
	else
	{
		// Mixamo rigs exported through Blender are usually in centimetres (the armature's 0.01
		// scale is not part of the skeleton). A humanoid taller than 20 units is not in metres.
		const auto& skeleton = *set->m_skeleton;
		ozz::vector<ozz::math::Float4x4> models( skeleton.num_joints() );
		ozz::animation::LocalToModelJob ltm;
		ltm.skeleton = &skeleton;
		ltm.input = skeleton.joint_rest_poses();
		ltm.output = ozz::make_span( models );
		float height = 0.0f;
		if ( ltm.Run() )
		{
			for ( const auto& m : models )
			{
				height = std::max( height, ozz::math::GetY( m.cols[3] ) );
			}
		}
		if ( height > 20.0f )
		{
			set->m_scale = 0.01f;
			warnings += "rig is " + std::to_string( int( height ) ) + " units tall, assuming centimetres (scale 0.01); ";
		}
	}
	if ( cfg.count( "lock_root_xz" ) )
	{
		set->m_lockRootXZ = cfg["lock_root_xz"] != "false" && cfg["lock_root_xz"] != "0";
	}

	int loaded = 0;
	for ( int c = 0; c < ClipCount; ++c )
	{
		const char* name = ClipName( Clip( c ) );
		std::string file = cfg.count( name ) ? cfg[name] : std::string( name ) + ".ozz";
		auto clip = LoadArchive<ozz::animation::Animation>( dir + "/" + file );
		if ( clip == nullptr )
		{
			warnings += std::string( "missing clip '" ) + name + "' (" + file + "); ";
			continue;
		}
		if ( clip->num_tracks() != set->m_skeleton->num_joints() )
		{
			warnings += std::string( "clip '" ) + name + "' was built for a different skeleton; ";
			continue;
		}
		set->m_clips[c] = std::move( clip );
		++loaded;
	}

	ComputeRestModels( *set, set->m_restModels, set->m_scale );
	set->m_description = dir + " (" + std::to_string( set->m_skeleton->num_joints() ) + " joints, " + std::to_string( loaded ) +
						 "/" + std::to_string( int( ClipCount ) ) + " clips)";
	return set;
}

bool AnimSet::Save( const std::string& dir ) const
{
	if ( SaveArchive( dir + "/skeleton.ozz", *m_skeleton ) == false )
	{
		return false;
	}
	std::ofstream cfg( dir + "/anim.cfg" );
	cfg << "# written by AnimSet::Save\n";
	cfg << "skeleton = skeleton.ozz\n";
	cfg << "scale = " << m_scale << "\n";
	cfg << "lock_root_xz = " << ( m_lockRootXZ ? "true" : "false" ) << "\n";
	for ( int c = 0; c < ClipCount; ++c )
	{
		if ( m_clips[c] == nullptr )
		{
			continue;
		}
		std::string file = std::string( ClipName( Clip( c ) ) ) + ".ozz";
		if ( SaveArchive( dir + "/" + file, *m_clips[c] ) == false )
		{
			return false;
		}
		cfg << ClipName( Clip( c ) ) << " = " << file << "\n";
	}
	return cfg.good();
}

} // namespace cb::anim
