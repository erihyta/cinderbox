#include "anim_set.h"

#include "anim_controller.h"
#include "joint_math.h"
#include "detmath.h"
#include "profile.h"

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/base/io/archive.h"
#include "ozz/base/io/stream.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

// Stances of the placeholder rig, for the shipped mods: the pistol (upper body) and the melee bat
// (full body). They only set what they change; the layer masks decide which bones they reach.

// Both hands forward, the left one under the right, as if holding a pistol; the aim chain then
// points the right arm exactly.
Pose PistolPose( float time, float duration )
{
	Pose p = IdlePose( time, duration );
	p.rotation[RightUpperArm] = RotX( -1.35f ) * RotZ( 0.15f );
	p.rotation[RightLowerArm] = RotX( -0.1f );
	p.rotation[LeftUpperArm] = RotX( -1.2f ) * RotZ( -0.45f );
	p.rotation[LeftLowerArm] = RotY( 0.9f ) * RotX( -0.3f );
	p.rotation[Chest] = RotY( 0.12f );
	return p;
}

// The bat over the right shoulder, feet apart, knees soft.
Pose MeleeArms( Pose p )
{
	p.rotation[RightUpperArm] = RotX( -0.5f ) * RotZ( -0.5f );
	p.rotation[RightLowerArm] = RotX( -2.0f );
	p.rotation[LeftUpperArm] = RotX( -0.9f ) * RotZ( -0.35f );
	p.rotation[LeftLowerArm] = RotY( 0.8f ) * RotX( -1.2f );
	return p;
}

Pose MeleeIdlePose( float time, float duration )
{
	Pose p = MeleeArms( IdlePose( time, duration ) );
	p.rotation[Spine] = RotY( 0.25f ) * RotX( 0.08f );
	p.rotation[LeftUpperLeg] = RotZ( 0.12f ) * RotX( -0.25f );
	p.rotation[RightUpperLeg] = RotZ( -0.12f ) * RotX( -0.15f );
	p.rotation[LeftLowerLeg] = RotX( 0.4f );
	p.rotation[RightLowerLeg] = RotX( 0.3f );
	p.hipsHeight = 0.9f;
	return p;
}

Pose MeleeWalkPose( float time, float duration )
{
	Pose p = MeleeArms( CyclePose( time / duration, 0.4f, 0.6f, 0.0f, 0.0f, 0.12f, 0.03f, 0.92f ) );
	p.rotation[Spine] = RotY( 0.2f ) * RotX( 0.12f );
	return p;
}

Pose MeleeRunPose( float time, float duration )
{
	Pose p = MeleeArms( CyclePose( time / duration, 0.75f, 1.0f, 0.0f, 0.0f, 0.3f, 0.06f, 0.9f ) );
	p.rotation[Spine] = RotY( 0.15f ) * RotX( 0.3f );
	return p;
}

// A horizontal swing from the right shoulder across to the left.
Pose MeleeSwingPose( float time, float duration )
{
	Pose p = MeleeIdlePose( 0.0f, 1.0f );
	float t = std::min( time / duration, 1.0f );
	float wind = t < 0.3f ? Smooth( t / 0.3f ) : 1.0f;				  // wind up
	float strike = t < 0.3f ? 0.0f : Smooth( ( t - 0.3f ) / 0.45f ); // swing through
	strike = std::min( strike, 1.0f );
	p.rotation[Spine] = RotY( 0.25f + 0.35f * wind - 1.5f * strike ) * RotX( 0.1f );
	p.rotation[RightUpperArm] = RotY( 0.3f * wind - 0.9f * strike ) * RotX( -1.3f ) * RotZ( -0.4f + 0.2f * strike );
	p.rotation[RightLowerArm] = RotX( Lerp( -1.2f, -0.2f, strike ) );
	p.rotation[LeftUpperArm] = RotY( -0.6f * strike ) * RotX( -1.2f ) * RotZ( -0.5f );
	p.rotation[LeftLowerArm] = RotY( 0.8f ) * RotX( -0.6f );
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
std::map<std::string, std::string> ReadConfig( const std::string& text )
{
	std::map<std::string, std::string> values;
	std::istringstream in( text );
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
ozz::unique_ptr<T> LoadArchive( const FileReader& read, const std::string& name )
{
	std::string bytes;
	if ( read( name, bytes ) == false )
	{
		return nullptr;
	}
	ozz::io::MemoryStream stream;
	if ( stream.Write( bytes.data(), bytes.size() ) != bytes.size() || stream.Seek( 0, ozz::io::Stream::kSet ) != 0 )
	{
		return nullptr;
	}
	ozz::io::IArchive archive( &stream );
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

namespace
{

struct Mat3
{
	float c[3][3]; // columns
};

Mat3 RestRotation( const ozz::math::Float4x4& m )
{
	Mat3 r;
	for ( int i = 0; i < 3; ++i )
	{
		float col[4];
		ozz::math::StorePtrU( m.cols[i], col );
		float length = std::sqrt( col[0] * col[0] + col[1] * col[1] + col[2] * col[2] );
		for ( int k = 0; k < 3; ++k )
		{
			r.c[i][k] = length > 0.0f ? col[k] / length : ( i == k ? 1.0f : 0.0f );
		}
	}
	return r;
}

bool Direction( const float from[3], const float to[3], float out[3] )
{
	float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
	float length = std::sqrt( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] );
	if ( length < 1e-6f )
	{
		return false;
	}
	for ( int k = 0; k < 3; ++k )
	{
		out[k] = d[k] / length;
	}
	return true;
}

// The rotation taking unit vector a onto unit vector b by the shortest arc (Rodrigues).
Mat3 Arc( const float a[3], const float b[3] )
{
	float v[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
	float cosine = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	Mat3 r;
	if ( cosine < -0.9999f )
	{
		// Opposite: half a turn about any axis perpendicular to a.
		float axis[3] = { 0.0f, -a[2], a[1] };
		if ( std::fabs( a[0] ) > 0.9f )
		{
			axis[0] = -a[1], axis[1] = a[0], axis[2] = 0.0f;
		}
		float length = std::sqrt( axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] );
		for ( int i = 0; i < 3; ++i )
		{
			for ( int k = 0; k < 3; ++k )
			{
				r.c[i][k] = 2.0f * axis[i] * axis[k] / ( length * length ) - ( i == k ? 1.0f : 0.0f );
			}
		}
		return r;
	}
	// R = I + [v] + [v]^2 / (1 + cos); element (row k, column i).
	float h = 1.0f / ( 1.0f + cosine );
	float cross[3][3] = { { 0.0f, v[2], -v[1] }, { -v[2], 0.0f, v[0] }, { v[1], -v[0], 0.0f } }; // [column][row]
	for ( int i = 0; i < 3; ++i )
	{
		for ( int k = 0; k < 3; ++k )
		{
			float square = v[k] * v[i] - ( i == k ? v[0] * v[0] + v[1] * v[1] + v[2] * v[2] : 0.0f );
			r.c[i][k] = ( i == k ? 1.0f : 0.0f ) + cross[i][k] + square * h;
		}
	}
	return r;
}

// a^T * b
Mat3 TransposeTimes( const Mat3& a, const Mat3& b )
{
	Mat3 r;
	for ( int i = 0; i < 3; ++i )
	{
		for ( int k = 0; k < 3; ++k )
		{
			// row k of a^T is column k of a
			r.c[i][k] = a.c[k][0] * b.c[i][0] + a.c[k][1] * b.c[i][1] + a.c[k][2] * b.c[i][2];
		}
	}
	return r;
}

// The placeholder rig's direction from a joint to the joint that continues it (its first child).
bool PlaceholderDirection( const char* profile, float out[3], const char*& child )
{
	for ( int j = 0; j < JointCount; ++j )
	{
		if ( std::strcmp( kJoints[j].name, profile ) != 0 )
		{
			continue;
		}
		for ( int c = j + 1; c < JointCount; ++c )
		{
			if ( kJoints[c].parent == j )
			{
				float zero[3] = { 0.0f, 0.0f, 0.0f };
				float offset[3] = { kJoints[c].offset.x, kJoints[c].offset.y, kJoints[c].offset.z };
				child = kJoints[c].name;
				return Direction( zero, offset, out );
			}
		}
		return false;
	}
	return false;
}

} // namespace

void AnimSet::ComputeAttachFrames()
{
	const ozz::animation::Skeleton& skeleton = *m_skeleton;
	int joints = skeleton.num_joints();
	auto names = skeleton.joint_names();
	m_attachFrames.assign( size_t( joints ), ozz::math::Float4x4::identity() );
	for ( int j = 0; j < joints && size_t( j ) < m_restModels.size(); ++j )
	{
		const char* profile = ProfileName( names[j] );
		float placeholder[3];
		const char* childProfile = nullptr;
		if ( profile == nullptr || PlaceholderDirection( profile, placeholder, childProfile ) == false )
		{
			continue;
		}
		int child = -1;
		for ( int c = 0; c < joints; ++c )
		{
			const char* name = ProfileName( names[c] );
			if ( name != nullptr && std::strcmp( name, childProfile ) == 0 )
			{
				child = c;
				break;
			}
		}
		if ( child < 0 )
		{
			continue;
		}
		float from[4], to[4], here[3];
		ozz::math::StorePtrU( m_restModels[size_t( j )].cols[3], from );
		ozz::math::StorePtrU( m_restModels[size_t( child )].cols[3], to );
		if ( Direction( from, to, here ) == false )
		{
			continue;
		}
		// The placeholder's frame, swung onto this rig's bone, expressed in this joint's rest frame.
		Mat3 frame = TransposeTimes( RestRotation( m_restModels[size_t( j )] ), Arc( placeholder, here ) );
		ozz::math::Float4x4& out = m_attachFrames[size_t( j )];
		for ( int i = 0; i < 3; ++i )
		{
			out.cols[i] = ozz::math::simd_float4::Load( frame.c[i][0], frame.c[i][1], frame.c[i][2], 0.0f );
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
	set->m_stanceClips["pistol"] = BuildClip( "pistol", 2.0f, PistolPose );
	set->m_stanceClips["melee_idle"] = BuildClip( "melee_idle", 2.0f, MeleeIdlePose );
	set->m_stanceClips["melee_walk"] = BuildClip( "melee_walk", anim_tuning::kWalkCycleSeconds, MeleeWalkPose );
	set->m_stanceClips["melee_run"] = BuildClip( "melee_run", anim_tuning::kRunCycleSeconds, MeleeRunPose );
	set->m_stanceClips["melee_swing"] = BuildClip( "melee_swing", 0.45f, MeleeSwingPose );
	set->m_description = "procedural placeholder rig";
	ComputeRestModels( *set, set->m_restModels, set->m_scale );
	set->ComputeAttachFrames();
	std::string ignored;
	set->SetAim( set->m_aimConfig, set->m_aimTipName, ignored );
	return set;
}

FileReader DiskReader( const std::string& dir )
{
	return [dir]( const std::string& name, std::string& bytes ) {
		std::ifstream in( dir + "/" + name, std::ios::binary );
		if ( in.good() == false )
		{
			return false;
		}
		std::ostringstream all;
		all << in.rdbuf();
		bytes = all.str();
		return true;
	};
}

std::unique_ptr<AnimSet> AnimSet::Load( const std::string& dir, std::string& error, std::string& warnings )
{
	return Load( DiskReader( dir ), dir, error, warnings );
}

std::unique_ptr<AnimSet> AnimSet::Load( const FileReader& read, const std::string& dir, std::string& error,
										std::string& warnings )
{
	std::string text;
	if ( read( "anim.cfg", text ) == false )
	{
		error = "no anim.cfg in " + dir;
		return nullptr;
	}
	auto cfg = ReadConfig( text );

	auto set = std::make_unique<AnimSet>();
	std::string skeletonFile = cfg.count( "skeleton" ) ? cfg["skeleton"] : "skeleton.ozz";
	set->m_skeleton = LoadArchive<ozz::animation::Skeleton>( read, skeletonFile );
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
	if ( cfg.count( "turn_legs" ) )
	{
		set->m_turnLegs = cfg["turn_legs"] != "false" && cfg["turn_legs"] != "0";
	}
	if ( cfg.count( "lock_root_xz" ) )
	{
		set->m_lockRootXZ = cfg["lock_root_xz"] != "false" && cfg["lock_root_xz"] != "0";
	}

	// A character with a state machine plays its own clips; the six built-in ones are optional.
	bool graph = read( "graph.cfg", set->m_graphText ) && set->m_graphText.empty() == false;
	int loaded = 0;
	for ( int c = 0; c < ClipCount; ++c )
	{
		const char* name = ClipName( Clip( c ) );
		std::string file = cfg.count( name ) ? cfg[name] : std::string( name ) + ".ozz";
		auto clip = LoadArchive<ozz::animation::Animation>( read, file );
		if ( clip == nullptr )
		{
			if ( graph == false )
			{
				warnings += std::string( "missing clip '" ) + name + "' (" + file + "); ";
			}
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
	set->ComputeAttachFrames();
	for ( const auto& [key, value] : cfg )
	{
		if ( key.rfind( "stance.", 0 ) == 0 )
		{
			std::string name = key.substr( 7 );
			auto clip = LoadArchive<ozz::animation::Animation>( read, value );
			if ( clip == nullptr || clip->num_tracks() != set->m_skeleton->num_joints() )
			{
				warnings += "stance clip '" + name + "' (" + value + ") could not be loaded for this skeleton; ";
				continue;
			}
			set->m_stanceClips[name] = std::move( clip );
		}
		else if ( key.rfind( "mask.", 0 ) == 0 )
		{
			set->m_masks[key.substr( 5 )] = value;
		}
		else if ( key.rfind( "clip.", 0 ) == 0 )
		{
			std::string name = key.substr( 5 );
			auto clip = LoadArchive<ozz::animation::Animation>( read, value );
			if ( clip == nullptr || clip->num_tracks() != set->m_skeleton->num_joints() )
			{
				warnings += "clip '" + name + "' (" + value + ") could not be loaded for this skeleton; ";
				continue;
			}
			set->m_namedClips[name] = std::move( clip );
		}
	}
	set->SetAim( cfg.count( "aim" ) ? cfg["aim"] : set->m_aimConfig, cfg.count( "aim_tip" ) ? cfg["aim_tip"] : set->m_aimTipName,
				 warnings );
	set->m_description = dir + " (" + std::to_string( set->m_skeleton->num_joints() ) + " joints, " +
						 ( graph ? "a state machine with " + std::to_string( set->m_namedClips.size() ) + " clips)"
								 : std::to_string( loaded ) + "/" + std::to_string( int( ClipCount ) ) + " clips)" );
	return set;
}

void AnimSet::SetAim( const std::string& chain, const std::string& tip, std::string& warnings )
{
	m_aimConfig = chain;
	m_aimTipName = tip;
	m_aimJoints.clear();
	std::istringstream in( chain );
	std::string entry;
	while ( in >> entry )
	{
		size_t colon = entry.find( ':' );
		std::string name = entry.substr( 0, colon );
		float weight = colon == std::string::npos ? 1.0f : float( std::atof( entry.c_str() + colon + 1 ) );
		int joint = FindJoint( *this, name.c_str() );
		if ( joint < 0 )
		{
			warnings += "aim joint '" + name + "' is not in the skeleton; ";
			continue;
		}
		m_aimJoints.emplace_back( joint, weight );
	}
	m_aimTip = FindJoint( *this, tip.c_str() );
	m_hipsJoint = FindJoint( *this, "Hips" );
	m_spineJoint = FindJoint( *this, "Spine" );
	if ( m_aimTip < 0 )
	{
		if ( m_aimJoints.empty() == false )
		{
			warnings += "aim tip '" + tip + "' is not in the skeleton; ";
		}
		m_aimJoints.clear();
	}
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
	cfg << "aim = " << m_aimConfig << "\n";
	cfg << "aim_tip = " << m_aimTipName << "\n";
	for ( const auto& [layer, roots] : m_masks )
	{
		cfg << "mask." << layer << " = " << roots << "\n";
	}
	for ( const auto& [name, clip] : m_stanceClips )
	{
		std::string file = "stance_" + name + ".ozz";
		if ( SaveArchive( dir + "/" + file, *clip ) == false )
		{
			return false;
		}
		cfg << "stance." << name << " = " << file << "\n";
	}
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
