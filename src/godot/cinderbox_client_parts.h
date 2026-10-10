#pragma once

// What the parts of CinderboxClient share. The class is one (cinderbox_client.h); its code is in
// several files, by what it is about:
//
//   cinderbox_client.cpp         connecting, input, the frame it draws, the entities' nodes, the lead
//   cinderbox_client_items.cpp   sockets and held items: where an item's scene hangs on a body
//   cinderbox_client_world.cpp   the director: cues, state and the world's reactions
//   cinderbox_client_values.cpp  what looks ask it: fields, conditions, formats, names, lists

#include "cinderbox_client.h"

#include "cinderbox_item_look.h"
#include "motions.h"
#include "ragdoll.h"
#include "joint_math.h"

#include "camera.h"
#include "cinderbox_character.h"
#include "cinderbox_track_player.h"
#include "cinderbox_skeleton.h"
#include "cue_guard.h"
#include "cue_prediction.h"
#include "detmath.h"
#include "object_source.h"
#include "pose_tools.h"
#include "view_file.h"
#include "types.h"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/bone_attachment3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_map.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

using namespace godot;

namespace cb::gd
{

namespace client_parts
{

inline const Color kSlotColors[] = {
	Color( 0.90f, 0.31f, 0.27f ), Color( 0.27f, 0.55f, 0.90f ), Color( 0.35f, 0.78f, 0.43f ), Color( 0.78f, 0.47f, 0.86f ),
	Color( 0.94f, 0.63f, 0.24f ), Color( 0.24f, 0.78f, 0.78f ), Color( 0.71f, 0.71f, 0.35f ), Color( 0.59f, 0.43f, 0.31f ),
};

inline Vector3 ToGodot( b3Vec3 v )
{
	return Vector3( v.x, v.y, v.z );
}

inline Quaternion ToGodot( b3Quat q )
{
	return Quaternion( q.v.x, q.v.y, q.v.z, q.s );
}

inline const char* KindName( present::VisualKind kind )
{
	switch ( kind )
	{
		case present::VisualKind::Static:
			return "static";
		case present::VisualKind::Player:
			return "player";
		case present::VisualKind::Ragdoll:
			return "ragdoll";
		case present::VisualKind::Item:
			return "item";
		case present::VisualKind::Prop:
		default:
			return "prop";
	}
}

inline std::string ToStd( const String& s )
{
	CharString utf8 = s.utf8();
	return std::string( utf8.get_data(), size_t( utf8.length() ) );
}

template <typename T>
T* FindInPrefab( Node* node )
{
	if ( auto* found = Object::cast_to<T>( node ) )
	{
		return found;
	}
	for ( int i = 0; i < node->get_child_count(); ++i )
	{
		if ( auto* found = FindInPrefab<T>( node->get_child( i ) ) )
		{
			return found;
		}
	}
	return nullptr;
}

inline CinderboxSkeleton* FindSkeleton( Node* node )
{
	return FindInPrefab<CinderboxSkeleton>( node );
}


} // namespace client_parts

using namespace client_parts;

// Where an item's scene goes in the frame it is carried in, and in a socket (cinderbox_client_items.cpp).
godot::Transform3D SceneInCarriedFrame( godot::Node3D* node );
godot::Transform3D SceneInSocket( godot::Node3D* node, uint8_t socket );

} // namespace cb::gd
