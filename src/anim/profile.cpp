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
};

} // namespace

const char* ProfileName( const char* jointName )
{
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
