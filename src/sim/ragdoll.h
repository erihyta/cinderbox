#pragma once

// The ragdoll's body layout: simulation constants, shared with presentation so it can pose a
// skeleton from the parts.
//
// Everything is in character space: feet at the origin, facing +Z, the character's left on +X,
// matching the placeholder rig's rest (arms down). A ragdoll starts from this standing pose, turned
// to the player's facing, which keeps asset data out of the simulation: clients blend from whatever
// pose they last drew into it. Changing any value here changes simulation results.

#include "components.h"

namespace cb::ragdoll
{

enum Part : int
{
	Pelvis,
	Torso,
	Head,
	LeftUpperArm,
	LeftLowerArm,
	RightUpperArm,
	RightLowerArm,
	LeftUpperLeg,
	LeftLowerLeg,
	RightUpperLeg,
	RightLowerLeg,
	PartCount,
};
static_assert( PartCount == kRagdollParts );

enum class JointKind : uint8_t
{
	None,	   // the root part
	Spherical, // cone and twist limits around the bone
	Hinge,	   // one axis: character-space X, limited to [lower, upper]
};

struct PartDef
{
	const char* name;
	ShapeKind shape;
	// Box: half extents. Sphere: x = radius. Capsule: x = radius, y = half distance between the
	// sphere centres, along the part's Y.
	b3Vec3 size;
	b3Vec3 center; // rest position of the body's origin
	int parent;
	JointKind joint;
	b3Vec3 anchor; // rest position of the joint to the parent
	// Spherical: the bone points along +Y (1) or -Y (-1) from the anchor; cone and twist limits.
	// Hinge: the angle range about +X (a positive angle swings the child's end backwards).
	float direction;
	float cone;
	float lower;
	float upper;
	// The rig joint this part carries in presentation, and the one where its segment ends.
	const char* rigJoint;
};

// Where the capsule's centre is above the feet while standing (see the character mover's pogo).
inline constexpr float kFeetBelowCenter = 1.38f;
inline constexpr float kDensity = 20.0f;
inline constexpr float kFriction = 0.7f;
inline constexpr float kLinearDamping = 0.05f;
inline constexpr float kAngularDamping = 0.4f;

// clang-format off
inline constexpr PartDef kParts[PartCount] = {
	{ "Pelvis",        ShapeKind::Box,     { 0.13f, 0.09f, 0.10f },  { 0.0f, 0.97f, 0.0f },   -1,           JointKind::None,      { 0.0f, 0.97f, 0.0f },   0.0f,  0.0f, 0.0f,   0.0f,  "Hips" },
	{ "Torso",         ShapeKind::Box,     { 0.13f, 0.19f, 0.10f },  { 0.0f, 1.26f, 0.0f },   Pelvis,       JointKind::Spherical, { 0.0f, 1.06f, 0.0f },   1.0f,  0.5f, -0.3f, 0.3f,  "Spine" },
	{ "Head",          ShapeKind::Sphere,  { 0.12f, 0.0f, 0.0f },    { 0.0f, 1.62f, 0.0f },   Torso,        JointKind::Spherical, { 0.0f, 1.47f, 0.0f },   1.0f,  0.6f, -0.5f, 0.5f,  "Neck" },
	{ "LeftUpperArm",  ShapeKind::Capsule, { 0.05f, 0.085f, 0.0f },  { 0.19f, 1.255f, 0.0f }, Torso,        JointKind::Spherical, { 0.19f, 1.37f, 0.0f },  -1.0f, 1.6f, -0.8f, 0.8f,  "LeftUpperArm" },
	{ "LeftLowerArm",  ShapeKind::Capsule, { 0.045f, 0.115f, 0.0f }, { 0.19f, 0.96f, 0.0f },  LeftUpperArm, JointKind::Hinge,     { 0.19f, 1.12f, 0.0f },  0.0f,  0.0f, -2.4f, 0.05f, "LeftLowerArm" },
	{ "RightUpperArm", ShapeKind::Capsule, { 0.05f, 0.085f, 0.0f },  { -0.19f, 1.255f, 0.0f }, Torso,       JointKind::Spherical, { -0.19f, 1.37f, 0.0f }, -1.0f, 1.6f, -0.8f, 0.8f,  "RightUpperArm" },
	{ "RightLowerArm", ShapeKind::Capsule, { 0.045f, 0.115f, 0.0f }, { -0.19f, 0.96f, 0.0f }, RightUpperArm, JointKind::Hinge,    { -0.19f, 1.12f, 0.0f }, 0.0f,  0.0f, -2.4f, 0.05f, "RightLowerArm" },
	{ "LeftUpperLeg",  ShapeKind::Capsule, { 0.06f, 0.15f, 0.0f },   { 0.10f, 0.69f, 0.0f },  Pelvis,       JointKind::Spherical, { 0.10f, 0.90f, 0.0f },  -1.0f, 1.2f, -0.4f, 0.4f,  "LeftUpperLeg" },
	{ "LeftLowerLeg",  ShapeKind::Capsule, { 0.055f, 0.175f, 0.0f }, { 0.10f, 0.25f, 0.0f },  LeftUpperLeg, JointKind::Hinge,     { 0.10f, 0.48f, 0.0f },  0.0f,  0.0f, -0.05f, 2.4f, "LeftLowerLeg" },
	{ "RightUpperLeg", ShapeKind::Capsule, { 0.06f, 0.15f, 0.0f },   { -0.10f, 0.69f, 0.0f }, Pelvis,       JointKind::Spherical, { -0.10f, 0.90f, 0.0f }, -1.0f, 1.2f, -0.4f, 0.4f,  "RightUpperLeg" },
	{ "RightLowerLeg", ShapeKind::Capsule, { 0.055f, 0.175f, 0.0f }, { -0.10f, 0.25f, 0.0f }, RightUpperLeg, JointKind::Hinge,    { -0.10f, 0.48f, 0.0f }, 0.0f,  0.0f, -0.05f, 2.4f, "RightLowerLeg" },
};
// clang-format on

} // namespace cb::ragdoll
