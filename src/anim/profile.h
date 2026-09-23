#pragma once

// Joint names: Godot's SkeletonProfileHumanoid, which the placeholder rig uses and Godot retargets
// imported characters onto, plus the Mixamo names clips often still carry.

namespace cb::anim
{

// The humanoid-profile name of a joint: the name itself when it already is one, its profile name
// when it is a known Mixamo name, otherwise nullptr.
const char* ProfileName( const char* jointName );

// The Mixamo name for a profile joint ("mixamorig:LeftArm" for "LeftUpperArm"), or nullptr.
const char* MixamoName( const char* profileName );

} // namespace cb::anim
