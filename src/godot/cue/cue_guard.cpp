#include "cue_guard.h"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/animation_library.hpp>
#include <godot_cpp/classes/animation_node.hpp>
#include <godot_cpp/classes/animation_node_state_machine_transition.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_state.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <unordered_map>
#include <unordered_set>

using namespace godot;

namespace cb::cue
{

namespace
{

// Everything a look is made of. A class that is not here is refused: add it when a look needs it
// and it cannot reach outside its scene or the process.
const char* const kNodeClasses[] = {
	// Structure
	"Node", "Node3D", "Marker3D", "Node2D", "Marker2D", "CanvasLayer", "CanvasGroup",
	// 3D looks
	"MeshInstance3D", "MultiMeshInstance3D", "Sprite3D", "AnimatedSprite3D", "Label3D", "Decal",
	"GPUParticles3D", "CPUParticles3D", "GPUParticlesAttractorBox3D", "GPUParticlesAttractorSphere3D",
	"GPUParticlesAttractorVectorField3D", "GPUParticlesCollisionBox3D", "GPUParticlesCollisionSphere3D",
	"GPUParticlesCollisionHeightField3D", "GPUParticlesCollisionSDF3D",
	"OmniLight3D", "SpotLight3D", "DirectionalLight3D", "FogVolume", "ReflectionProbe", "LightmapGI", "VoxelGI",
	"WorldEnvironment", "CSGBox3D", "CSGSphere3D", "CSGCylinder3D", "CSGTorus3D", "CSGPolygon3D", "CSGMesh3D",
	"CSGCombiner3D", "Path3D", "PathFollow3D",
	// Loose bodies for looks only (the simulation's entities never have Godot bodies)
	"StaticBody3D", "RigidBody3D", "CollisionShape3D",
	// Skeletons and animation
	"Skeleton3D", "BoneAttachment3D", "LookAtModifier3D", "SpringBoneSimulator3D", "AnimationPlayer", "AnimationTree",
	// Sound
	"AudioStreamPlayer", "AudioStreamPlayer2D", "AudioStreamPlayer3D",
	// 2D looks
	"Sprite2D", "AnimatedSprite2D", "GPUParticles2D", "CPUParticles2D", "Line2D", "Polygon2D",
	// UI
	"Control", "Label", "RichTextLabel", "Panel", "PanelContainer", "MarginContainer", "VBoxContainer", "HBoxContainer",
	"GridContainer", "CenterContainer", "AspectRatioContainer", "ScrollContainer", "HFlowContainer", "VFlowContainer",
	"TabContainer", "TextureRect", "ColorRect", "NinePatchRect", "ProgressBar", "TextureProgressBar", "HSeparator",
	"VSeparator", "Button", "TextureButton", "CheckButton", "CheckBox", "LineEdit", "HSlider", "VSlider", "SpinBox",
	"OptionButton", "ItemList",
};

const char* const kMethods[] = {
	"restart", "play", "play_backwards", "stop", "pause", "queue", "clear_queue", "seek", "advance",
	"show", "hide", "set_visible", "set_emitting", "set_text", "set_value", "set_frame", "set_modulate", "set_self_modulate",
	"set_stream_paused", "set_volume_db", "set_pitch_scale", "set_speed_scale",
};

struct NameHash
{
	size_t operator()( const String& s ) const
	{
		return size_t( s.hash() );
	}
};

const std::unordered_set<String, NameHash>& NodeClasses()
{
	static const std::unordered_set<String, NameHash> set = [] {
		std::unordered_set<String, NameHash> names;
		for ( const char* name : kNodeClasses )
		{
			names.insert( String( name ) );
		}
		return names;
	}();
	return set;
}

// --- The walk ----------------------------------------------------------------------------------

struct Walk
{
	std::unordered_set<uint64_t> seen; // resources already checked in this walk
	int depth = 0;
};

String CheckVariant( const Variant& value, int nodeDepth, Walk& walk );
String CheckSceneState( const Ref<PackedScene>& scene, Walk& walk );

// A NodePath kept inside the scene: not absolute, and climbing at most `nodeDepth` levels (a node
// `nodeDepth` below the scene's root reaches the root with that many ".."). nodeDepth < 0: the path
// is not anchored at a known node (an animation track), so it must not climb at all.
String CheckPathStays( const NodePath& path, int nodeDepth )
{
	if ( path.is_absolute() )
	{
		return "the path " + String( path ) + " starts at the root";
	}
	int climbs = 0;
	for ( int64_t i = 0; i < path.get_name_count(); ++i )
	{
		climbs += String( path.get_name( i ) ) == ".." ? 1 : 0;
	}
	if ( climbs > std::max( nodeDepth, 0 ) )
	{
		return "the path " + String( path ) + " leaves the scene";
	}
	return String();
}

String CheckAnimation( const Ref<Animation>& animation )
{
	for ( int32_t t = 0; t < animation->get_track_count(); ++t )
	{
		NodePath path = animation->track_get_path( t );
		String problem = CheckPathStays( path, -1 );
		if ( problem.is_empty() == false )
		{
			return "an animation track: " + problem;
		}
		String property = path.get_concatenated_subnames();
		if ( property.is_empty() == false && PropertyAllowed( property ) == false )
		{
			return "an animation track sets " + property;
		}
		if ( animation->track_get_type( t ) == Animation::TYPE_METHOD )
		{
			for ( int32_t k = 0; k < animation->track_get_key_count( t ); ++k )
			{
				String method = animation->method_track_get_name( t, k );
				if ( MethodAllowed( method ) == false )
				{
					return "an animation calls " + method + "()";
				}
			}
		}
	}
	return String();
}

String CheckResourceIn( const Ref<Resource>& resource, Walk& walk )
{
	if ( resource.is_null() || walk.seen.insert( resource->get_instance_id() ).second == false )
	{
		return String();
	}
	if ( Object::cast_to<Script>( resource.ptr() ) != nullptr || resource->get_script().get_type() != Variant::NIL )
	{
		return "a script";
	}
	if ( Ref<PackedScene> scene = resource; scene.is_valid() )
	{
		return CheckSceneState( scene, walk );
	}
	if ( Ref<Animation> animation = resource; animation.is_valid() )
	{
		return CheckAnimation( animation );
	}
	if ( Ref<AnimationLibrary> library = resource; library.is_valid() )
	{
		TypedArray<StringName> names = library->get_animation_list();
		for ( int64_t i = 0; i < names.size(); ++i )
		{
			String problem = CheckResourceIn( library->get_animation( names[i] ), walk );
			if ( problem.is_empty() == false )
			{
				return problem + " (animation " + String( names[i] ) + ")";
			}
		}
		return String();
	}
	if ( Ref<AnimationNodeStateMachineTransition> transition = resource; transition.is_valid() )
	{
		// An advance expression is evaluated against a node: it can call that node's methods. The
		// game never needs one (a character's state machine runs baked, in the simulation), so it is
		// taken out. Not in the editor: there the scene is the author's own, being edited.
		if ( transition->get_advance_expression().is_empty() == false && Engine::get_singleton()->is_editor_hint() == false )
		{
			transition->set_advance_expression( String() );
		}
		return String();
	}
	if ( Object::cast_to<AnimationNode>( resource.ptr() ) != nullptr )
	{
		// A blend tree or state machine: its nodes and transitions are in its properties.
		TypedArray<Dictionary> properties = resource->get_property_list();
		for ( int64_t i = 0; i < properties.size(); ++i )
		{
			Dictionary property = properties[i];
			int type = property.get( "type", 0 );
			if ( type != Variant::OBJECT && type != Variant::ARRAY && type != Variant::DICTIONARY )
			{
				continue;
			}
			String problem = CheckVariant( resource->get( property["name"] ), -1, walk );
			if ( problem.is_empty() == false )
			{
				return problem;
			}
		}
	}
	// Anything else (a mesh, a material, a texture, a sound) is data.
	return String();
}

String CheckVariant( const Variant& value, int nodeDepth, Walk& walk )
{
	switch ( value.get_type() )
	{
		case Variant::NODE_PATH:
			return nodeDepth >= 0 ? CheckPathStays( value, nodeDepth ) : String();
		case Variant::OBJECT:
		{
			Ref<Resource> resource = value;
			return CheckResourceIn( resource, walk );
		}
		case Variant::ARRAY:
		{
			Array array = value;
			for ( int64_t i = 0; i < array.size(); ++i )
			{
				String problem = CheckVariant( array[i], nodeDepth, walk );
				if ( problem.is_empty() == false )
				{
					return problem;
				}
			}
			return String();
		}
		case Variant::DICTIONARY:
		{
			Dictionary dictionary = value;
			Array values = dictionary.values();
			for ( int64_t i = 0; i < values.size(); ++i )
			{
				String problem = CheckVariant( values[i], nodeDepth, walk );
				if ( problem.is_empty() == false )
				{
					return problem;
				}
			}
			return String();
		}
		default:
			return String();
	}
}

String CheckSceneState( const Ref<PackedScene>& scene, Walk& walk )
{
	Ref<SceneState> state = scene->get_state();
	if ( state.is_null() )
	{
		return String();
	}
	if ( ++walk.depth > 32 )
	{
		return "scenes nested too deep";
	}
	if ( state->get_connection_count() > 0 )
	{
		return "a signal connection (" + String( state->get_connection_signal( 0 ) ) + " -> " + String( state->get_connection_method( 0 ) ) + "())";
	}
	for ( int32_t n = 0; n < state->get_node_count(); ++n )
	{
		String name = state->get_node_name( n );
		StringName type = state->get_node_type( n );
		// No type: an instance of another scene, or a node of one being changed here.
		if ( String( type ).is_empty() == false && NodeClassAllowed( type ) == false )
		{
			return "a " + String( type ) + " node (" + name + ")";
		}
		Ref<PackedScene> instance = state->get_node_instance( n );
		if ( instance.is_valid() )
		{
			String problem = CheckResourceIn( instance, walk );
			if ( problem.is_empty() == false )
			{
				return problem + ", in the scene " + name + " is an instance of";
			}
		}
		// How far below the scene's root this node is: its paths may climb that far.
		NodePath path = state->get_node_path( n );
		int depth = 0;
		for ( int64_t i = 0; i < path.get_name_count(); ++i )
		{
			depth += String( path.get_name( i ) ) != "." ? 1 : 0;
		}
		for ( int32_t p = 0; p < state->get_node_property_count( n ); ++p )
		{
			String property = state->get_node_property_name( n, p );
			if ( property == "script" )
			{
				return "a script (on " + name + ")";
			}
			String problem = CheckVariant( state->get_node_property_value( n, p ), depth, walk );
			if ( problem.is_empty() == false )
			{
				return problem + " (" + name + "." + property + ")";
			}
		}
	}
	walk.depth -= 1;
	return String();
}

// Scenes already judged, by instance.
std::unordered_map<uint64_t, String>& Judged()
{
	static std::unordered_map<uint64_t, String> judged;
	return judged;
}

} // namespace

bool NodeClassAllowed( const StringName& type )
{
	String name = type;
	if ( NodeClasses().count( name ) != 0 )
	{
		return true;
	}
	// This extension's own nodes. A pack cannot define a class, so the prefix cannot be borrowed.
	return ( name.begins_with( "Cb" ) || name.begins_with( "Cinderbox" ) ) && ClassDBSingleton::get_singleton()->class_exists( type );
}

bool MethodAllowed( const String& method )
{
	for ( const char* allowed : kMethods )
	{
		if ( method == allowed )
		{
			return true;
		}
	}
	return false;
}

bool PropertyAllowed( const String& property )
{
	return property != "script" && property.begins_with( "script:" ) == false && property.begins_with( "metadata/" ) == false;
}

String CheckResource( const Ref<Resource>& resource )
{
	Walk walk;
	return CheckResourceIn( resource, walk );
}

String CheckScene( const Ref<PackedScene>& scene )
{
	if ( scene.is_null() )
	{
		return String();
	}
	uint64_t id = scene->get_instance_id();
	auto found = Judged().find( id );
	if ( found != Judged().end() )
	{
		return found->second;
	}
	Walk walk;
	walk.seen.insert( id );
	String problem = CheckSceneState( scene, walk );
	Judged()[id] = problem;
	if ( problem.is_empty() == false )
	{
		String path = scene->get_path();
		UtilityFunctions::push_warning( "Cinderbox: the scene ", path.is_empty() ? String( "(built in)" ) : path,
										" is not used: it has ", problem, ". A look is made of listed node classes and data only." );
	}
	return problem;
}

Node* Instantiate( const Ref<PackedScene>& scene )
{
	if ( scene.is_null() || CheckScene( scene ).is_empty() == false )
	{
		return nullptr;
	}
	return scene->instantiate();
}

} // namespace cb::cue
