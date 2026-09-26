#include "profile.h"

#include <cstring>

namespace cb::anim
{

namespace
{

struct Alias
{
	const char* mixamo;
	const char* profile;
};

const Alias kAliases[] = {
	{ "mixamorig:Hips", "Hips" },
	{ "mixamorig:Spine", "Spine" },
	{ "mixamorig:Spine1", "Chest" },
	{ "mixamorig:Spine2", "UpperChest" },
	{ "mixamorig:Neck", "Neck" },
	{ "mixamorig:Head", "Head" },
	{ "mixamorig:LeftShoulder", "LeftShoulder" },
	{ "mixamorig:LeftArm", "LeftUpperArm" },
	{ "mixamorig:LeftForeArm", "LeftLowerArm" },
	{ "mixamorig:LeftHand", "LeftHand" },
	{ "mixamorig:LeftHandMiddle1", "LeftMiddleProximal" },
	{ "mixamorig:RightShoulder", "RightShoulder" },
	{ "mixamorig:RightArm", "RightUpperArm" },
	{ "mixamorig:RightForeArm", "RightLowerArm" },
	{ "mixamorig:RightHand", "RightHand" },
	{ "mixamorig:RightHandMiddle1", "RightMiddleProximal" },
	{ "mixamorig:LeftUpLeg", "LeftUpperLeg" },
	{ "mixamorig:LeftLeg", "LeftLowerLeg" },
	{ "mixamorig:LeftFoot", "LeftFoot" },
	{ "mixamorig:LeftToeBase", "LeftToes" },
	{ "mixamorig:RightUpLeg", "RightUpperLeg" },
	{ "mixamorig:RightLeg", "RightLowerLeg" },
	{ "mixamorig:RightFoot", "RightFoot" },
	{ "mixamorig:RightToeBase", "RightToes" },
	{ "mixamorig:LeftHandThumb1", "LeftThumbMetacarpal" },
	{ "mixamorig:LeftHandThumb2", "LeftThumbProximal" },
	{ "mixamorig:LeftHandThumb3", "LeftThumbDistal" },
	{ "mixamorig:LeftHandIndex1", "LeftIndexProximal" },
	{ "mixamorig:LeftHandIndex2", "LeftIndexIntermediate" },
	{ "mixamorig:LeftHandIndex3", "LeftIndexDistal" },
	{ "mixamorig:LeftHandMiddle2", "LeftMiddleIntermediate" },
	{ "mixamorig:LeftHandMiddle3", "LeftMiddleDistal" },
	{ "mixamorig:LeftHandRing1", "LeftRingProximal" },
	{ "mixamorig:LeftHandRing2", "LeftRingIntermediate" },
	{ "mixamorig:LeftHandRing3", "LeftRingDistal" },
	{ "mixamorig:LeftHandPinky1", "LeftLittleProximal" },
	{ "mixamorig:LeftHandPinky2", "LeftLittleIntermediate" },
	{ "mixamorig:LeftHandPinky3", "LeftLittleDistal" },
	{ "mixamorig:RightHandThumb1", "RightThumbMetacarpal" },
	{ "mixamorig:RightHandThumb2", "RightThumbProximal" },
	{ "mixamorig:RightHandThumb3", "RightThumbDistal" },
	{ "mixamorig:RightHandIndex1", "RightIndexProximal" },
	{ "mixamorig:RightHandIndex2", "RightIndexIntermediate" },
	{ "mixamorig:RightHandIndex3", "RightIndexDistal" },
	{ "mixamorig:RightHandMiddle2", "RightMiddleIntermediate" },
	{ "mixamorig:RightHandMiddle3", "RightMiddleDistal" },
	{ "mixamorig:RightHandRing1", "RightRingProximal" },
	{ "mixamorig:RightHandRing2", "RightRingIntermediate" },
	{ "mixamorig:RightHandRing3", "RightRingDistal" },
	{ "mixamorig:RightHandPinky1", "RightLittleProximal" },
	{ "mixamorig:RightHandPinky2", "RightLittleIntermediate" },
	{ "mixamorig:RightHandPinky3", "RightLittleDistal" },
};

// SkeletonProfileHumanoid's names that have no Mixamo counterpart above.
const char* const kProfileOnly[] = {
	"Root", "Jaw", "LeftEye", "RightEye",
};

} // namespace

const char* ProfileName( const char* jointName )
{
	for ( const char* name : kProfileOnly )
	{
		if ( std::strcmp( name, jointName ) == 0 )
		{
			return name;
		}
	}
	for ( const Alias& a : kAliases )
	{
		if ( std::strcmp( a.profile, jointName ) == 0 )
		{
			return a.profile;
		}
		if ( std::strcmp( a.mixamo, jointName ) == 0 )
		{
			return a.profile;
		}
	}
	return nullptr;
}

const char* MixamoName( const char* profileName )
{
	for ( const Alias& a : kAliases )
	{
		if ( std::strcmp( a.profile, profileName ) == 0 )
		{
			return a.mixamo;
		}
	}
	return nullptr;
}

} // namespace cb::anim
