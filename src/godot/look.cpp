#include "look.h"

#include "cinderbox_character.h" // CbSocket, CbHitbox
#include "cinderbox_item_look.h" // CbItem::HandSocketFrame
#include "cinderbox_skeleton.h"

#include "anim_set.h"
#include "pose.h"
#include "profile.h"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/bone_attachment3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/skeleton_modifier3d.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/animation_tree.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/cpu_particles3d.hpp>
#include <godot_cpp/classes/gpu_particles3d.hpp>
#include <godot_cpp/classes/light3d.hpp>
#include <godot_cpp/classes/area3d.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/marker3d.hpp>

#include "cinderbox_motion.h" // CbMotionPart, CbCurve
#include "cue_rules.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

using namespace godot;

namespace cb::gd
{

namespace
{

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

// Where `node` is in `ancestor`'s frame (identity for the node itself).
Transform3D RelativeTo( const Node* ancestor, Node* node )
{
	Transform3D out;
	for ( Node* n = node; n != nullptr && n != ancestor; n = n->get_parent() )
	{
		if ( auto* spatial = Object::cast_to<Node3D>( n ) )
		{
			out = spatial->get_transform() * out;
		}
	}
	return out;
}

bool Shown( const Node* root, Node* node )
{
	for ( Node* n = node; n != nullptr; n = n->get_parent() )
	{
		if ( auto* spatial = Object::cast_to<Node3D>( n ); spatial != nullptr && spatial->is_visible() == false )
		{
			return false;
		}
		if ( n == root )
		{
			break;
		}
	}
	return true;
}

// Joint names are matched as they are, then through the humanoid profile Godot retargets imported
// characters onto (CinderboxSkeleton's matching).
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

// Our rig hangs its arms at rest; the humanoid profile's rest is a T-pose (CinderboxSkeleton's).
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
	float angle = left ? Math_PI * 0.5f : -Math_PI * 0.5f;
	return Quaternion( Vector3( 0, 0, 1 ), angle );
}

RenderingServer* RS()
{
	return RenderingServer::get_singleton();
}

// A unit box's 24 corners (4 a face, each with its face's normal) and 36 indices, into `out`.
void AddBox( const Transform3D& box, const Transform3D& toModel, int bone, const Color& color, PackedVector3Array& vertices,
			 PackedVector3Array& normals, PackedColorArray& colors, PackedInt32Array& bones, PackedFloat32Array& weights,
			 PackedInt32Array& indices )
{
	static const float kFaces[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	const Transform3D full = toModel * box;
	const Basis normalBasis = full.basis.inverse().transposed();
	for ( const auto& f : kFaces )
	{
		const Vector3 n( f[0], f[1], f[2] );
		// Two axes across the face, so the corners go round it.
		const Vector3 u = std::abs( n.x ) > 0.5f ? Vector3( 0, 1, 0 ) : Vector3( 1, 0, 0 );
		const Vector3 v = n.cross( u );
		const int base = int( vertices.size() );
		const Vector3 corners[4] = { n * 0.5f - u * 0.5f - v * 0.5f, n * 0.5f + u * 0.5f - v * 0.5f, n * 0.5f + u * 0.5f + v * 0.5f,
									 n * 0.5f - u * 0.5f + v * 0.5f };
		const Vector3 normal = normalBasis.xform( n ).normalized();
		for ( const Vector3& corner : corners )
		{
			vertices.push_back( full.xform( corner ) );
			normals.push_back( normal );
			colors.push_back( color );
			bones.push_back( bone );
			bones.push_back( 0 );
			bones.push_back( 0 );
			bones.push_back( 0 );
			weights.push_back( 1.0f );
			weights.push_back( 0.0f );
			weights.push_back( 0.0f );
			weights.push_back( 0.0f );
		}
		// (Facing out: Godot's front faces wind clockwise.)
		indices.push_back( base );
		indices.push_back( base + 2 );
		indices.push_back( base + 1 );
		indices.push_back( base );
		indices.push_back( base + 3 );
		indices.push_back( base + 2 );
	}
}

} // namespace

BoneStyle BoneStyleFor( const char* jointName, float length )
{
	const std::string n = jointName;
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

Transform3D BoneBoxTransform( const Vector3& a, const Vector3& b, const Vector3& refX, float width, float depth )
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
	return Transform3D( Basis( x * width, y * length, z * depth ), ( a + b ) * 0.5f );
}

// --- CharacterLook -----------------------------------------------------------------------------------

std::shared_ptr<CharacterLook> CharacterLook::FromScene( Node* root, const anim::AnimSet& set )
{
	if ( root == nullptr )
	{
		return nullptr;
	}
	auto look = std::make_shared<CharacterLook>();
	std::function<CinderboxSkeleton*( Node* )> findDriver = [&]( Node* n ) -> CinderboxSkeleton* {
		if ( auto* found = Object::cast_to<CinderboxSkeleton>( n ) )
		{
			return found;
		}
		for ( int i = 0; i < n->get_child_count(); ++i )
		{
			if ( CinderboxSkeleton* found = findDriver( n->get_child( i ) ) )
			{
				return found;
			}
		}
		return nullptr;
	};
	CinderboxSkeleton* driver = findDriver( root );
	Skeleton3D* target = nullptr;
	if ( driver != nullptr )
	{
		look->m_poseInRoot = RelativeTo( root, driver );
		look->m_useSlotColor = driver->get_use_slot_color();
		look->m_bodyColor = driver->get_body_color();
		look->m_retarget = driver->get_retarget();
		if ( driver->get_skeleton_path().is_empty() == false )
		{
			target = Object::cast_to<Skeleton3D>( driver->get_node_or_null( driver->get_skeleton_path() ) );
		}
	}
	if ( target != nullptr )
	{
		look->m_skeletonInRoot = RelativeTo( root, target );
		const int count = target->get_bone_count();
		look->m_bones.resize( size_t( count ) );
		for ( int b = 0; b < count; ++b )
		{
			Bone& bone = look->m_bones[size_t( b )];
			bone.name = target->get_bone_name( b );
			bone.parent = target->get_bone_parent( b );
			bone.rest = target->get_bone_rest( b );
			bone.pose = Transform3D( Basis( target->get_bone_pose_rotation( b ) ) * Basis::from_scale( target->get_bone_pose_scale( b ) ),
									 target->get_bone_pose_position( b ) );
			bone.global = target->get_bone_global_rest( b );
		}
		look->Bind( set );
		// (Bind matched names through the target; a second pass for what the scene has only by name.)
		look->m_jointToBone.assign( size_t( set.Skeleton().num_joints() ), -1 );
		auto names = set.Skeleton().joint_names();
		for ( int j = 0; j < set.Skeleton().num_joints(); ++j )
		{
			look->m_jointToBone[size_t( j )] = FindTargetBone( target, names[j] );
		}
		// The rest bridge, as CinderboxSkeleton::Bind makes it.
		look->m_restBridge.assign( size_t( set.Skeleton().num_joints() ), Quaternion() );
		const auto& restModels = set.RestModels();
		for ( int j = 0; j < set.Skeleton().num_joints(); ++j )
		{
			const int bone = look->m_jointToBone[size_t( j )];
			if ( bone < 0 )
			{
				continue;
			}
			Quaternion sourceRest = ToTransform( restModels[size_t( j )] ).basis.orthonormalized().get_rotation_quaternion();
			Quaternion targetRest = target->get_bone_global_rest( bone ).basis.orthonormalized().get_rotation_quaternion();
			look->m_restBridge[size_t( j )] = ( sourceRest * RestPosture( names[j] ) ).inverse() * targetRest;
			if ( std::strcmp( names[j], "Hips" ) == 0 || std::strcmp( names[j], "mixamorig:Hips" ) == 0 )
			{
				look->m_hips = bone;
				look->m_sourceHipsRest = ToTransform( restModels[size_t( j )] ).origin;
				look->m_targetHipsRest = target->get_bone_global_rest( bone ).origin;
				look->m_hipScale = look->m_sourceHipsRest.y > 0.001f ? float( look->m_targetHipsRest.y / look->m_sourceHipsRest.y ) : 1.0f;
			}
		}
	}
	else if ( driver != nullptr && driver->get_draw_bone_boxes() )
	{
		look->AddBoxes( set );
	}
	if ( driver != nullptr && target != nullptr && driver->get_draw_bone_boxes() )
	{
		look->m_notDrawn.push_back( "the bone boxes (the skeleton's meshes are drawn instead)" );
	}

	// The meshes, the effects, and what is not drawn.
	std::vector<std::pair<Node*, String>> effectNodes;
	std::function<void( Node* )> walk = [&]( Node* n ) {
		if ( n != root && Object::cast_to<CbSocket>( n ) != nullptr )
		{
			return; // (what sits in a socket in the editor is a preview)
		}
		if ( Object::cast_to<CbHitbox>( n ) != nullptr || Object::cast_to<CollisionShape3D>( n ) != nullptr )
		{
			return; // (the simulation's, baked)
		}
		const String path = String( root->get_path_to( n ) );
		if ( n != root && n->get_script().get_type() != Variant::NIL )
		{
			look->m_notDrawn.push_back( path + String( " (a script)" ) );
		}
		if ( auto* mi = Object::cast_to<MeshInstance3D>( n ) )
		{
			Ref<godot::Mesh> mesh = mi->get_mesh();
			if ( mesh.is_valid() && Shown( root, mi ) )
			{
				Mesh m;
				m.mesh = mesh;
				Ref<ArrayMesh> arrays = mesh;
				bool hasBones = arrays.is_null() && mi->get_skin().is_valid();
				for ( int s = 0; s < mesh->get_surface_count(); ++s )
				{
					hasBones |= arrays.is_valid() && ( uint64_t( arrays->surface_get_format( s ) ) & uint64_t( godot::Mesh::ARRAY_FORMAT_BONES ) ) != 0;
					m.surfaces.push_back( mi->get_surface_override_material( s ) );
				}
				m.materialOverride = mi->get_material_override();
				m.overlay = mi->get_material_overlay();
				m.layers = mi->get_layer_mask();
				m.castShadows = int( mi->get_cast_shadows_setting() );
				m.lodBias = float( mi->get_lod_bias() );
				Node* skinnedTo = mi->get_skeleton_path().is_empty() ? nullptr : mi->get_node_or_null( mi->get_skeleton_path() );
				if ( hasBones && target != nullptr && skinnedTo == target )
				{
					m.skinned = true;
					m.local = RelativeTo( target, mi );
					Ref<Skin> skin = mi->get_skin();
					if ( skin.is_valid() )
					{
						for ( int i = 0; i < skin->get_bind_count(); ++i )
						{
							const StringName name = skin->get_bind_name( i );
							m.binds.push_back( String( name ).is_empty() ? skin->get_bind_bone( i ) : target->find_bone( name ) );
							m.bindPoses.push_back( skin->get_bind_pose( i ) );
						}
					}
					else
					{
						// (As Godot makes one from the skeleton's rest.)
						for ( int b = 0; b < target->get_bone_count(); ++b )
						{
							m.binds.push_back( b );
							m.bindPoses.push_back( target->get_bone_global_rest( b ).affine_inverse() );
						}
					}
				}
				else
				{
					// On the bone of the nearest attachment above it, or in the root's frame.
					BoneAttachment3D* on = nullptr;
					for ( Node* p = mi->get_parent(); p != nullptr && p != root; p = p->get_parent() )
					{
						if ( ( on = Object::cast_to<BoneAttachment3D>( p ) ) != nullptr )
						{
							break;
						}
					}
					const int bone = on != nullptr && target != nullptr && on->get_parent() == target ? target->find_bone( on->get_bone_name() ) : -1;
					if ( bone >= 0 )
					{
						m.bone = bone;
						m.local = RelativeTo( on, mi );
					}
					else
					{
						m.bone = -1;
						m.local = RelativeTo( root, mi );
					}
				}
				look->m_meshes.push_back( m );
			}
		}
		else if ( Object::cast_to<GPUParticles3D>( n ) != nullptr || Object::cast_to<CPUParticles3D>( n ) != nullptr ||
				  Object::cast_to<Light3D>( n ) != nullptr || Object::cast_to<AudioStreamPlayer3D>( n ) != nullptr )
		{
			// An effect: a node of the entity's, at its bone (with what is under it), named uniquely.
			Effect effect;
			String name = n->get_name();
			for ( int k = 2; std::any_of( look->m_effects.begin(), look->m_effects.end(), [&]( const Effect& e ) { return e.name == name; } ); ++k )
			{
				name = String( n->get_name() ) + String::num_int64( k );
			}
			effect.name = name;
			BoneAttachment3D* on = nullptr;
			for ( Node* p = n->get_parent(); p != nullptr && p != root; p = p->get_parent() )
			{
				if ( ( on = Object::cast_to<BoneAttachment3D>( p ) ) != nullptr )
				{
					break;
				}
			}
			effect.bone = on != nullptr && target != nullptr && on->get_parent() == target ? target->find_bone( on->get_bone_name() ) : -1;
			effect.local = effect.bone >= 0 ? RelativeTo( on, n ) : RelativeTo( root, n );
			effect.source = n->duplicate();
			if ( auto* spatial = Object::cast_to<Node3D>( effect.source ) )
			{
				spatial->set_transform( Transform3D() );
			}
			look->m_effects.push_back( effect );
			effectNodes.emplace_back( n, name );
			return; // (what is under it goes with it)
		}
		else if ( Object::cast_to<VisualInstance3D>( n ) != nullptr )
		{
			look->m_notDrawn.push_back( path + String( " (" ) + n->get_class() + String( ")" ) );
		}
		else if ( Object::cast_to<SkeletonModifier3D>( n ) != nullptr && n->get_class() != String( "CbPoseModifier" ) )
		{
			look->m_notDrawn.push_back( path + String( " (a skeleton modifier: the pose alone places the bones)" ) );
		}
		for ( int i = 0; i < n->get_child_count(); ++i )
		{
			walk( n->get_child( i ) );
		}
	};
	walk( root );

	// The sockets: where held items go, as the game moves them to the entity itself.
	std::function<void( Node* )> sockets = [&]( Node* n ) {
		if ( auto* socket = Object::cast_to<CbSocket>( n ) )
		{
			Socket s;
			s.name = socket->get_name();
			s.bone = socket->get_bone();
			s.local = socket->get_transform();
			if ( auto* attachment = Object::cast_to<BoneAttachment3D>( socket->get_parent() ) )
			{
				if ( s.bone.is_empty() )
				{
					s.bone = attachment->get_bone_name();
				}
			}
			else
			{
				s.local = Transform3D(); // not under a bone: at the bone itself
			}
			look->m_sockets.push_back( s );
		}
		for ( int i = 0; i < n->get_child_count(); ++i )
		{
			sockets( n->get_child( i ) );
		}
	};
	sockets( root );
	// Every character has hands to hold things in, and a head to hang things on.
	for ( const char* hand : { "RightHand", "LeftHand", "Head" } )
	{
		bool have = false;
		for ( const Socket& s : look->m_sockets )
		{
			have |= s.name == String( hand );
		}
		if ( have )
		{
			continue;
		}
		Socket s;
		s.name = hand;
		s.bone = hand;
		const bool isHand = String( hand ) != String( "Head" );
		s.local = isHand ? CbItem::HandSocketFrame() : Transform3D();
		s.itemFrame = isHand;
		look->m_sockets.push_back( s );
	}
	if ( look->m_meshes.empty() )
	{
		look->m_notDrawn.push_back( "(no mesh: the body is not drawn)" );
	}
	look->TakeTracks( root, effectNodes );
	return look;
}

CharacterLook::~CharacterLook()
{
	for ( Effect& effect : m_effects )
	{
		if ( effect.source != nullptr )
		{
			memdelete( effect.source );
		}
	}
}

void CharacterLook::TakeTracks( Node* root, const std::vector<std::pair<Node*, String>>& effectNodes )
{
	auto* character = Object::cast_to<CbCharacter>( root );
	if ( character == nullptr )
	{
		return;
	}
	Ref<AnimationLibrary> library = character->build_track_library();
	auto* player = Object::cast_to<AnimationPlayer>(
		character->get_animation_player_path().is_empty() ? nullptr : character->get_node_or_null( character->get_animation_player_path() ) );
	if ( player == nullptr )
	{
		TypedArray<Node> players = character->find_children( "*", "AnimationPlayer", true, false );
		player = players.is_empty() ? nullptr : Object::cast_to<AnimationPlayer>( players[0] );
	}
	Node* animationRoot = player != nullptr ? player->get_node_or_null( player->get_root_node() ) : nullptr;
	if ( library.is_null() || animationRoot == nullptr )
	{
		return;
	}
	// Each track's path, from the entity now: an effect's node by its name, an item by its socket's
	// ("RightHand/Item/AnimationPlayer"). A track into anything else is not played (not drawn).
	TypedArray<Node> sockets = root->find_children( "*", "CbSocket", true, false );
	TypedArray<StringName> names = library->get_animation_list();
	for ( int64_t a = 0; a < names.size(); ++a )
	{
		Ref<Animation> animation = library->get_animation( names[a] );
		for ( int32_t t = animation->get_track_count() - 1; t >= 0; --t )
		{
			const String path = String( animation->track_get_path( t ) );
			const int colon = path.find( ":" );
			const String nodePart = colon >= 0 ? path.substr( 0, colon ) : path;
			const String property = colon >= 0 ? path.substr( colon ) : String();
			Node* target = animationRoot->get_node_or_null( NodePath( nodePart ) );
			String moved;
			for ( const auto& [node, name] : effectNodes )
			{
				if ( target != nullptr && ( target == node || node->is_ancestor_of( target ) ) )
				{
					moved = name + ( target == node ? String() : String( "/" ) + String( node->get_path_to( target ) ) ) + property;
				}
			}
			if ( moved.is_empty() )
			{
				const String full = String( root->get_path_to( animationRoot ) ) + String( "/" ) + nodePart;
				for ( int64_t k = 0; k < sockets.size(); ++k )
				{
					Node* socket = Object::cast_to<Node>( sockets[k] );
					const String at = String( root->get_path_to( socket ) );
					if ( full.begins_with( at + String( "/" ) ) || full.begins_with( String( "./" ) + at + String( "/" ) ) )
					{
						moved = String( socket->get_name() ) + full.substr( full.find( at ) + at.length() ) + property;
					}
				}
			}
			if ( moved.is_empty() )
			{
				m_notDrawn.push_back( String( "the track " ) + path + String( " of " ) + String( names[a] ) );
				animation->remove_track( t );
				continue;
			}
			animation->track_set_path( t, NodePath( moved ) );
		}
		if ( animation->get_track_count() == 0 )
		{
			library->remove_animation( names[a] );
		}
	}
	if ( library->get_animation_list().is_empty() == false )
	{
		m_tracks = library;
	}
}

void CharacterLook::Bind( const anim::AnimSet& set )
{
	// Parents before children, whatever order the bones are listed in.
	m_order.clear();
	std::vector<std::vector<int>> children( m_bones.size() );
	for ( size_t b = 0; b < m_bones.size(); ++b )
	{
		const int parent = m_bones[b].parent;
		if ( parent < 0 || size_t( parent ) >= m_bones.size() )
		{
			m_order.push_back( int( b ) );
		}
		else
		{
			children[size_t( parent )].push_back( int( b ) );
		}
	}
	for ( size_t i = 0; i < m_order.size(); ++i )
	{
		for ( int child : children[size_t( m_order[i] )] )
		{
			m_order.push_back( child );
		}
	}
	m_jointNames.clear();
	auto names = set.Skeleton().joint_names();
	for ( int j = 0; j < set.Skeleton().num_joints(); ++j )
	{
		m_jointNames.emplace_back( names[j] );
	}
}

void CharacterLook::AddBoxes( const anim::AnimSet& set )
{
	// The placeholder: the pose's own joints, driven exactly, and a box on each one with a parent,
	// as one skinned mesh (the boxes in their parent joint's frame; the nose on the head).
	const auto& skeleton = set.Skeleton();
	const int joints = skeleton.num_joints();
	auto parents = skeleton.joint_parents();
	auto names = skeleton.joint_names();
	const auto& rest = set.RestModels();
	m_retarget = false;
	m_skeletonInRoot = m_poseInRoot;
	m_bones.resize( size_t( joints ) );
	m_jointToBone.resize( size_t( joints ) );
	for ( int j = 0; j < joints; ++j )
	{
		Bone& bone = m_bones[size_t( j )];
		bone.name = names[j];
		bone.parent = parents[j];
		bone.global = ToTransform( rest[size_t( j )] );
		bone.rest = bone.parent >= 0 ? ToTransform( rest[size_t( bone.parent )] ).affine_inverse() * bone.global : bone.global;
		bone.pose = bone.rest;
		m_jointToBone[size_t( j )] = j;
	}
	Bind( set );

	PackedVector3Array vertices[2], normals[2];
	PackedColorArray colors[2];
	PackedInt32Array bones[2], indices[2];
	PackedFloat32Array weights[2];
	for ( int j = 0; j < joints; ++j )
	{
		const int p = parents[j];
		if ( p < 0 )
		{
			continue;
		}
		const Transform3D parentRest = ToTransform( rest[size_t( p )] );
		const Vector3 b = parentRest.affine_inverse().xform( ToTransform( rest[size_t( j )] ).origin );
		BoneStyle style = BoneStyleFor( names[j], b.length() );
		// (Body white, limbs a quarter darker: the material is the body's colour.)
		const Color shade = style.body ? Color( 1, 1, 1 ) : Color( 0.75f, 0.75f, 0.75f );
		AddBox( BoneBoxTransform( Vector3(), b, Vector3( 1, 0, 0 ), style.width, style.depth ), parentRest, p, shade, vertices[0], normals[0],
				colors[0], bones[0], weights[0], indices[0] );
		if ( std::string( names[j] ).find( "HeadTop" ) != std::string::npos )
		{
			const Vector3 a = parentRest.origin;
			const Vector3 top = ToTransform( rest[size_t( j )] ).origin;
			const Vector3 nose = a.lerp( top, 0.45f ) + Vector3( 0, 0, 0.14f );
			AddBox( Transform3D( Basis().scaled( Vector3( 0.06f, 0.06f, 0.06f ) ), nose ), Transform3D(), p, Color( 1, 1, 1 ), vertices[1],
					normals[1], colors[1], bones[1], weights[1], indices[1] );
		}
	}
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	for ( int s = 0; s < 2; ++s )
	{
		if ( vertices[s].is_empty() )
		{
			continue;
		}
		Array arrays;
		arrays.resize( godot::Mesh::ARRAY_MAX );
		arrays[godot::Mesh::ARRAY_VERTEX] = vertices[s];
		arrays[godot::Mesh::ARRAY_NORMAL] = normals[s];
		arrays[godot::Mesh::ARRAY_COLOR] = colors[s];
		arrays[godot::Mesh::ARRAY_BONES] = bones[s];
		arrays[godot::Mesh::ARRAY_WEIGHTS] = weights[s];
		arrays[godot::Mesh::ARRAY_INDEX] = indices[s];
		mesh->add_surface_from_arrays( godot::Mesh::PRIMITIVE_TRIANGLES, arrays );
	}
	Ref<StandardMaterial3D> nose;
	nose.instantiate();
	nose->set_albedo( Color( 0.25f, 0.25f, 0.25f ) );
	nose->set_roughness( 0.8f );
	if ( mesh->get_surface_count() > 1 )
	{
		mesh->surface_set_material( 1, nose );
	}
	Mesh m;
	m.mesh = mesh;
	m.skinned = true;
	m.tinted = true;
	for ( int j = 0; j < joints; ++j )
	{
		m.binds.push_back( j );
		m.bindPoses.push_back( m_bones[size_t( j )].global.affine_inverse() );
	}
	m.surfaces.resize( size_t( mesh->get_surface_count() ) );
	m_meshes.push_back( m );
}

int CharacterLook::BoneOf( const char* profileName ) const
{
	// (By the pose's joints, as CinderboxSkeleton::BoneOf: the bone each is driven onto.)
	for ( size_t j = 0; j < m_jointNames.size() && j < m_jointToBone.size(); ++j )
	{
		const char* profile = anim::ProfileName( m_jointNames[j].c_str() );
		if ( profile != nullptr && std::strcmp( profile, profileName ) == 0 )
		{
			return m_jointToBone[j];
		}
	}
	return -1;
}

void CharacterLook::Drive( const ozz::vector<ozz::math::Float4x4>& models, std::vector<Transform3D>& locals,
						   std::vector<Transform3D>& globals ) const
{
	const size_t bones = m_bones.size();
	locals.resize( bones );
	globals.resize( bones );
	// The joint that drives each bone (the last one, as a later joint's pose overwrote an earlier's).
	std::vector<int>& driver = m_driverScratch;
	driver.assign( bones, -1 );
	for ( size_t j = 0; j < m_jointToBone.size() && j < models.size(); ++j )
	{
		const int bone = m_jointToBone[j];
		if ( bone >= 0 && size_t( bone ) < bones )
		{
			driver[size_t( bone )] = int( j );
		}
	}
	if ( m_retarget == false )
	{
		// Every driven bone where the pose has it; the others follow their parent with their pose.
		for ( int bone : m_order )
		{
			const int parent = m_bones[size_t( bone )].parent;
			const Transform3D parentGlobal = parent >= 0 ? globals[size_t( parent )] : Transform3D();
			const int j = driver[size_t( bone )];
			if ( j >= 0 )
			{
				globals[size_t( bone )] = ToTransform( models[size_t( j )] );
				locals[size_t( bone )] = parentGlobal.affine_inverse() * globals[size_t( bone )];
			}
			else
			{
				locals[size_t( bone )] = m_bones[size_t( bone )].pose;
				globals[size_t( bone )] = parentGlobal * locals[size_t( bone )];
			}
		}
		return;
	}
	// Retargeted: rotation only, bone by bone, so the target keeps its own bone lengths; the hips
	// also move (CinderboxSkeleton::DriveSkeleton).
	std::vector<Quaternion>& global = m_rotationScratch;
	global.assign( bones, Quaternion() );
	for ( int bone : m_order )
	{
		const Bone& b = m_bones[size_t( bone )];
		const Quaternion parentRotation = b.parent >= 0 ? global[size_t( b.parent )] : Quaternion();
		const int j = driver[size_t( bone )];
		Quaternion local;
		if ( j >= 0 )
		{
			global[size_t( bone )] =
				ToTransform( models[size_t( j )] ).basis.orthonormalized().get_rotation_quaternion() * m_restBridge[size_t( j )];
			local = parentRotation.inverse() * global[size_t( bone )];
		}
		else
		{
			local = b.pose.basis.orthonormalized().get_rotation_quaternion();
			global[size_t( bone )] = parentRotation * local;
		}
		Vector3 position = b.pose.origin;
		if ( bone == m_hips && models.empty() == false )
		{
			position = b.rest.origin + ( ToTransform( models[0] ).origin - m_sourceHipsRest ) * m_hipScale;
		}
		locals[size_t( bone )] = Transform3D( Basis( local ) * Basis::from_scale( b.pose.basis.get_scale() ), position );
	}
	Globals( locals, globals );
}

void CharacterLook::Globals( const std::vector<Transform3D>& locals, std::vector<Transform3D>& globals ) const
{
	globals.resize( m_bones.size() );
	for ( int bone : m_order )
	{
		const int parent = m_bones[size_t( bone )].parent;
		globals[size_t( bone )] = parent >= 0 ? globals[size_t( parent )] * locals[size_t( bone )] : locals[size_t( bone )];
	}
}

// --- DrawnBody ---------------------------------------------------------------------------------------

DrawnBody::DrawnBody( std::shared_ptr<const CharacterLook> look, const RID& scenario ) : m_look( std::move( look ) ), m_scenario( scenario )
{
	for ( const CharacterLook::Mesh& mesh : m_look->Meshes() )
	{
		if ( mesh.tinted && m_tintMaterial.is_null() )
		{
			m_tintMaterial.instantiate();
			m_tintMaterial->set_flag( BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true );
			m_tintMaterial->set_roughness( 0.8f );
			m_tintMaterial->set_albedo( m_look->BodyColor() );
		}
	}
	Make( m_body, 0, true );
	// Its pose until it is posed: as the scene has it.
	m_body.locals.resize( m_look->Bones().size() );
	for ( size_t b = 0; b < m_look->Bones().size(); ++b )
	{
		m_body.locals[b] = m_look->Bones()[b].pose;
	}
	m_look->Globals( m_body.locals, m_body.globals );
	PushBones( m_body );
}

DrawnBody::~DrawnBody()
{
	Free( m_body );
	Free( m_legs );
	Free( m_arms );
}

void DrawnBody::Make( Copy& copy, uint32_t layers, bool castShadows )
{
	RenderingServer* rs = RS();
	if ( copy.made || rs == nullptr )
	{
		return;
	}
	const auto& meshes = m_look->Meshes();
	copy.skeletons.assign( meshes.size(), RID() );
	copy.instances.assign( meshes.size(), RID() );
	for ( size_t i = 0; i < meshes.size(); ++i )
	{
		const CharacterLook::Mesh& mesh = meshes[i];
		RID instance = rs->instance_create2( mesh.mesh->get_rid(), m_scenario );
		rs->instance_set_visible( instance, false );
		if ( mesh.skinned )
		{
			RID skeleton = rs->skeleton_create();
			rs->skeleton_allocate_data( skeleton, int32_t( mesh.binds.size() ) );
			rs->instance_attach_skeleton( instance, skeleton );
			copy.skeletons[i] = skeleton;
		}
		rs->instance_set_layer_mask( instance, layers != 0 ? layers : mesh.layers );
		rs->instance_geometry_set_cast_shadows_setting(
			instance, castShadows ? RenderingServer::ShadowCastingSetting( mesh.castShadows ) : RenderingServer::SHADOW_CASTING_SETTING_OFF );
		for ( size_t s = 0; s < mesh.surfaces.size(); ++s )
		{
			if ( mesh.tinted && s == 0 && m_tintMaterial.is_valid() )
			{
				rs->instance_set_surface_override_material( instance, int32_t( s ), m_tintMaterial->get_rid() );
			}
			else if ( mesh.surfaces[s].is_valid() )
			{
				rs->instance_set_surface_override_material( instance, int32_t( s ), mesh.surfaces[s]->get_rid() );
			}
		}
		if ( mesh.materialOverride.is_valid() )
		{
			rs->instance_geometry_set_material_override( instance, mesh.materialOverride->get_rid() );
		}
		if ( mesh.overlay.is_valid() )
		{
			rs->instance_geometry_set_material_overlay( instance, mesh.overlay->get_rid() );
		}
		if ( mesh.lodBias != 1.0f )
		{
			rs->instance_geometry_set_lod_bias( instance, mesh.lodBias );
		}
		copy.instances[i] = instance;
	}
	copy.made = true;
	copy.shown = false;
}

void DrawnBody::Free( Copy& copy )
{
	RenderingServer* rs = RS();
	if ( copy.made == false || rs == nullptr )
	{
		copy.made = false;
		return;
	}
	for ( const RID& instance : copy.instances )
	{
		if ( instance.is_valid() )
		{
			rs->free_rid( instance );
		}
	}
	for ( const RID& skeleton : copy.skeletons )
	{
		if ( skeleton.is_valid() )
		{
			rs->free_rid( skeleton );
		}
	}
	copy.instances.clear();
	copy.skeletons.clear();
	copy.made = false;
	copy.shown = false;
}

void DrawnBody::Show( Copy& copy, bool shown )
{
	if ( copy.made == false || copy.shown == shown )
	{
		return;
	}
	copy.shown = shown;
	for ( const RID& instance : copy.instances )
	{
		RS()->instance_set_visible( instance, shown );
	}
}

void DrawnBody::PushBones( Copy& copy )
{
	if ( copy.made == false || copy.globals.size() != m_look->Bones().size() )
	{
		return;
	}
	RenderingServer* rs = RS();
	const auto& meshes = m_look->Meshes();
	// (Bounds from where the bones are: a skinned mesh's own would be worked out bone by bone.)
	AABB bounds;
	bool first = true;
	for ( const Transform3D& global : copy.globals )
	{
		if ( first )
		{
			bounds = AABB( global.origin, Vector3() );
			first = false;
		}
		else
		{
			bounds.expand_to( global.origin );
		}
	}
	bounds = bounds.grow( 0.35f );
	for ( size_t i = 0; i < meshes.size(); ++i )
	{
		const CharacterLook::Mesh& mesh = meshes[i];
		if ( mesh.skinned == false )
		{
			continue;
		}
		const RID& skeleton = copy.skeletons[i];
		for ( size_t k = 0; k < mesh.binds.size(); ++k )
		{
			const int bone = mesh.binds[k];
			if ( bone >= 0 && size_t( bone ) < copy.globals.size() )
			{
				rs->skeleton_bone_set_transform( skeleton, int32_t( k ), copy.globals[size_t( bone )] * mesh.bindPoses[k] );
			}
		}
		if ( first == false )
		{
			rs->instance_set_custom_aabb( copy.instances[i], mesh.local.affine_inverse().xform( bounds ) );
		}
	}
}

void DrawnBody::PushPlaces( Copy& copy )
{
	if ( copy.made == false )
	{
		return;
	}
	RenderingServer* rs = RS();
	const auto& meshes = m_look->Meshes();
	const Transform3D root = copy.frame * m_look->SkeletonInRoot().affine_inverse();
	for ( size_t i = 0; i < meshes.size(); ++i )
	{
		const CharacterLook::Mesh& mesh = meshes[i];
		Transform3D at;
		if ( mesh.skinned )
		{
			at = copy.frame * mesh.local;
		}
		else if ( mesh.bone >= 0 && size_t( mesh.bone ) < copy.globals.size() )
		{
			at = copy.frame * copy.globals[size_t( mesh.bone )] * mesh.local;
		}
		else
		{
			at = root * mesh.local;
		}
		rs->instance_set_transform( copy.instances[i], at );
	}
}

void DrawnBody::Place( const Transform3D& world )
{
	if ( m_placed && world.is_equal_approx( m_world ) )
	{
		return;
	}
	m_world = world;
	m_placed = true;
	m_body.frame = world * m_look->SkeletonInRoot();
	PushPlaces( m_body );
	Show( m_body, m_visible );
}

void DrawnBody::SetVisible( bool visible )
{
	m_visible = visible;
	Show( m_body, visible && m_placed );
	if ( visible == false )
	{
		Show( m_legs, false );
		Show( m_arms, false );
	}
}

namespace
{
RenderingServer::ShadowCastingSetting CastFor( const CharacterLook::Mesh& mesh, bool shadows, bool firstPerson )
{
	if ( firstPerson )
	{
		return RenderingServer::SHADOW_CASTING_SETTING_SHADOWS_ONLY;
	}
	return shadows ? RenderingServer::ShadowCastingSetting( mesh.castShadows ) : RenderingServer::SHADOW_CASTING_SETTING_OFF;
}
} // namespace

void DrawnBody::SetShadows( bool on )
{
	if ( on == m_shadows )
	{
		return;
	}
	m_shadows = on;
	if ( m_body.made == false )
	{
		return;
	}
	const auto& meshes = m_look->Meshes();
	for ( size_t i = 0; i < meshes.size(); ++i )
	{
		RS()->instance_geometry_set_cast_shadows_setting( m_body.instances[i], CastFor( meshes[i], m_shadows, m_firstPerson ) );
	}
}

void DrawnBody::SetBodyColor( const Color& color )
{
	if ( m_tintMaterial.is_valid() )
	{
		m_tintMaterial->set_albedo( color );
	}
}

void DrawnBody::ApplyPose( const anim::AnimSet& set, const ozz::vector<ozz::math::Float4x4>& models )
{
	m_set = &set;
	m_models = models;
	m_look->Drive( models, m_body.locals, m_body.globals );
	PushBones( m_body );
	PushPlaces( m_body ); // (what rides on a bone)
	UpdateViewLegs();
	UpdateViewArms();
}

bool DrawnBody::JointTransform( const String& profileName, Transform3D& out, bool forItems, bool view ) const
{
	if ( m_set == nullptr )
	{
		return false;
	}
	const auto& models = view && m_viewModels.size() == m_models.size() ? m_viewModels : m_models;
	CharString name = profileName.utf8();
	auto names = m_set->Skeleton().joint_names();
	for ( size_t j = 0; j < names.size() && j < models.size(); ++j )
	{
		const char* profile = anim::ProfileName( names[j] );
		if ( profile != nullptr && std::strcmp( profile, name.get_data() ) == 0 )
		{
			out = ToTransform( models[j] );
			if ( forItems )
			{
				out = out * ToTransform( m_set->AttachFrame( int( j ) ) );
			}
			// (The view's hands are moved by its offset; the fingers go with them.)
			if ( &models == &m_viewModels && std::strstr( profile, "Arm" ) == nullptr && std::strstr( profile, "Shoulder" ) == nullptr &&
				 ( std::strncmp( profile, "Left", 4 ) == 0 || std::strncmp( profile, "Right", 5 ) == 0 ) &&
				 std::strstr( profile, "Leg" ) == nullptr && std::strstr( profile, "Foot" ) == nullptr && std::strstr( profile, "Toes" ) == nullptr &&
				 std::strstr( profile, "Eye" ) == nullptr )
			{
				out = m_viewMove * out;
				out.origin += m_viewShort[std::strncmp( profile, "Left", 4 ) == 0 ? 0 : 1];
			}
			out = out.orthonormalized();
			return true;
		}
	}
	return false;
}

void DrawnBody::SetFirstPerson( bool on )
{
	if ( on == m_firstPerson )
	{
		return;
	}
	m_firstPerson = on;
	if ( m_body.made )
	{
		const auto& meshes = m_look->Meshes();
		for ( size_t i = 0; i < meshes.size(); ++i )
		{
			RS()->instance_geometry_set_cast_shadows_setting( m_body.instances[i], CastFor( meshes[i], m_shadows, m_firstPerson ) );
		}
	}
	if ( on == false )
	{
		Free( m_legs );
		Free( m_arms );
		m_viewModels.clear();
		m_viewArms = false;
		m_viewHandShown[0] = m_viewHandShown[1] = true;
	}
}

void DrawnBody::SetViewPose( const ozz::vector<ozz::math::Float4x4>* models )
{
	if ( models != nullptr )
	{
		m_viewModels = *models;
	}
	else
	{
		m_viewModels.clear();
	}
}

void DrawnBody::SetGlobal( Copy& copy, int bone, const Transform3D& global )
{
	const int parent = m_look->Bones()[size_t( bone )].parent;
	const Transform3D parentGlobal = parent >= 0 ? copy.globals[size_t( parent )] : Transform3D();
	copy.locals[size_t( bone )] = parentGlobal.affine_inverse() * global;
	m_look->Globals( copy.locals, copy.globals );
}

// The legs the viewer sees: a copy of the body in the body's own pose, everything above the waist
// shrunk to a point at it. PlaceViewLegs moves it once the camera is placed.
void DrawnBody::UpdateViewLegs()
{
	const int spine = m_look->BoneOf( "Spine" );
	if ( m_firstPerson == false || m_models.empty() || spine < 0 )
	{
		Free( m_legs );
		return;
	}
	if ( m_legs.made == false )
	{
		// (In the world's picture, among the world's things; it casts nothing: the body's is the shadow.)
		const auto& meshes = m_look->Meshes();
		Make( m_legs, meshes.empty() ? 1u : meshes[0].layers, false );
	}
	m_look->Drive( m_models, m_legs.locals, m_legs.globals );
	m_legs.locals[size_t( spine )].basis = m_legs.locals[size_t( spine )].basis.orthonormalized() * Basis::from_scale( Vector3( 0.0001f, 0.0001f, 0.0001f ) );
	m_look->Globals( m_legs.locals, m_legs.globals );
	m_legs.frame = m_body.frame;
	m_legs.frame.origin += m_legsShift;
	PushBones( m_legs );
	PushPlaces( m_legs );
	Show( m_legs, m_visible );
}

void DrawnBody::PlaceViewLegs( const Vector3& shift )
{
	m_legsShift = shift;
	if ( m_legs.made == false )
	{
		return;
	}
	m_legs.frame = m_body.frame;
	m_legs.frame.origin += shift;
	PushPlaces( m_legs );
}

// The view's arms: a copy of the body on the view model's layer, in the view's pose, of which only
// the arms are left. Bent onto an item instead (SetViewArms), it is posed by PlaceViewArms.
void DrawnBody::UpdateViewArms()
{
	const bool want = m_firstPerson && m_viewModels.empty() == false && m_viewModels.size() == m_models.size();
	if ( want == false )
	{
		Free( m_arms );
		return;
	}
	if ( m_arms.made == false )
	{
		Make( m_arms, kViewLayer, false );
	}
	m_look->Drive( m_viewModels, m_arms.locals, m_arms.globals );
	const char* hands[2] = { "LeftHand", "RightHand" };
	m_viewShort[0] = m_viewShort[1] = Vector3();
	// The skeleton's frame from the pose's.
	const Transform3D toRig = m_look->SkeletonInRoot().affine_inverse() * m_look->PoseInRoot();
	if ( m_viewMove.is_equal_approx( Transform3D() ) == false && m_viewArms == false )
	{
		// The hands where the item's settings put them: each arm bent again from its shoulder, its
		// elbow to the side the animation has it.
		const Transform3D move = toRig * m_viewMove * toRig.affine_inverse();
		const char* uppers[2] = { "LeftUpperArm", "RightUpperArm" };
		const char* lowers[2] = { "LeftLowerArm", "RightLowerArm" };
		for ( int side = 0; side < 2; ++side )
		{
			const int upper = m_look->BoneOf( uppers[side] ), lower = m_look->BoneOf( lowers[side] ), hand = m_look->BoneOf( hands[side] );
			if ( upper < 0 || lower < 0 || hand < 0 )
			{
				continue;
			}
			const Transform3D to = move * m_arms.globals[size_t( hand )];
			BendArm( m_arms, upper, lower, hand, to, m_arms.globals[size_t( lower )].origin - m_arms.globals[size_t( upper )].origin );
			// (What the hand holds stays in it where the arm is too short.)
			m_viewShort[side] = toRig.affine_inverse().basis.xform( m_arms.globals[size_t( hand )].origin - to.origin );
		}
	}
	for ( int side = 0; side < 2; ++side )
	{
		const int bone = m_look->BoneOf( hands[side] );
		m_viewHands[side] = bone >= 0 ? m_body.frame * m_arms.globals[size_t( bone )] : m_body.frame;
	}
	if ( m_viewArms )
	{
		return; // (posed and placed by PlaceViewArms, once the camera is)
	}
	m_arms.frame = m_body.frame;
	KeepArms( m_arms, m_viewHandShown[0], m_viewHandShown[1] );
	PushBones( m_arms );
	PushPlaces( m_arms );
	Show( m_arms, m_visible && ( m_viewHandShown[0] || m_viewHandShown[1] ) );
}

// Only the arms of `rig` are left to draw: everything else shrinks to a point, and the shoulders of
// the arms that are kept stay as the pose has them.
void DrawnBody::KeepArms( Copy& rig, bool left, bool right )
{
	const int hips = m_look->BoneOf( "Hips" );
	const int shoulders[2] = { m_look->BoneOf( "LeftShoulder" ), m_look->BoneOf( "RightShoulder" ) };
	if ( hips < 0 || shoulders[0] < 0 || shoulders[1] < 0 )
	{
		return;
	}
	const Basis nothing = Basis::from_scale( Vector3( 0.0001f, 0.0001f, 0.0001f ) );
	const Transform3D kept[2] = { rig.globals[size_t( shoulders[0] )], rig.globals[size_t( shoulders[1] )] };
	// All of it shrinks to one point, between the shoulders (see CinderboxSkeleton::KeepArms).
	const Vector3 between = ( kept[0].origin + kept[1].origin ) * 0.5f;
	SetGlobal( rig, hips, Transform3D( nothing, between ) );
	for ( const char* name : { "Spine", "Chest", "UpperChest" } )
	{
		if ( const int bone = m_look->BoneOf( name ); bone >= 0 )
		{
			SetGlobal( rig, bone, Transform3D( nothing, between ) );
		}
	}
	for ( int side = 0; side < 2; ++side )
	{
		const bool shown = side == 0 ? left : right;
		SetGlobal( rig, shoulders[side], shown ? kept[side] : Transform3D( nothing, between ) );
	}
}

void DrawnBody::PlaceViewRig( const Transform3D& stitch )
{
	if ( m_arms.made == false || m_viewArms )
	{
		return;
	}
	m_arms.frame = stitch * m_body.frame;
	PushPlaces( m_arms );
}

// Bends an arm of `rig` so that its hand is at `hand` (the skeleton's frame), the elbow toward
// `pole`: the upper arm and the forearm keep their lengths, and a hand out of reach is reached for.
void DrawnBody::BendArm( Copy& rig, int upper, int lower, int end, const Transform3D& hand, const Vector3& pole )
{
	Transform3D u = rig.globals[size_t( upper )];
	const Vector3 from = u.origin;
	const float first = ( rig.globals[size_t( lower )].origin - from ).length();
	const float second = ( rig.globals[size_t( end )].origin - rig.globals[size_t( lower )].origin ).length();
	const Vector3 to = hand.origin - from;
	if ( first < 1e-5f || second < 1e-5f || to.length() < 1e-5f )
	{
		return;
	}
	const float reach = std::clamp( float( to.length() ), std::abs( first - second ) + 1e-4f, ( first + second ) * 0.999f );
	const Vector3 along = to.normalized();
	const float ahead = ( first * first - second * second + reach * reach ) / ( 2.0f * reach );
	const float out = std::sqrt( std::max( 0.0f, first * first - ahead * ahead ) );
	Vector3 side = pole - along * pole.dot( along );
	side = side.length() > 1e-4f ? side.normalized() : Vector3( 0, -1, 0 );
	const Vector3 elbow = from + along * ahead + side * out;

	Vector3 was = ( rig.globals[size_t( lower )].origin - from ).normalized();
	u.basis = Basis( Quaternion( was, ( elbow - from ).normalized() ) ) * u.basis;
	SetGlobal( rig, upper, u );
	Transform3D l = rig.globals[size_t( lower )];
	was = ( rig.globals[size_t( end )].origin - l.origin ).normalized();
	l.basis = Basis( Quaternion( was, ( from + along * reach - l.origin ).normalized() ) ) * l.basis;
	SetGlobal( rig, lower, l );
	Transform3D h = rig.globals[size_t( end )];
	h.basis = hand.basis.orthonormalized();
	SetGlobal( rig, end, h );
}

void DrawnBody::PlaceViewArms( const Transform3D& camera, const Transform3D& left, const Transform3D& right, bool holdsLeft, bool holdsRight )
{
	if ( m_arms.made == false )
	{
		return;
	}
	const CharacterLook& look = *m_look;
	const int chest = look.BoneOf( "UpperChest" );
	const int head = look.BoneOf( "Head" );
	const int hips = look.BoneOf( "Hips" );
	const int shoulders[2] = { look.BoneOf( "LeftShoulder" ), look.BoneOf( "RightShoulder" ) };
	const int uppers[2] = { look.BoneOf( "LeftUpperArm" ), look.BoneOf( "RightUpperArm" ) };
	const int lowers[2] = { look.BoneOf( "LeftLowerArm" ), look.BoneOf( "RightLowerArm" ) };
	const int hands[2] = { look.BoneOf( "LeftHand" ), look.BoneOf( "RightHand" ) };
	if ( hips < 0 || chest < 0 || head < 0 || shoulders[0] < 0 || shoulders[1] < 0 || uppers[0] < 0 || uppers[1] < 0 || lowers[0] < 0 ||
		 lowers[1] < 0 || hands[0] < 0 || hands[1] < 0 )
	{
		Show( m_arms, false );
		return;
	}
	// (An arm that holds nothing is not in the view, nor one the item does not show.)
	holdsLeft = holdsLeft && m_viewHandShown[0];
	holdsRight = holdsRight && m_viewHandShown[1];
	const auto& bones = look.Bones();
	const Transform3D kept[2] = { m_arms.globals[size_t( shoulders[0] )], m_arms.globals[size_t( shoulders[1] )] };
	const Basis chestTurn =
		m_arms.globals[size_t( chest )].basis.orthonormalized() * bones[size_t( chest )].global.basis.orthonormalized().inverse();
	KeepArms( m_arms, holdsLeft, holdsRight );

	// The chest (the point between the shoulders, turned as the pose turned it from its rest) goes
	// where the rest pose has it under the head, facing where the camera looks.
	const Vector3 scale = m_body.frame.basis.get_scale();
	const Transform3D piece( chestTurn, ( kept[0].origin + kept[1].origin ) * 0.5f );
	Vector3 atRest = ( bones[size_t( shoulders[0] )].global.origin + bones[size_t( shoulders[1] )].global.origin ) * 0.5f -
					 Vector3( 0.0f, bones[size_t( head )].global.origin.y, 0.0f );
	atRest *= scale;
	const Transform3D under( Basis( Vector3( 0, 1, 0 ), float( Math_PI ) ), Vector3( -atRest.x, atRest.y, -atRest.z ) );
	Transform3D world = camera * under * Transform3D( Basis().scaled( scale ), Vector3() ) * piece.affine_inverse();

	// A hand the carrying arm cannot reach: the shoulders go after it (they are out of view).
	const Transform3D* wanted[2] = { &left, &right };
	const bool holds[2] = { holdsLeft, holdsRight };
	for ( int side = 1; side >= 0; --side )
	{
		if ( holds[side] == false )
		{
			continue;
		}
		const Vector3 from = world.xform( m_arms.globals[size_t( uppers[side] )].origin );
		const float reach = float( ( ( m_arms.globals[size_t( lowers[side] )].origin - m_arms.globals[size_t( uppers[side] )].origin ).length() +
									 ( m_arms.globals[size_t( hands[side] )].origin - m_arms.globals[size_t( lowers[side] )].origin ).length() ) *
								   scale.y * 0.97f );
		const Vector3 to = wanted[side]->origin - from;
		if ( to.length() > reach )
		{
			world.origin += to - to.normalized() * reach;
		}
		break; // by the carrying hand alone; the other reaches as far as it can
	}
	m_arms.frame = world;
	const Transform3D toRig = world.affine_inverse();
	for ( int side = 0; side < 2; ++side )
	{
		if ( holds[side] == false )
		{
			continue;
		}
		const Transform3D hand = toRig * *wanted[side];
		// The elbows down and out to their sides.
		const Vector3 pole = toRig.basis.xform( camera.basis.xform( Vector3( side == 1 ? 0.55f : -0.55f, -0.8f, 0.15f ) ) ).normalized();
		BendArm( m_arms, uppers[side], lowers[side], hands[side], hand, pole );
	}
	PushBones( m_arms );
	PushPlaces( m_arms );
	Show( m_arms, m_visible );
}

// --- ItemLook, DrawnItem ------------------------------------------------------------------------------

namespace
{
// Whether a rules sheet has anything the viewer runs: a reaction (on, while) or a prediction. A
// motion is the simulation's.
bool RulesDoSomething( const String& path )
{
	const String text = FileAccess::get_file_as_string( path );
	const PackedStringArray lines = text.split( "\n" );
	for ( const String& line : lines )
	{
		if ( line.begins_with( "on " ) || line.begins_with( "while " ) || line.begins_with( "predict " ) )
		{
			return true;
		}
	}
	return false;
}
} // namespace

std::shared_ptr<ItemLook> ItemLook::FromScene( Node* root )
{
	if ( root == nullptr )
	{
		return nullptr;
	}
	auto look = std::make_shared<ItemLook>();
	look->m_inCarriedFrame = CbItem::CarryFrameUnder( root ).affine_inverse();
	std::function<void( Node* )> walk = [&]( Node* n ) {
		const String path = String( root->get_path_to( n ) );
		if ( n != root && n->get_script().get_type() != Variant::NIL && look->m_why.is_empty() )
		{
			look->m_why = path + String( " has a script" );
		}
		if ( auto* mi = Object::cast_to<MeshInstance3D>( n ) )
		{
			Ref<godot::Mesh> mesh = mi->get_mesh();
			if ( mesh.is_valid() && Shown( root, mi ) )
			{
				Part part;
				part.mesh = mesh;
				part.local = RelativeTo( root, mi );
				for ( int s = 0; s < mesh->get_surface_count(); ++s )
				{
					part.surfaces.push_back( mi->get_surface_override_material( s ) );
				}
				part.materialOverride = mi->get_material_override();
				part.overlay = mi->get_material_overlay();
				part.layers = mi->get_layer_mask();
				part.castShadows = int( mi->get_cast_shadows_setting() );
				part.lodBias = float( mi->get_lod_bias() );
				look->m_parts.push_back( part );
			}
		}
		else if ( auto* rules = Object::cast_to<CbRules>( n ) )
		{
			if ( RulesDoSomething( rules->get_file() ) && look->m_why.is_empty() )
			{
				look->m_why = path + String( " reacts or predicts" );
			}
		}
		else if ( n == root || Object::cast_to<CollisionShape3D>( n ) != nullptr || Object::cast_to<Area3D>( n ) != nullptr ||
				  Object::cast_to<Marker3D>( n ) != nullptr || Object::cast_to<CbMotionPart>( n ) != nullptr ||
				  Object::cast_to<CbCurve>( n ) != nullptr || n->get_class() == String( "Node3D" ) || n->get_class() == String( "Node" ) )
		{
			// (The simulation's: its body, its shapes, its motions; or places and groups.)
		}
		else if ( look->m_why.is_empty() )
		{
			look->m_why = path + String( " is a " ) + n->get_class();
		}
		for ( int i = 0; i < n->get_child_count(); ++i )
		{
			walk( n->get_child( i ) );
		}
	};
	walk( root );
	return look;
}

DrawnItem::DrawnItem( std::shared_ptr<const ItemLook> look, const RID& scenario ) : m_look( std::move( look ) )
{
	RenderingServer* rs = RS();
	for ( const ItemLook::Part& part : m_look->Parts() )
	{
		RID instance = rs->instance_create2( part.mesh->get_rid(), scenario );
		rs->instance_set_visible( instance, false );
		rs->instance_set_layer_mask( instance, part.layers );
		rs->instance_geometry_set_cast_shadows_setting( instance, RenderingServer::ShadowCastingSetting( part.castShadows ) );
		for ( size_t s = 0; s < part.surfaces.size(); ++s )
		{
			if ( part.surfaces[s].is_valid() )
			{
				rs->instance_set_surface_override_material( instance, int32_t( s ), part.surfaces[s]->get_rid() );
			}
		}
		if ( part.materialOverride.is_valid() )
		{
			rs->instance_geometry_set_material_override( instance, part.materialOverride->get_rid() );
		}
		if ( part.overlay.is_valid() )
		{
			rs->instance_geometry_set_material_overlay( instance, part.overlay->get_rid() );
		}
		if ( part.lodBias != 1.0f )
		{
			rs->instance_geometry_set_lod_bias( instance, part.lodBias );
		}
		m_instances.push_back( instance );
	}
}

DrawnItem::~DrawnItem()
{
	if ( RenderingServer* rs = RS() )
	{
		for ( const RID& instance : m_instances )
		{
			rs->free_rid( instance );
		}
	}
}

void DrawnItem::Place( const Transform3D& scene )
{
	if ( m_placed && scene.is_equal_approx( m_at ) )
	{
		return;
	}
	m_at = scene;
	m_placed = true;
	const auto& parts = m_look->Parts();
	for ( size_t i = 0; i < parts.size(); ++i )
	{
		RS()->instance_set_transform( m_instances[i], scene * parts[i].local );
	}
}

void DrawnItem::SetVisible( bool visible )
{
	if ( visible == m_visible )
	{
		return;
	}
	m_visible = visible;
	for ( const RID& instance : m_instances )
	{
		RS()->instance_set_visible( instance, visible );
	}
}

void DrawnItem::SetShadows( bool on )
{
	if ( on == m_shadows )
	{
		return;
	}
	m_shadows = on;
	const auto& parts = m_look->Parts();
	for ( size_t i = 0; i < parts.size(); ++i )
	{
		RS()->instance_geometry_set_cast_shadows_setting(
			m_instances[i], on ? RenderingServer::ShadowCastingSetting( parts[i].castShadows ) : RenderingServer::SHADOW_CASTING_SETTING_OFF );
	}
}

} // namespace cb::gd
