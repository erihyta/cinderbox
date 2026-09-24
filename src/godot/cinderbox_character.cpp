#include "cinderbox_character.h"

#include "cinderbox_skeleton.h"
#include "pose.h" // CinderboxSkeleton holds a PoseEvaluator

#include "anim_set.h"
#include "hitboxes.h"

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/io/archive.h"
#include "ozz/base/io/stream.h"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/animation_tree.hpp>
#include <godot_cpp/classes/bone_attachment3d.hpp>
#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/capsule_shape3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/animation_library.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/skeleton_modifier3d.hpp>
#include <godot_cpp/classes/sphere_shape3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace godot;

namespace cb::gd
{

namespace
{

std::string Std( const String& s )
{
	return std::string( s.utf8().get_data() );
}

ozz::math::Transform ToOzz( const Transform3D& t )
{
	ozz::math::Transform out;
	Basis basis = t.basis;
	Vector3 scale = basis.get_scale();
	Quaternion q = basis.orthonormalized().get_rotation_quaternion();
	out.translation = ozz::math::Float3( float( t.origin.x ), float( t.origin.y ), float( t.origin.z ) );
	out.rotation = ozz::math::Quaternion( float( q.x ), float( q.y ), float( q.z ), float( q.w ) );
	out.scale = ozz::math::Float3( float( scale.x ), float( scale.y ), float( scale.z ) );
	return out;
}

template <typename T>
PackedByteArray Archive( const T& object )
{
	ozz::io::MemoryStream stream;
	{
		ozz::io::OArchive archive( &stream );
		archive << object;
	}
	PackedByteArray bytes;
	bytes.resize( int64_t( stream.Size() ) );
	stream.Seek( 0, ozz::io::Stream::kSet );
	stream.Read( bytes.ptrw(), size_t( bytes.size() ) );
	return bytes;
}

bool WriteFile( const String& path, const PackedByteArray& bytes, String& error )
{
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::WRITE );
	if ( file.is_null() )
	{
		error = "cannot write " + path;
		return false;
	}
	file->store_buffer( bytes );
	return true;
}

bool WriteText( const String& path, const std::string& text, String& error )
{
	PackedByteArray bytes;
	bytes.resize( int64_t( text.size() ) );
	for ( size_t i = 0; i < text.size(); ++i )
	{
		bytes[int64_t( i )] = uint8_t( text[i] );
	}
	return WriteFile( path, bytes, error );
}

bool IsIdentity( const Transform3D& t )
{
	return t.origin.length() < 1e-4 && ( t.basis.get_column( 0 ) - Vector3( 1, 0, 0 ) ).length() < 1e-4 &&
		   ( t.basis.get_column( 1 ) - Vector3( 0, 1, 0 ) ).length() < 1e-4 &&
		   ( t.basis.get_column( 2 ) - Vector3( 0, 0, 1 ) ).length() < 1e-4;
}

// The transform of `node` relative to `ancestor`, through the tree (no need for either to be in a
// scene tree, unlike global transforms).
Transform3D RelativeTo( Node* node, Node* ancestor )
{
	Transform3D t;
	for ( Node* n = node; n != nullptr && n != ancestor; n = n->get_parent() )
	{
		if ( Node3D* n3 = Object::cast_to<Node3D>( n ) )
		{
			t = n3->get_transform() * t;
		}
	}
	return t;
}

void Collect( Node* node, std::vector<CbHitbox*>& out )
{
	if ( CbHitbox* h = Object::cast_to<CbHitbox>( node ) )
	{
		out.push_back( h );
	}
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		Collect( node->get_child( i ), out );
	}
}

template <typename T>
T* FindFirst( Node* node )
{
	if ( T* t = Object::cast_to<T>( node ) )
	{
		return t;
	}
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		if ( T* t = FindFirst<T>( node->get_child( i ) ) )
		{
			return t;
		}
	}
	return nullptr;
}

} // namespace

// --- CbHitbox ---------------------------------------------------------------------------------------

void CbHitbox::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_zone", "zone" ), &CbHitbox::set_zone );
	ClassDB::bind_method( D_METHOD( "get_zone" ), &CbHitbox::get_zone );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "zone", PROPERTY_HINT_PLACEHOLDER_TEXT, "head, torso, arm, leg" ), "set_zone",
				  "get_zone" );
}

PackedStringArray CbHitbox::_get_configuration_warnings() const
{
	// Not a physics body's shape: the character bake reads it. So no CollisionObject3D warning.
	PackedStringArray warnings;
	bool underBone = false;
	for ( Node* n = get_parent(); n != nullptr; n = n->get_parent() )
	{
		underBone |= Object::cast_to<BoneAttachment3D>( n ) != nullptr;
	}
	if ( underBone == false )
	{
		warnings.push_back( "Put this hitbox under a BoneAttachment3D: it follows that bone." );
	}
	Ref<Shape3D> shape = get_shape();
	if ( shape.is_null() || ( Object::cast_to<SphereShape3D>( shape.ptr() ) == nullptr && Object::cast_to<CapsuleShape3D>( shape.ptr() ) == nullptr &&
							  Object::cast_to<BoxShape3D>( shape.ptr() ) == nullptr ) )
	{
		warnings.push_back( "Hitboxes are spheres, capsules or boxes." );
	}
	if ( m_zone.strip_edges().is_empty() || m_zone.contains( " " ) )
	{
		warnings.push_back( "The zone is one word, like head or torso." );
	}
	return warnings;
}

// --- CbCharacter ------------------------------------------------------------------------------------

void CbCharacter::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "bake_to", "folder" ), &CbCharacter::bake_to );
	ClassDB::bind_method( D_METHOD( "bake" ), &CbCharacter::bake );
	ClassDB::bind_method( D_METHOD( "get_bake_button" ), &CbCharacter::get_bake_button );

#define CB_PROP( type, name, hint, hintText )                                                                                          \
	ClassDB::bind_method( D_METHOD( "set_" #name, "value" ), &CbCharacter::set_##name );                                               \
	ClassDB::bind_method( D_METHOD( "get_" #name ), &CbCharacter::get_##name );                                                        \
	ADD_PROPERTY( PropertyInfo( type, #name, hint, hintText ), "set_" #name, "get_" #name );

	CB_PROP( Variant::STRING, character_name, PROPERTY_HINT_PLACEHOLDER_TEXT, "the item's name" )
	CB_PROP( Variant::NODE_PATH, skeleton_path, PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Skeleton3D" )
	CB_PROP( Variant::NODE_PATH, animation_player_path, PROPERTY_HINT_NODE_PATH_VALID_TYPES, "AnimationPlayer" )
	ADD_GROUP( "Clips", "clip_" );
	CB_PROP( Variant::STRING, clip_idle, PROPERTY_HINT_NONE, "" )
	CB_PROP( Variant::STRING, clip_walk, PROPERTY_HINT_NONE, "" )
	CB_PROP( Variant::STRING, clip_run, PROPERTY_HINT_NONE, "" )
	CB_PROP( Variant::STRING, clip_jump_start, PROPERTY_HINT_NONE, "" )
	CB_PROP( Variant::STRING, clip_fall, PROPERTY_HINT_NONE, "" )
	CB_PROP( Variant::STRING, clip_land, PROPERTY_HINT_NONE, "" )
	ADD_GROUP( "Bake", "" );
	CB_PROP( Variant::FLOAT, sample_rate, PROPERTY_HINT_RANGE, "10,120,1,suffix:Hz" )
	CB_PROP( Variant::BOOL, lock_root_xz, PROPERTY_HINT_NONE, "" )
	ADD_GROUP( "Layers", "" );
	CB_PROP( Variant::DICTIONARY, stance_clips, PROPERTY_HINT_DICTIONARY_TYPE, "String;String" )
	CB_PROP( Variant::DICTIONARY, masks, PROPERTY_HINT_DICTIONARY_TYPE, "String;String" )
	ADD_GROUP( "Aim", "aim_" );
	CB_PROP( Variant::STRING, aim_chain, PROPERTY_HINT_PLACEHOLDER_TEXT, "UpperChest:0.3 RightUpperArm:1" )
	CB_PROP( Variant::STRING, aim_tip, PROPERTY_HINT_NONE, "" )
#undef CB_PROP
	ADD_PROPERTY( PropertyInfo( Variant::CALLABLE, "bake_button", PROPERTY_HINT_TOOL_BUTTON, "Bake character,Save",
								PROPERTY_USAGE_EDITOR ),
				  "", "get_bake_button" );
}

Callable CbCharacter::get_bake_button()
{
	return Callable( this, "bake" );
}

PackedStringArray CbCharacter::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	if ( m_name.strip_edges().is_empty() )
	{
		warnings.push_back( "Name the character: it is the item's name, and the bake writes to res://characters/<name>/." );
	}
	return warnings;
}

void CbCharacter::bake()
{
	Dictionary result = bake_to( String() );
	if ( bool( result["ok"] ) )
	{
		UtilityFunctions::print( "Baked character ", m_name, " into ", result["folder"], ": ", result["joints"], " joints, ",
								 result["clips"], " clips, ", result["hitboxes"], " hitboxes" );
		if ( String( result["warnings"] ).is_empty() == false )
		{
			UtilityFunctions::push_warning( "Character bake: ", result["warnings"] );
		}
	}
	else
	{
		UtilityFunctions::push_error( "Character bake failed: ", result["error"] );
	}
}

Dictionary CbCharacter::bake_to( const String& requestedFolder )
{
	Dictionary result;
	result["ok"] = false;
	String warnings;
	auto fail = [&]( const String& error ) {
		result["error"] = error;
		return result;
	};

	String name = m_name.strip_edges();
	if ( name.is_empty() || name.contains( "/" ) || name.contains( " " ) )
	{
		return fail( "character_name must be one word (the item's name)" );
	}
	String folder = requestedFolder.is_empty() ? "res://characters/" + name + "/" : requestedFolder;
	if ( folder.ends_with( "/" ) == false )
	{
		folder += "/";
	}

	Skeleton3D* skeleton = m_skeleton.is_empty() ? FindFirst<Skeleton3D>( this ) : Object::cast_to<Skeleton3D>( get_node_or_null( m_skeleton ) );
	if ( skeleton == nullptr )
	{
		return fail( "no Skeleton3D (set skeleton_path)" );
	}
	AnimationPlayer* player =
		m_player.is_empty() ? FindFirst<AnimationPlayer>( this ) : Object::cast_to<AnimationPlayer>( get_node_or_null( m_player ) );
	if ( player == nullptr )
	{
		return fail( "no AnimationPlayer (set animation_player_path)" );
	}
	// The simulation places the skeleton's space at the character's feet, facing +Z; the scene must
	// agree, or hitboxes would sit away from the mesh.
	if ( IsIdentity( RelativeTo( skeleton, this ) ) == false )
	{
		return fail( "the Skeleton3D must sit at the character's origin, unrotated and unscaled (reset the nodes above it, or "
					 "import with Apply Root Scale / Apply Node Transforms)" );
	}

	// --- Skeleton: the rest pose, hierarchy and names as they are.
	using namespace ozz::animation::offline;
	RawSkeleton raw;
	std::function<void( int, RawSkeleton::Joint& )> fill = [&]( int bone, RawSkeleton::Joint& joint ) {
		joint.name = Std( skeleton->get_bone_name( bone ) ).c_str();
		joint.transform = ToOzz( skeleton->get_bone_rest( bone ) );
		PackedInt32Array children = skeleton->get_bone_children( bone );
		joint.children.resize( size_t( children.size() ) );
		for ( int64_t c = 0; c < children.size(); ++c )
		{
			fill( children[c], joint.children[size_t( c )] );
		}
	};
	PackedInt32Array roots = skeleton->get_parentless_bones();
	raw.roots.resize( size_t( roots.size() ) );
	for ( int64_t r = 0; r < roots.size(); ++r )
	{
		fill( roots[r], raw.roots[size_t( r )] );
	}
	if ( raw.Validate() == false )
	{
		return fail( "the skeleton is not valid for ozz (too many bones?)" );
	}
	ozz::unique_ptr<ozz::animation::Skeleton> built = SkeletonBuilder()( raw );
	if ( built == nullptr )
	{
		return fail( "ozz could not build the skeleton" );
	}
	const int joints = built->num_joints();
	std::vector<int> boneOf( size_t( joints ), -1 ); // ozz joint -> Godot bone
	for ( int j = 0; j < joints; ++j )
	{
		boneOf[size_t( j )] = skeleton->find_bone( String::utf8( built->joint_names()[size_t( j )] ) );
	}
	for ( const char* required : { "Hips", "Head", "RightHand", "LeftHand", "LeftFoot", "RightFoot" } )
	{
		if ( skeleton->find_bone( required ) < 0 )
		{
			warnings += String( "no bone named " ) + required + " (use Godot's SkeletonProfileHumanoid names); ";
		}
	}

	DirAccess::make_dir_recursive_absolute( folder );
	String error;
	if ( WriteFile( folder + "skeleton.ozz", Archive( *built ), error ) == false )
	{
		return fail( error );
	}

	// --- Clips: sampled from the AnimationPlayer at a fixed rate, bone by bone.
	Node* animationRoot = player->get_node_or_null( player->get_root_node() );
	std::string cfg = "# Baked by CbCharacter in the editor. The game reads these files; edit the scene and bake again.\n";
	cfg += "skeleton = skeleton.ozz\nscale = 1\n";
	cfg += std::string( "lock_root_xz = " ) + ( m_lockRootXZ ? "true" : "false" ) + "\n";
	cfg += "aim = " + Std( m_aimChain.strip_edges() ) + "\n";
	cfg += "aim_tip = " + Std( m_aimTip.strip_edges() ) + "\n";
	{
		PackedStringArray entries = m_aimChain.strip_edges().split( " ", false );
		for ( const String& entry : entries )
		{
			String bone = entry.get_slice( ":", 0 );
			if ( skeleton->find_bone( bone ) < 0 )
			{
				warnings += "aim chain bone " + bone + " is not in the skeleton; ";
			}
		}
		if ( entries.is_empty() == false && skeleton->find_bone( m_aimTip.strip_edges() ) < 0 )
		{
			warnings += "aim tip " + m_aimTip + " is not in the skeleton; ";
		}
	}
	// One clip: sampled from an animation of the AnimationPlayer at a fixed rate, bone by bone,
	// built by ozz and written as `file`. Returns an error, or "".
	auto bakeClip = [&]( const String& animationName, const String& file ) -> String {
		Ref<Animation> animation = player->get_animation( animationName );
		float duration = std::max( float( animation->get_length() ), 1.0f / float( m_sampleRate ) );

		// Which tracks move which bone.
		std::vector<int> positionTrack( size_t( skeleton->get_bone_count() ), -1 );
		std::vector<int> rotationTrack( positionTrack.size(), -1 );
		std::vector<int> scaleTrack( positionTrack.size(), -1 );
		for ( int t = 0; t < animation->get_track_count(); ++t )
		{
			String path = String( animation->track_get_path( t ) );
			int colon = path.find( ":" );
			if ( colon < 0 )
			{
				continue;
			}
			Node* target = animationRoot != nullptr ? animationRoot->get_node_or_null( NodePath( path.substr( 0, colon ) ) ) : nullptr;
			int bone = target == skeleton ? skeleton->find_bone( path.substr( colon + 1 ) ) : -1;
			if ( bone < 0 )
			{
				continue;
			}
			switch ( animation->track_get_type( t ) )
			{
				case Animation::TYPE_POSITION_3D:
					positionTrack[size_t( bone )] = t;
					break;
				case Animation::TYPE_ROTATION_3D:
					rotationTrack[size_t( bone )] = t;
					break;
				case Animation::TYPE_SCALE_3D:
					scaleTrack[size_t( bone )] = t;
					break;
				default:
					break;
			}
		}

		RawAnimation rawClip;
		rawClip.duration = duration;
		rawClip.tracks.resize( size_t( joints ) );
		int samples = std::max( 2, int( std::ceil( duration * float( m_sampleRate ) ) ) + 1 );
		for ( int j = 0; j < joints; ++j )
		{
			int bone = boneOf[size_t( j )];
			RawAnimation::JointTrack& track = rawClip.tracks[size_t( j )];
			ozz::math::Transform rest = ToOzz( skeleton->get_bone_rest( bone ) );
			int pt = positionTrack[size_t( bone )];
			int rt = rotationTrack[size_t( bone )];
			int st = scaleTrack[size_t( bone )];
			if ( pt < 0 )
			{
				track.translations.push_back( { 0.0f, rest.translation } );
			}
			if ( rt < 0 )
			{
				track.rotations.push_back( { 0.0f, rest.rotation } );
			}
			if ( st < 0 )
			{
				track.scales.push_back( { 0.0f, rest.scale } );
			}
			ozz::math::Quaternion previous = rest.rotation;
			for ( int s = 0; s < samples; ++s )
			{
				float time = std::min( duration, float( s ) / float( m_sampleRate ) );
				if ( s == samples - 1 )
				{
					time = duration;
				}
				if ( pt >= 0 )
				{
					Vector3 p = animation->position_track_interpolate( pt, time );
					track.translations.push_back( { time, ozz::math::Float3( float( p.x ), float( p.y ), float( p.z ) ) } );
				}
				if ( rt >= 0 )
				{
					Quaternion q = animation->rotation_track_interpolate( rt, time ).normalized();
					ozz::math::Quaternion oq( float( q.x ), float( q.y ), float( q.z ), float( q.w ) );
					// Keep neighbouring keys in the same hemisphere so interpolation takes the short way.
					if ( previous.x * oq.x + previous.y * oq.y + previous.z * oq.z + previous.w * oq.w < 0.0f )
					{
						oq = ozz::math::Quaternion( -oq.x, -oq.y, -oq.z, -oq.w );
					}
					previous = oq;
					track.rotations.push_back( { time, oq } );
				}
				if ( st >= 0 )
				{
					Vector3 v = animation->scale_track_interpolate( st, time );
					track.scales.push_back( { time, ozz::math::Float3( float( v.x ), float( v.y ), float( v.z ) ) } );
				}
				if ( time >= duration )
				{
					break;
				}
			}
		}
		if ( rawClip.Validate() == false )
		{
			return "animation " + animationName + " is not valid for ozz";
		}
		ozz::unique_ptr<ozz::animation::Animation> builtClip = AnimationBuilder()( rawClip );
		if ( builtClip == nullptr )
		{
			return "ozz could not build animation " + animationName;
		}
		String writeError;
		if ( WriteFile( folder + file, Archive( *builtClip ), writeError ) == false )
		{
			return writeError;
		}
		return String();
	};

	// Companion tracks: everything but the bones, kept as Godot animations with the baked clips' names
	// (see cinderbox_companion.h). A track is a bone track when it moves a bone of the skeleton.
	Ref<AnimationLibrary> companion;
	companion.instantiate();
	auto addCompanion = [&]( const String& animationName, const String& clipName ) {
		Ref<Animation> source = player->get_animation( animationName );
		Ref<Animation> rest = source->duplicate();
		for ( int t = rest->get_track_count() - 1; t >= 0; --t )
		{
			Animation::TrackType type = rest->track_get_type( t );
			bool transform = type == Animation::TYPE_POSITION_3D || type == Animation::TYPE_ROTATION_3D || type == Animation::TYPE_SCALE_3D;
			String path = String( rest->track_get_path( t ) );
			int colon = path.find( ":" );
			Node* target = animationRoot != nullptr && colon >= 0 ? animationRoot->get_node_or_null( NodePath( path.substr( 0, colon ) ) ) : nullptr;
			if ( transform && target == skeleton )
			{
				rest->remove_track( t );
			}
		}
		if ( rest->get_track_count() > 0 )
		{
			companion->add_animation( clipName, rest );
		}
	};

	int clips = 0;
	for ( int c = 0; c < anim::ClipCount; ++c )
	{
		const char* clipName = anim::ClipName( anim::Clip( c ) );
		String animationName = m_clips[c].strip_edges();
		if ( animationName.is_empty() || player->has_animation( animationName ) == false )
		{
			warnings += String( "no animation '" ) + animationName + "' for clip " + clipName + "; ";
			continue;
		}
		String file = String( clipName ) + ".ozz";
		String problem = bakeClip( animationName, file );
		if ( problem.is_empty() == false )
		{
			return fail( problem );
		}
		cfg += std::string( clipName ) + " = " + Std( file ) + "\n";
		addCompanion( animationName, clipName );
		++clips;
	}

	// Stances: "<stance>" (one loop) or "<stance>_<clip>" -> an animation of the player.
	int stanceClips = 0;
	Array stanceNames = m_stanceClips.keys();
	for ( int64_t i = 0; i < stanceNames.size(); ++i )
	{
		String name = String( stanceNames[i] ).strip_edges();
		String animationName = String( m_stanceClips[stanceNames[i]] ).strip_edges();
		if ( name.is_empty() || name.contains( " " ) || name.contains( "/" ) )
		{
			warnings += "stance clip name '" + name + "' must be one word; ";
			continue;
		}
		if ( player->has_animation( animationName ) == false )
		{
			warnings += "no animation '" + animationName + "' for stance clip " + name + "; ";
			continue;
		}
		String file = "stance_" + name + ".ozz";
		String problem = bakeClip( animationName, file );
		if ( problem.is_empty() == false )
		{
			return fail( problem );
		}
		cfg += "stance." + Std( name ) + " = " + Std( file ) + "\n";
		addCompanion( animationName, "stance_" + name );
		++stanceClips;
	}

	// Layer masks: "<layer>" -> bones whose subtrees it covers ("Spine", "Spine:0.5 RightShoulder").
	Array layerNames = m_masks.keys();
	for ( int64_t i = 0; i < layerNames.size(); ++i )
	{
		String layer = String( layerNames[i] ).strip_edges();
		String roots = String( m_masks[layerNames[i]] ).strip_edges();
		for ( const String& entry : roots.split( " ", false ) )
		{
			if ( skeleton->find_bone( entry.get_slice( ":", 0 ) ) < 0 )
			{
				warnings += "mask " + layer + ": no bone " + entry.get_slice( ":", 0 ) + "; ";
			}
		}
		cfg += "mask." + Std( layer ) + " = " + Std( roots ) + "\n";
	}
	result["stance_clips"] = stanceClips;
	if ( player->has_animation( "RESET" ) )
	{
		addCompanion( "RESET", "RESET" ); // what a channel returns to when its clip has nothing to say
	}
	if ( ResourceSaver::get_singleton()->save( companion, folder + "companion.tres" ) != OK )
	{
		return fail( "cannot write " + folder + "companion.tres" );
	}
	result["companion_clips"] = int64_t( companion->get_animation_list().size() );

	if ( WriteText( folder + "anim.cfg", cfg, error ) == false )
	{
		return fail( error );
	}

	// --- Hitboxes, in their bone's space.
	std::vector<CbHitbox*> hitboxNodes;
	Collect( this, hitboxNodes );
	anim::HitboxSet hitboxes;
	for ( CbHitbox* node : hitboxNodes )
	{
		BoneAttachment3D* attachment = nullptr;
		for ( Node* n = node->get_parent(); n != nullptr && attachment == nullptr; n = n->get_parent() )
		{
			attachment = Object::cast_to<BoneAttachment3D>( n );
		}
		if ( attachment == nullptr )
		{
			warnings += "hitbox " + String( node->get_name() ) + " is not under a BoneAttachment3D; ";
			continue;
		}
		Transform3D local = RelativeTo( node, attachment );
		float scale = float( local.basis.get_column( 0 ).length() );
		Quaternion q = local.basis.orthonormalized().get_rotation_quaternion();
		anim::Hitbox box;
		box.zone = Std( node->get_zone().strip_edges() );
		box.bone = Std( attachment->get_bone_name() );
		box.translation = { float( local.origin.x ), float( local.origin.y ), float( local.origin.z ) };
		box.rotation = { { float( q.x ), float( q.y ), float( q.z ) }, float( q.w ) };
		Ref<Shape3D> shape = node->get_shape();
		if ( auto* sphere = Object::cast_to<SphereShape3D>( shape.ptr() ) )
		{
			box.shape = anim::HitShape::Sphere;
			box.radius = float( sphere->get_radius() ) * scale;
		}
		else if ( auto* capsule = Object::cast_to<CapsuleShape3D>( shape.ptr() ) )
		{
			box.shape = anim::HitShape::Capsule;
			box.radius = float( capsule->get_radius() ) * scale;
			box.height = float( capsule->get_height() ) * scale;
		}
		else if ( auto* cube = Object::cast_to<BoxShape3D>( shape.ptr() ) )
		{
			box.shape = anim::HitShape::Box;
			Vector3 half = cube->get_size() * 0.5 * scale;
			box.halfExtents = { float( half.x ), float( half.y ), float( half.z ) };
		}
		else
		{
			warnings += "hitbox " + String( node->get_name() ) + " is not a sphere, capsule or box; ";
			continue;
		}
		if ( box.zone.empty() || box.bone.empty() || skeleton->find_bone( attachment->get_bone_name() ) < 0 )
		{
			warnings += "hitbox " + String( node->get_name() ) + " has no zone or no bone; ";
			continue;
		}
		hitboxes.boxes.push_back( box );
	}
	if ( hitboxes.boxes.empty() )
	{
		return fail( "no hitboxes: add CbHitbox shapes under BoneAttachment3D nodes" );
	}

	// The ozz pose places the body; hitboxes follow it. Godot animation may add to a player, but
	// nothing it does to a bone with a hitbox is seen by the server.
	String hitBones;
	for ( const anim::Hitbox& box : hitboxes.boxes )
	{
		String bone = String::utf8( box.bone.c_str() );
		if ( hitBones.contains( bone ) == false )
		{
			hitBones += ( hitBones.is_empty() ? "" : ", " ) + bone;
		}
	}
	std::function<void( Node* )> scan = [&]( Node* node ) {
		if ( auto* tree = Object::cast_to<AnimationTree>( node ) )
		{
			warnings += "AnimationTree " + String( tree->get_name() ) +
						" runs before the ozz pose: on players only what the pose leaves alone survives (faces, props, "
						"materials; not " +
						hitBones + "); ";
		}
		if ( auto* modifier = Object::cast_to<SkeletonModifier3D>( node ) )
		{
			if ( Object::cast_to<CbPoseModifier>( node ) == nullptr )
			{
				warnings += "skeleton modifier " + String( modifier->get_name() ) +
							" runs after the ozz pose: keep it off the bones with hitboxes (" + hitBones +
							"), or the server will hit where it is not drawn; ";
			}
		}
		for ( int i = 0; i < node->get_child_count(); ++i )
		{
			scan( node->get_child( i ) );
		}
	};
	scan( this );
	if ( WriteText( folder + "hitboxes.cfg", anim::FormatHitboxes( hitboxes ), error ) == false )
	{
		return fail( error );
	}

	if ( Engine::get_singleton()->is_editor_hint() && EditorInterface::get_singleton() != nullptr )
	{
		EditorInterface::get_singleton()->get_resource_filesystem()->scan();
	}
	result["ok"] = true;
	result["folder"] = folder;
	result["joints"] = joints;
	result["clips"] = clips;
	result["hitboxes"] = int64_t( hitboxes.boxes.size() );
	result["warnings"] = warnings;
	return result;
}

} // namespace cb::gd
