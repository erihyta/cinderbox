#include "cinderbox_skeleton.h"

#include "pose.h"
#include "profile.h"

#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

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
	ClassDB::bind_method( D_METHOD( "apply_anim_state", "mode", "mode_time", "locomotion_phase", "ground_speed" ),
						  &CinderboxSkeleton::apply_anim_state );
	ClassDB::bind_method( D_METHOD( "set_retarget", "value" ), &CinderboxSkeleton::set_retarget );
	ClassDB::bind_method( D_METHOD( "get_retarget" ), &CinderboxSkeleton::get_retarget );
	ClassDB::bind_method( D_METHOD( "get_joint_global_transform", "profile_name" ), &CinderboxSkeleton::get_joint_global_transform );
	ClassDB::bind_method( D_METHOD( "set_use_slot_color", "value" ), &CinderboxSkeleton::set_use_slot_color );
	ClassDB::bind_method( D_METHOD( "get_use_slot_color" ), &CinderboxSkeleton::get_use_slot_color );

	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "draw_bone_boxes" ), "set_draw_bone_boxes", "get_draw_bone_boxes" );
	ADD_PROPERTY( PropertyInfo( Variant::NODE_PATH, "skeleton_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Skeleton3D" ),
				  "set_skeleton_path", "get_skeleton_path" );
	ADD_PROPERTY( PropertyInfo( Variant::COLOR, "body_color" ), "set_body_color", "get_body_color" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "use_slot_color" ), "set_use_slot_color", "get_use_slot_color" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "retarget" ), "set_retarget", "get_retarget" );
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

void CinderboxSkeleton::set_retarget( bool value )
{
	m_retarget = value;
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
	ApplyPose( pose.Set(), pose.Models() );
}

bool CinderboxSkeleton::JointTransform( const String& profileName, Transform3D& out ) const
{
	if ( m_lastSet == nullptr )
	{
		return false;
	}
	CharString name = profileName.utf8();
	auto names = m_lastSet->Skeleton().joint_names();
	for ( size_t j = 0; j < names.size() && j < m_lastModels.size(); ++j )
	{
		const char* profile = anim::ProfileName( names[j] );
		if ( profile != nullptr && std::strcmp( profile, name.get_data() ) == 0 )
		{
			out = ToTransform( m_lastModels[j] ).orthonormalized();
			return true;
		}
	}
	return false;
}

Transform3D CinderboxSkeleton::get_joint_global_transform( const String& profile_name ) const
{
	Transform3D local;
	if ( JointTransform( profile_name, local ) == false )
	{
		return get_global_transform();
	}
	return get_global_transform() * local;
}

void CinderboxSkeleton::ApplyPose( const anim::AnimSet& set, const ozz::vector<ozz::math::Float4x4>& models )
{
	m_lastSet = &set;
	m_lastModels = models;
	const auto& skeleton = set.Skeleton();
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
			Bind( target, set );
		}
		DriveSkeleton( target, models );
		EnsureModifier( target );
	}
}

void CinderboxSkeleton::EnsureModifier( Skeleton3D* target )
{
	// The modifier is an internal child: it has to be looked for among those too.
	for ( int i = 0; i < target->get_child_count( true ); ++i )
	{
		if ( auto* existing = Object::cast_to<CbPoseModifier>( target->get_child( i, true ) ) )
		{
			existing->set_driver( this );
			return;
		}
	}
	CbPoseModifier* modifier = memnew( CbPoseModifier );
	modifier->set_name( "CinderboxPose" );
	modifier->set_driver( this );
	target->add_child( modifier, false, Node::INTERNAL_MODE_FRONT );
}

void CinderboxSkeleton::ReapplyPose( Skeleton3D* target )
{
	if ( m_lastSet == nullptr || m_lastModels.empty() || target == nullptr )
	{
		return;
	}
	if ( m_mappedSkeleton != target->get_instance_id() || int( m_boneMap.size() ) != m_lastSet->Skeleton().num_joints() )
	{
		Bind( target, *m_lastSet );
	}
	DriveSkeleton( target, m_lastModels );
}

void CbPoseModifier::set_driver( CinderboxSkeleton* driver )
{
	m_driver = driver != nullptr ? uint64_t( driver->get_instance_id() ) : 0;
}

void CbPoseModifier::_process_modification()
{
	if ( auto* driver = Object::cast_to<CinderboxSkeleton>( ObjectDB::get_instance( ObjectID( m_driver ) ) ) )
	{
		driver->ReapplyPose( get_skeleton() );
	}
}

namespace
{

// Joint names are matched as they are, then through the humanoid profile Godot retargets imported
// characters onto (so Mixamo-named clips drive a profile-named character and the other way round).
int FindTargetBone( Skeleton3D* target, const char* jointName )
{
	int bone = target->find_bone( String( jointName ) );
	if ( bone >= 0 )
	{
		return bone;
	}
	if ( const char* profile = anim::ProfileName( jointName ) )
	{
		bone = target->find_bone( String( profile ) );
		if ( bone >= 0 )
		{
			return bone;
		}
		if ( const char* mixamo = anim::MixamoName( profile ) )
		{
			return target->find_bone( String( mixamo ) );
		}
	}
	return -1;
}

// Our rig hangs its arms at rest; the humanoid profile's rest is a T-pose. Without this the two
// rests would be read as the same posture and every retargeted arm would stick out sideways.
Quaternion RestPosture( const char* jointName )
{
	std::string name = jointName;
	bool left = name.find( "Left" ) != std::string::npos;
	bool arm = name.find( "Arm" ) != std::string::npos || name.find( "Hand" ) != std::string::npos ||
			   name.find( "Shoulder" ) != std::string::npos;
	if ( arm == false )
	{
		return Quaternion();
	}
	// A quarter turn about Z takes the profile's outstretched arm down to where ours rests.
	float angle = left ? Math_PI * 0.5f : -Math_PI * 0.5f;
	return Quaternion( Vector3( 0, 0, 1 ), angle );
}

} // namespace

void CinderboxSkeleton::apply_anim_state( int mode, float mode_time, float locomotion_phase, float ground_speed )
{
	if ( !m_previewPose )
	{
		m_previewSet = anim::AnimSet::CreateProcedural();
		m_previewPose = std::make_unique<anim::PoseEvaluator>( *m_previewSet );
	}
	AnimState state;
	state.mode = AnimMode( std::min( std::max( mode, 0 ), int( AnimMode::Land ) ) );
	state.modeTime = mode_time;
	state.locomotionPhase = locomotion_phase;
	state.groundSpeed = ground_speed;
	m_previewPose->Evaluate( state );
	ApplyPose( *m_previewPose );
}

void CinderboxSkeleton::Bind( Skeleton3D* target, const anim::AnimSet& set )
{
	const auto& skeleton = set.Skeleton();
	auto names = skeleton.joint_names();
	int joints = skeleton.num_joints();

	m_boneMap.assign( size_t( joints ), -1 );
	// Parents before children, whatever order the target lists its bones in.
	m_boneOrder.clear();
	PackedInt32Array roots = target->get_parentless_bones();
	for ( int64_t r = 0; r < roots.size(); ++r )
	{
		m_boneOrder.push_back( roots[r] );
	}
	for ( size_t i = 0; i < m_boneOrder.size(); ++i )
	{
		PackedInt32Array children = target->get_bone_children( m_boneOrder[i] );
		for ( int64_t c = 0; c < children.size(); ++c )
		{
			m_boneOrder.push_back( children[c] );
		}
	}
	m_restBridge.assign( size_t( joints ), Quaternion() );
	m_mappedSkeleton = target->get_instance_id();
	m_targetHips = -1;
	m_hipScale = 1.0f;

	// Our rig's rest in model space, which is what the pose is relative to.
	const ozz::vector<ozz::math::Float4x4>& restModels = set.RestModels();

	for ( int j = 0; j < joints; ++j )
	{
		int bone = FindTargetBone( target, names[j] );
		m_boneMap[size_t( j )] = bone;
		if ( bone < 0 )
		{
			continue;
		}

		Basis sourceRest = ToTransform( restModels[size_t( j )] ).basis;
		Quaternion sourceRestRotation = sourceRest.orthonormalized().get_rotation_quaternion();
		Quaternion targetRest = target->get_bone_global_rest( bone ).basis.orthonormalized().get_rotation_quaternion();

		// Reading our rest as if it were the target's rest posture, this is the constant that
		// turns one into the other. Applying it to a pose keeps the target's own rest as the
		// baseline, so its proportions and its rest posture both survive.
		Quaternion reference = sourceRestRotation * RestPosture( names[j] );
		m_restBridge[size_t( j )] = reference.inverse() * targetRest;

		if ( std::strcmp( names[j], "Hips" ) == 0 || std::strcmp( names[j], "mixamorig:Hips" ) == 0 )
		{
			m_targetHips = bone;
			m_sourceHipsRest = ToTransform( restModels[size_t( j )] ).origin;
			m_targetHipsRest = target->get_bone_global_rest( bone ).origin;
			// A taller character's hips have to travel further for the same crouch.
			m_hipScale = m_sourceHipsRest.y > 0.001f ? float( m_targetHipsRest.y / m_sourceHipsRest.y ) : 1.0f;
		}
	}
}

void CinderboxSkeleton::DriveSkeleton( Skeleton3D* target, const ozz::vector<ozz::math::Float4x4>& models )
{
	int joints = int( m_boneMap.size() );

	if ( m_retarget == false )
	{
		// Exact-match rigs: every driven bone where our rig has it. Its global pose is the model
		// matrix; bones we do not drive follow their parent with their current local pose. Setting
		// local poses parents first lets the skeleton rebuild its globals once, not once per bone
		// (set_bone_global_pose recomputes the hierarchy on every call, which cost milliseconds per
		// character when it ran twice a frame).
		size_t bones = size_t( target->get_bone_count() );
		m_globals.resize( bones );
		m_known.assign( bones, 0 );
		for ( int j = 0; j < joints; ++j )
		{
			int bone = m_boneMap[size_t( j )];
			if ( bone >= 0 && size_t( bone ) < bones )
			{
				m_globals[size_t( bone )] = ToTransform( models[size_t( j )] );
				m_known[size_t( bone )] = 1;
			}
		}
		for ( int bone : m_boneOrder )
		{
			int parent = target->get_bone_parent( bone );
			Transform3D parentGlobal = parent >= 0 ? m_globals[size_t( parent )] : Transform3D();
			if ( m_known[size_t( bone )] == 0 )
			{
				m_globals[size_t( bone )] = parentGlobal * target->get_bone_pose( bone );
				continue;
			}
			Transform3D local = parentGlobal.affine_inverse() * m_globals[size_t( bone )];
			Vector3 scale = local.basis.get_scale();
			target->set_bone_pose_position( bone, local.origin );
			target->set_bone_pose_rotation( bone, local.basis.orthonormalized().get_rotation_quaternion() );
			target->set_bone_pose_scale( bone, scale );
		}
		return;
	}

	// Rotation only, bone by bone, so the target keeps its own bone lengths. Global rotations are
	// built first and then turned into local ones, because a target may have bones ours does not.
	int boneCount = target->get_bone_count();
	size_t bones = size_t( boneCount );
	std::vector<Quaternion> global( bones );
	std::vector<bool> known( bones, false );

	for ( int j = 0; j < joints; ++j )
	{
		int bone = m_boneMap[size_t( j )];
		if ( bone < 0 )
		{
			continue;
		}
		Quaternion sourcePose = ToTransform( models[size_t( j )] ).basis.orthonormalized().get_rotation_quaternion();
		global[size_t( bone )] = sourcePose * m_restBridge[size_t( j )];
		known[size_t( bone )] = true;
	}

	for ( int bone = 0; bone < boneCount; ++bone )
	{
		int parent = target->get_bone_parent( bone );
		Quaternion parentGlobal = parent >= 0 ? global[size_t( parent )] : Quaternion();
		if ( known[size_t( bone )] == false )
		{
			// A bone we do not drive keeps its rest shape and follows whatever drives its parent.
			global[size_t( bone )] = parentGlobal * target->get_bone_rest( bone ).basis.orthonormalized().get_rotation_quaternion();
			continue;
		}
		target->set_bone_pose_rotation( bone, parentGlobal.inverse() * global[size_t( bone )] );
	}

	if ( m_targetHips >= 0 )
	{
		// The hips are the one bone that also moves: crouching and bobbing live there.
		Vector3 offset = ToTransform( models[0] ).origin - m_sourceHipsRest;
		Vector3 rest = target->get_bone_rest( m_targetHips ).origin;
		target->set_bone_pose_position( m_targetHips, rest + offset * m_hipScale );
	}
}

} // namespace cb::gd
