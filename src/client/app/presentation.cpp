#include "presentation.h"


#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <string>

#include <cmath>

namespace cb::present
{

namespace
{


constexpr Color kPlayerColors[] = {
	{ 230, 80, 70, 255 },  { 70, 140, 230, 255 }, { 90, 200, 110, 255 }, { 200, 120, 220, 255 },
	{ 240, 160, 60, 255 }, { 60, 200, 200, 255 }, { 180, 180, 90, 255 }, { 150, 110, 80, 255 },
};

Vector3 ToRay( b3Vec3 v )
{
	return { v.x, v.y, v.z };
}

Quaternion ToRay( b3Quat q )
{
	return { q.v.x, q.v.y, q.v.z, q.s };
}

uint32_t Mix( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352d;
	x ^= x >> 15;
	x *= 0x846ca68b;
	x ^= x >> 16;
	return x;
}

Color PropColor( uint32_t netId, bool sphere )
{
	uint32_t h = Mix( netId );
	unsigned char r = uint8_t( 120 + ( h & 0x7F ) );
	unsigned char g = uint8_t( 100 + ( ( h >> 8 ) & 0x7F ) );
	unsigned char b = uint8_t( 90 + ( ( h >> 16 ) & 0x7F ) );
	return sphere ? Color{ b, r, g, 255 } : Color{ r, g, b, 255 };
}

void PushPose( Vector3 position, Quaternion rotation )
{
	rlPushMatrix();
	rlTranslatef( position.x, position.y, position.z );
	Vector3 axis;
	float angle;
	QuaternionToAxisAngle( rotation, &axis, &angle );
	if ( angle != 0.0f )
	{
		rlRotatef( angle * RAD2DEG, axis.x, axis.y, axis.z );
	}
}

void DrawBlock( Vector3 center, Vector3 size, Color color )
{
	DrawCube( center, size.x, size.y, size.z, color );
	DrawCubeWires( center, size.x, size.y, size.z, Fade( BLACK, 0.35f ) );
}

// Bone box sizes by joint name (Mixamo naming; anything unknown gets a size from its length).
struct BoneStyle
{
	float width;
	float depth;
	bool body; // body colour instead of limb colour
};

BoneStyle StyleFor( const char* name, float length )
{
	std::string n = name;
	auto has = [&]( const char* part ) { return n.find( part ) != std::string::npos; };
	if ( has( "HeadTop" ) )
		return { 0.26f, 0.26f, true };
	if ( has( "Spine" ) )
		return { 0.34f, 0.2f, true };
	if ( has( "Neck" ) || has( "Head" ) )
		return { 0.1f, 0.1f, true };
	if ( has( "Shoulder" ) )
		return { 0.1f, 0.1f, true };
	if ( has( "UpLeg" ) )
		return { 0.15f, 0.16f, false };
	if ( has( "Leg" ) )
		return { 0.12f, 0.13f, false };
	if ( has( "Toe" ) || has( "Foot" ) )
		return { 0.1f, 0.1f, false };
	if ( has( "Hand" ) )
		return { 0.07f, 0.04f, false };
	if ( has( "Arm" ) )
		return { 0.1f, 0.1f, false };
	float w = std::clamp( length * 0.4f, 0.02f, 0.12f );
	return { w, w, false };
}

Vector3 Column( const ozz::math::Float4x4& m, int c )
{
	float v[4];
	ozz::math::StorePtrU( m.cols[c], v );
	return { v[0], v[1], v[2] };
}

// A box spanning from `a` to `b`, oriented by `refX` (usually the parent joint's X axis).
void DrawBone( Vector3 a, Vector3 b, Vector3 refX, float width, float depth, Color color )
{
	Vector3 axis = Vector3Subtract( b, a );
	float length = Vector3Length( axis );
	if ( length < 1e-4f )
	{
		return;
	}
	Vector3 y = Vector3Scale( axis, 1.0f / length );
	Vector3 x = Vector3Subtract( refX, Vector3Scale( y, Vector3DotProduct( refX, y ) ) );
	if ( Vector3Length( x ) < 1e-3f )
	{
		x = std::fabs( y.x ) < 0.9f ? Vector3{ 1, 0, 0 } : Vector3{ 0, 0, 1 };
		x = Vector3Subtract( x, Vector3Scale( y, Vector3DotProduct( x, y ) ) );
	}
	x = Vector3Normalize( x );
	Vector3 z = Vector3CrossProduct( x, y );
	Vector3 mid = Vector3Lerp( a, b, 0.5f );

	Matrix m = { 0 };
	m.m0 = x.x * width;
	m.m1 = x.y * width;
	m.m2 = x.z * width;
	m.m4 = y.x * length;
	m.m5 = y.y * length;
	m.m6 = y.z * length;
	m.m8 = z.x * depth;
	m.m9 = z.y * depth;
	m.m10 = z.z * depth;
	m.m12 = mid.x;
	m.m13 = mid.y;
	m.m14 = mid.z;
	m.m15 = 1.0f;

	rlPushMatrix();
	rlMultMatrixf( MatrixToFloat( m ) );
	DrawCube( { 0, 0, 0 }, 1.0f, 1.0f, 1.0f, color );
	DrawCubeWires( { 0, 0, 0 }, 1.0f, 1.0f, 1.0f, Fade( BLACK, 0.35f ) );
	rlPopMatrix();
}

} // namespace

void DrawSkeleton( Vector3 feet, Quaternion rotation, float scale, const anim::PoseEvaluator* eval, Color color )
{
	DrawSkeleton( feet, rotation, scale, eval ? &eval->Set() : nullptr, eval ? &eval->Models() : nullptr, color );
}

void DrawSkeleton( Vector3 feet, Quaternion rotation, float scale, const anim::AnimSet* set, const Models* pose, Color color )
{
	Color body = color;
	Color limbs = ColorBrightness( color, -0.25f );

	// Skeleton space: feet at the origin, facing +Z.
	PushPose( feet, rotation );
	rlScalef( scale, scale, scale );

	if ( set != nullptr && pose != nullptr && pose->empty() == false )
	{
		const auto& skeleton = set->Skeleton();
		const auto& models = *pose;
		auto parents = skeleton.joint_parents();
		auto names = skeleton.joint_names();

		for ( int j = 0; j < skeleton.num_joints(); ++j )
		{
			int p = parents[j];
			if ( p < 0 )
			{
				continue;
			}
			Vector3 a = Column( models[p], 3 );
			Vector3 b = Column( models[j], 3 );
			BoneStyle style = StyleFor( names[j], Vector3Distance( a, b ) );
			DrawBone( a, b, Vector3Normalize( Column( models[p], 0 ) ), style.width, style.depth, style.body ? body : limbs );

			// Nose on the head so facing is readable.
			if ( std::string( names[j] ).find( "HeadTop" ) != std::string::npos )
			{
				Vector3 mid = Vector3Lerp( a, b, 0.45f );
				DrawCube( { mid.x, mid.y, mid.z + 0.14f }, 0.06f, 0.06f, 0.06f, DARKGRAY );
			}
		}
	}
	else
	{
		DrawCube( { 0.0f, 0.9f, 0.0f }, 0.5f, 1.8f, 0.3f, body );
	}
	rlPopMatrix();
}

namespace
{

void DrawPlayer( const RenderPose& pose, const Visual& v, const PlayerAnim* anim, Color color )
{
	Vector3 feet = { pose.position.x, pose.position.y - kFeetOffset, pose.position.z };
	DrawSkeleton( feet, ToRay( pose.rotation ), pose.scale, anim ? anim->evaluator.get() : nullptr, color );

	if ( v.isLocalPlayer )
	{
		Vector3 top = { feet.x, feet.y + 2.1f, feet.z };
		DrawCylinder( top, 0.12f, 0.0f, 0.2f, 4, GOLD );
	}
}

} // namespace

Presentation::Presentation( std::shared_ptr<const anim::AnimSet> animSet )
	: m_mirror( std::move( animSet ) )
{
}

void Presentation::Update( const SimView& view, float frameSeconds )
{
	if ( view.sim == nullptr )
	{
		return;
	}
	CaptureFrame( *view.sim, m_frame );
	m_frame.resetGeneration = view.resetGeneration;
	m_frame.rolledBack = view.rolledBack;
	m_frame.localNetId = view.hasLocalPlayer ? view.sim->Globals().playerNetIds[view.localSlot] : 0;
	m_mirror.Update( m_frame, view.tickAlpha, frameSeconds );
}

bool Presentation::LocalPlayerPosition( Vector3& out ) const
{
	RenderPose pose;
	if ( m_mirror.LocalPlayer( pose ) == false )
	{
		return false;
	}
	out = ToRay( pose.position );
	return true;
}

void Presentation::Render()
{
	m_mirror.ForEach( [&]( uint64_t, const Visual& v, const RenderPose& pose, const PlayerAnim* anim, const RagdollAnim* ragdoll ) {
		if ( pose.scale <= 0.001f || v.dead )
		{
			return;
		}
		if ( v.kind == VisualKind::Ragdoll )
		{
			if ( ragdoll != nullptr )
			{
				Color body = kPlayerColors[v.slot % ( sizeof( kPlayerColors ) / sizeof( kPlayerColors[0] ) )];
				DrawSkeleton( ToRay( pose.position ), ToRay( pose.rotation ), pose.scale, &m_mirror.AnimSet(), &ragdoll->models, body );
			}
			return;
		}

		Color color;
		switch ( v.kind )
		{
			case VisualKind::Static:
			{
				float shade = 0.35f + 0.1f * std::fmin( pose.position.y, 3.0f );
				unsigned char c = uint8_t( 255.0f * std::fmin( shade, 0.75f ) );
				color = { c, c, uint8_t( c + 12 ), 255 };
				break;
			}
			case VisualKind::Player:
				color = kPlayerColors[v.slot % ( sizeof( kPlayerColors ) / sizeof( kPlayerColors[0] ) )];
				DrawPlayer( pose, v, anim, color );
				return;
			case VisualKind::Prop:
				color = PropColor( v.netId, v.shape == ShapeKind::Sphere );
				break;
			case VisualKind::Ragdoll:
				return;
		}

		Vector3 size = { 2.0f * v.halfExtents.x * pose.scale, 2.0f * v.halfExtents.y * pose.scale,
						 2.0f * v.halfExtents.z * pose.scale };
		PushPose( ToRay( pose.position ), ToRay( pose.rotation ) );
		switch ( v.shape )
		{
			case ShapeKind::Box:
				DrawBlock( { 0, 0, 0 }, size, color );
				break;
			case ShapeKind::Sphere:
			{
				float r = v.halfExtents.x * pose.scale;
				DrawSphereEx( { 0, 0, 0 }, r, 10, 12, color );
				// A band so rolling is visible.
				DrawCylinderWires( { 0, -0.05f * r, 0 }, r * 1.01f, r * 1.01f, 0.1f * r, 12, Fade( BLACK, 0.5f ) );
				break;
			}
			case ShapeKind::Capsule:
			{
				float r = v.halfExtents.x * pose.scale;
				float h = v.halfExtents.y * pose.scale;
				DrawCapsule( { 0, -h, 0 }, { 0, h, 0 }, r, 10, 6, color );
				break;
			}
		}
		rlPopMatrix();
	} );
}

} // namespace cb::present
