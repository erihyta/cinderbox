#include "cinderbox_skeleton.h"

#include "pose.h"

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace cb::gd
{

namespace
{

struct BoneStyle
{
	float width;
	float depth;
	bool body;
};

// Mixamo naming; anything unknown gets a size from its length (same as the raylib client).
BoneStyle StyleFor( const std::string& n, float length )
{
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
	return Vector3( v[0], v[1], v[2] );
}

Transform3D ToTransform( const ozz::math::Float4x4& m )
{
	// Godot's three-vector Basis constructor takes columns, like ozz matrices store them.
	return Transform3D( Basis( Column( m, 0 ), Column( m, 1 ), Column( m, 2 ) ), Column( m, 3 ) );
}

// A unit box stretched from `a` to `b`, oriented by `refX`.
Transform3D BoneBox( Vector3 a, Vector3 b, Vector3 refX, float width, float depth )
{
	Vector3 axis = b - a;
	float length = axis.length();
	if ( length < 1e-4f )
	{
		return Transform3D( Basis().scaled( Vector3( 0, 0, 0 ) ), a );
	}
	Vector3 y = axis / length;
	Vector3 x = refX - y * refX.dot( y );
	if ( x.length() < 1e-3f )
	{
		x = std::fabs( y.x ) < 0.9f ? Vector3( 1, 0, 0 ) : Vector3( 0, 0, 1 );
		x = x - y * x.dot( y );
	}
	x = x.normalized();
	Vector3 z = x.cross( y );
	Basis basis = Basis( x * width, y * length, z * depth ); // columns
	return Transform3D( basis, ( a + b ) * 0.5f );
}

} // namespace

void CinderboxSkeleton::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_draw_bone_boxes", "value" ), &CinderboxSkeleton::set_draw_bone_boxes );
	ClassDB::bind_method( D_METHOD( "get_draw_bone_boxes" ), &CinderboxSkeleton::get_draw_bone_boxes );
	ClassDB::bind_method( D_METHOD( "set_skeleton_path", "path" ), &CinderboxSkeleton::set_skeleton_path );
	ClassDB::bind_method( D_METHOD( "get_skeleton_path" ), &CinderboxSkeleton::get_skeleton_path );
	ClassDB::bind_method( D_METHOD( "set_body_color", "color" ), &CinderboxSkeleton::set_body_color );
	ClassDB::bind_method( D_METHOD( "get_body_color" ), &CinderboxSkeleton::get_body_color );
	ClassDB::bind_method( D_METHOD( "set_use_slot_color", "value" ), &CinderboxSkeleton::set_use_slot_color );
	ClassDB::bind_method( D_METHOD( "get_use_slot_color" ), &CinderboxSkeleton::get_use_slot_color );

	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "draw_bone_boxes" ), "set_draw_bone_boxes", "get_draw_bone_boxes" );
	ADD_PROPERTY( PropertyInfo( Variant::NODE_PATH, "skeleton_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Skeleton3D" ),
				  "set_skeleton_path", "get_skeleton_path" );
	ADD_PROPERTY( PropertyInfo( Variant::COLOR, "body_color" ), "set_body_color", "get_body_color" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "use_slot_color" ), "set_use_slot_color", "get_use_slot_color" );
}

void CinderboxSkeleton::_ready()
{
	if ( m_boxes == nullptr )
	{
		m_boxes = memnew( MultiMeshInstance3D );
		m_boxes->set_name( "BoneBoxes" );
		add_child( m_boxes );
	}
	m_boxes->set_visible( m_drawBoneBoxes );
}

void CinderboxSkeleton::set_draw_bone_boxes( bool value )
{
	m_drawBoneBoxes = value;
	if ( m_boxes != nullptr )
	{
		m_boxes->set_visible( value );
	}
}

void CinderboxSkeleton::set_skeleton_path( const NodePath& path )
{
	m_skeletonPath = path;
	m_mappedSkeleton = 0;
}

void CinderboxSkeleton::set_body_color( const Color& color )
{
	m_bodyColor = color;
}

void CinderboxSkeleton::EnsureMultiMesh( int instances )
{
	if ( m_multimesh.is_valid() && m_multimesh->get_instance_count() == instances )
	{
		return;
	}
	Ref<BoxMesh> box;
	box.instantiate();
	box->set_size( Vector3( 1, 1, 1 ) );
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_flag( BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true );
	material->set_roughness( 0.8f );
	box->set_material( material );

	m_multimesh.instantiate();
	m_multimesh->set_transform_format( MultiMesh::TRANSFORM_3D );
	m_multimesh->set_use_colors( true );
	m_multimesh->set_mesh( box );
	m_multimesh->set_instance_count( instances );
	if ( m_boxes != nullptr )
	{
		m_boxes->set_multimesh( m_multimesh );
	}
}

void CinderboxSkeleton::ApplyPose( const anim::PoseEvaluator& pose )
{
	const auto& skeleton = pose.Set().Skeleton();
	const auto& models = pose.Models();
	auto parents = skeleton.joint_parents();
	auto names = skeleton.joint_names();
	int joints = skeleton.num_joints();

	if ( m_drawBoneBoxes && m_boxes != nullptr )
	{
		// One box per joint with a parent, plus the nose.
		EnsureMultiMesh( joints + 1 );
		Color body = m_bodyColor;
		Color limbs = m_bodyColor.darkened( 0.25f );
		int instance = 0;
		for ( int j = 0; j < joints; ++j )
		{
			int p = parents[j];
			if ( p < 0 )
			{
				m_multimesh->set_instance_transform( instance, Transform3D( Basis().scaled( Vector3() ), Vector3() ) );
				++instance;
				continue;
			}
			Vector3 a = Column( models[p], 3 );
			Vector3 b = Column( models[j], 3 );
			std::string name = names[j];
			BoneStyle style = StyleFor( name, a.distance_to( b ) );
			m_multimesh->set_instance_transform( instance, BoneBox( a, b, Column( models[p], 0 ).normalized(), style.width, style.depth ) );
			m_multimesh->set_instance_color( instance, style.body ? body : limbs );
			++instance;

			if ( name.find( "HeadTop" ) != std::string::npos )
			{
				Vector3 nose = a.lerp( b, 0.45f ) + Vector3( 0, 0, 0.14f );
				m_multimesh->set_instance_transform( joints, Transform3D( Basis().scaled( Vector3( 0.06f, 0.06f, 0.06f ) ), nose ) );
				m_multimesh->set_instance_color( joints, Color( 0.25f, 0.25f, 0.25f ) );
			}
		}
	}

	if ( m_skeletonPath.is_empty() == false )
	{
		Skeleton3D* target = Object::cast_to<Skeleton3D>( get_node_or_null( m_skeletonPath ) );
		if ( target == nullptr )
		{
			return;
		}
		if ( m_mappedSkeleton != target->get_instance_id() || int( m_boneMap.size() ) != joints )
		{
			m_boneMap.assign( size_t( joints ), -1 );
			for ( int j = 0; j < joints; ++j )
			{
				m_boneMap[size_t( j )] = target->find_bone( String( names[j] ) );
			}
			m_mappedSkeleton = target->get_instance_id();
		}
		// Model-space poses map directly when the rig shares the ozz skeleton's root space.
		for ( int j = 0; j < joints; ++j )
		{
			int bone = m_boneMap[size_t( j )];
			if ( bone >= 0 )
			{
				target->set_bone_global_pose( bone, ToTransform( models[j] ) );
			}
		}
	}
}

} // namespace cb::gd
