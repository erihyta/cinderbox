#include "cinderbox_reaction.h"

#include "entity_path.h"

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/gpu_particles3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/scene_tree_timer.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace cb::gd
{

void CbReaction::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "fire" ), &CbReaction::fire );
	ClassDB::bind_method( D_METHOD( "set_on", "on" ), &CbReaction::set_on );
	ClassDB::bind_method( D_METHOD( "is_on" ), &CbReaction::is_on );
	ClassDB::bind_method( D_METHOD( "fire_in", "root", "limit" ), &CbReaction::FireIn );
	ClassDB::bind_method( D_METHOD( "set_on_in", "on", "root", "limit" ), &CbReaction::SetOnIn );

#define CB_REACTION_PROP( type, name, hint, hintText )                                                                             \
	ClassDB::bind_method( D_METHOD( "set_" #name, "value" ), &CbReaction::set_##name );                                            \
	ClassDB::bind_method( D_METHOD( "get_" #name ), &CbReaction::get_##name );                                                     \
	ADD_PROPERTY( PropertyInfo( type, #name, hint, hintText ), "set_" #name, "get_" #name );

	ADD_GROUP( "When", "" );
	CB_REACTION_PROP( Variant::INT, when, PROPERTY_HINT_ENUM, "On event,While" )
	CB_REACTION_PROP( Variant::STRING, event, PROPERTY_HINT_PLACEHOLDER_TEXT, "melee.hit" )
	CB_REACTION_PROP( Variant::PACKED_STRING_ARRAY, conditions, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::STRING, subject, PROPERTY_HINT_PLACEHOLDER_TEXT, "self, holder, event.b, local" )
	CB_REACTION_PROP( Variant::INT, event_side, PROPERTY_HINT_ENUM, "A: it is about the subject,B: the subject is the other one,Either" )
	CB_REACTION_PROP( Variant::STRING, subject_kind, PROPERTY_HINT_ENUM_SUGGESTION, "any,player,prop,static,ragdoll,item" )
	CB_REACTION_PROP( Variant::STRING, subject_template, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::FLOAT, cooldown, PROPERTY_HINT_RANGE, "0,10,0.01,suffix:s" )
	CB_REACTION_PROP( Variant::STRING, act_on, PROPERTY_HINT_PLACEHOLDER_TEXT, "this scene, or holder, event.b/item:RightHand, world" )
	ADD_GROUP( "Animation", "" );
	CB_REACTION_PROP( Variant::NODE_PATH, animation_player, PROPERTY_HINT_NODE_PATH_VALID_TYPES, "AnimationPlayer" )
	CB_REACTION_PROP( Variant::STRING, animation, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::STRING, animation_off, PROPERTY_HINT_NONE, "" )
	ADD_GROUP( "Property", "" );
	CB_REACTION_PROP( Variant::NODE_PATH, target, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::STRING, property, PROPERTY_HINT_PLACEHOLDER_TEXT, "visible, or surface_material_override/0:albedo_color" )
	ClassDB::bind_method( D_METHOD( "set_value", "value" ), &CbReaction::set_value );
	ClassDB::bind_method( D_METHOD( "get_value" ), &CbReaction::get_value );
	ADD_PROPERTY( PropertyInfo( Variant::NIL, "value", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT ),
				  "set_value", "get_value" );
	CB_REACTION_PROP( Variant::STRING, method, PROPERTY_HINT_PLACEHOLDER_TEXT, "restart" )
	ADD_GROUP( "Scene", "scene" );
	CB_REACTION_PROP( Variant::STRING, scene, PROPERTY_HINT_FILE, "*.tscn,*.scn" )
	CB_REACTION_PROP( Variant::NODE_PATH, scene_parent, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::FLOAT, scene_lifetime, PROPERTY_HINT_RANGE, "0,30,0.05,suffix:s" )
	ADD_GROUP( "Place", "" );
	CB_REACTION_PROP( Variant::INT, place, PROPERTY_HINT_ENUM, "Under its parent,Event point,Event end,Beam (point to end),Subject's bone,Follow the subject" )
	CB_REACTION_PROP( Variant::STRING, bone, PROPERTY_HINT_PLACEHOLDER_TEXT, "RightHand" )
	CB_REACTION_PROP( Variant::VECTOR3, offset, PROPERTY_HINT_NONE, "" )
	ADD_GROUP( "Sound", "" );
	CB_REACTION_PROP( Variant::STRING, sound, PROPERTY_HINT_FILE, "*.wav,*.ogg,*.mp3" )
	CB_REACTION_PROP( Variant::FLOAT, volume_db, PROPERTY_HINT_RANGE, "-60,24,0.1,suffix:dB" )
	CB_REACTION_PROP( Variant::FLOAT, pitch_scale, PROPERTY_HINT_RANGE, "0.01,4,0.01" )
	CB_REACTION_PROP( Variant::FLOAT, pitch_jitter, PROPERTY_HINT_RANGE, "0,1,0.01" )
	CB_REACTION_PROP( Variant::STRING, bus, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::FLOAT, max_distance, PROPERTY_HINT_RANGE, "0,500,0.1,suffix:m" )
	ADD_GROUP( "Screen", "" );
	CB_REACTION_PROP( Variant::FLOAT, shake, PROPERTY_HINT_RANGE, "0,1,0.001" )
	CB_REACTION_PROP( Variant::FLOAT, shake_time, PROPERTY_HINT_RANGE, "0,5,0.01,suffix:s" )
	CB_REACTION_PROP( Variant::COLOR, flash_color, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::FLOAT, flash_time, PROPERTY_HINT_RANGE, "0,5,0.01,suffix:s" )
#undef CB_REACTION_PROP

	BIND_ENUM_CONSTANT( WHEN_EVENT );
	BIND_ENUM_CONSTANT( WHEN_WHILE );
	BIND_ENUM_CONSTANT( SIDE_A );
	BIND_ENUM_CONSTANT( SIDE_B );
	BIND_ENUM_CONSTANT( SIDE_EITHER );
	BIND_ENUM_CONSTANT( PLACE_PARENT );
	BIND_ENUM_CONSTANT( PLACE_EVENT_POINT );
	BIND_ENUM_CONSTANT( PLACE_EVENT_END );
	BIND_ENUM_CONSTANT( PLACE_BEAM );
	BIND_ENUM_CONSTANT( PLACE_BONE );
	BIND_ENUM_CONSTANT( PLACE_FOLLOW );
}

PackedStringArray CbReaction::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	if ( m_when == WHEN_EVENT && m_event.strip_edges().is_empty() )
	{
		warnings.push_back( "Name the event this reacts to (a mod's event, like melee.hit)." );
	}
	if ( m_when == WHEN_WHILE && m_conditions.is_empty() )
	{
		warnings.push_back( "A \"while\" reaction needs conditions (like melee.hot, or pistol.ammo > 0)." );
	}
	if ( m_animation.is_empty() && m_property.is_empty() && m_method.is_empty() && m_scene.is_empty() && m_sound.is_empty() &&
		 m_shake <= 0.0 && m_flashColor.a <= 0.0f )
	{
		warnings.push_back( "It does nothing yet: set an animation, a property, a method, a scene, a sound or a screen effect." );
	}
	if ( m_when == WHEN_WHILE && m_place != PLACE_PARENT && m_place != PLACE_FOLLOW )
	{
		warnings.push_back( "A \"while\" has no event to place things at: use Under its parent or Follow the subject." );
	}
	if ( m_method.is_empty() == false && m_target.is_empty() )
	{
		warnings.push_back( "Calling a method needs a target node." );
	}
	if ( Refused( m_method, m_property ) )
	{
		warnings.push_back( "The game refuses \"free\", \"queue_free\" and \"script\"." );
	}
	std::string error;
	present::EntityPath path;
	if ( present::ParseEntityPath( m_subject.utf8().get_data(), path, &error ) == false )
	{
		warnings.push_back( "Subject: " + String::utf8( error.c_str() ) );
	}
	if ( present::ParseEntityPath( m_actOn.utf8().get_data(), path, &error ) == false )
	{
		warnings.push_back( "Act on: " + String::utf8( error.c_str() ) );
	}
	for ( int64_t i = 0; i < m_conditions.size(); ++i )
	{
		present::PathCondition condition;
		if ( present::ParsePathCondition( m_conditions[i].utf8().get_data(), condition, &error ) == false )
		{
			warnings.push_back( "Condition \"" + m_conditions[i] + "\": " + String::utf8( error.c_str() ) );
		}
	}
	return warnings;
}

void CbReaction::fire()
{
	FireIn( nullptr, nullptr );
}

void CbReaction::set_on( bool on )
{
	SetOnIn( on, nullptr, nullptr );
}

void CbReaction::FireIn( Node* root, Node* limit )
{
	Act( true, root, limit, nullptr );
}

void CbReaction::FireAt( Node* root, Node* limit, const Placement* place )
{
	Act( true, root, limit, place );
}

void CbReaction::SetOnIn( bool on, Node* root, Node* limit )
{
	if ( on == m_on )
	{
		return;
	}
	m_on = on;
	Act( on, root, limit, nullptr );
}

bool CbReaction::Refused( const String& method, const String& property )
{
	return method == "free" || method == "queue_free" || property == "script" || property.begins_with( "script:" );
}

Node* CbReaction::Find( const NodePath& path, Node* root, Node* limit ) const
{
	if ( path.is_empty() )
	{
		return nullptr;
	}
	Node* found = root != nullptr ? root->get_node_or_null( path ) : get_node_or_null( path );
	if ( found != nullptr && limit != nullptr && found != limit && limit->is_ancestor_of( found ) == false )
	{
		return nullptr; // outside the entity's scene
	}
	return found;
}

void CbReaction::PlaySound( Node* parent, const Placement* place )
{
	Ref<AudioStream> stream = ResourceLoader::get_singleton()->load( m_sound, "AudioStream" );
	if ( stream.is_null() || parent == nullptr )
	{
		return;
	}
	auto* player = memnew( AudioStreamPlayer3D );
	player->set_stream( stream );
	player->set_volume_db( float( m_volumeDb ) );
	player->set_pitch_scale( float( std::max( 0.01, m_pitchScale + UtilityFunctions::randf_range( -m_pitchJitter, m_pitchJitter ) ) ) );
	if ( m_bus.is_empty() == false )
	{
		player->set_bus( m_bus );
	}
	if ( m_maxDistance > 0.0 )
	{
		player->set_max_distance( float( m_maxDistance ) );
	}
	parent->add_child( player );
	if ( place != nullptr && place->global )
	{
		player->set_global_position( place->transform.origin );
	}
	player->connect( "finished", Callable( player, "queue_free" ) );
	player->play();
}

void CbReaction::Act( bool on, Node* root, Node* limit, const Placement* place )
{
	bool event = m_when == WHEN_EVENT;
	bool refused = Refused( m_method, m_property );

	// Animation: the one for on; for a "while" ending, the off one (where the on one played).
	String animation = on ? m_animation : m_animationOff;
	auto* player = on ? Object::cast_to<AnimationPlayer>( Find( m_player, root, limit ) )
					  : Object::cast_to<AnimationPlayer>( ObjectDB::get_instance( m_onPlayer ) );
	if ( on && event == false )
	{
		m_onPlayer = player != nullptr ? player->get_instance_id() : ObjectID();
	}
	if ( player != nullptr && animation.is_empty() == false && player->has_animation( animation ) )
	{
		player->stop();
		player->play( animation );
	}

	// Property: set it; a "while" puts back what was there when it ends (on the node it set).
	Node* target = on ? Find( m_target, root, limit ) : Object::cast_to<Node>( ObjectDB::get_instance( m_onTarget ) );
	if ( target != nullptr && m_property.is_empty() == false && refused == false )
	{
		NodePath path = NodePath( m_property ).get_as_property_path();
		if ( on )
		{
			if ( event == false && m_haveOriginal == false )
			{
				m_original = target->get_indexed( path );
				m_haveOriginal = true;
				m_onTarget = target->get_instance_id();
			}
			target->set_indexed( path, m_value );
		}
		else if ( m_haveOriginal )
		{
			target->set_indexed( path, m_original );
		}
	}
	if ( on == false )
	{
		m_haveOriginal = false;
		m_onTarget = ObjectID();
	}
	// Method: built-in methods with no arguments only ("restart", "play", "show").
	if ( on && target != nullptr && m_method.is_empty() == false && refused == false && target->has_method( m_method ) )
	{
		target->call( m_method );
	}

	// Scene: added under the parent (this reaction's parent, or the acted-on entity's root).
	if ( m_scene.is_empty() == false )
	{
		if ( on )
		{
			Ref<PackedScene> packed = ResourceLoader::get_singleton()->load( m_scene, "PackedScene" );
			Node* parent = m_sceneParent.is_empty() ? ( root != nullptr ? root : get_parent() ) : Find( m_sceneParent, root, limit );
			if ( place != nullptr && place->parent != nullptr )
			{
				parent = place->parent;
			}
			Node* spawned = packed.is_valid() && parent != nullptr ? packed->instantiate() : nullptr;
			if ( spawned != nullptr )
			{
				parent->add_child( spawned );
				if ( auto* spatial = Object::cast_to<Node3D>( spawned ) )
				{
					if ( place != nullptr && place->global )
					{
						spatial->set_global_transform( place->transform );
					}
					else if ( place != nullptr )
					{
						spatial->set_position( m_offset );
					}
				}
				// One-shot particles start over, so a pooled or cached scene still bursts.
				TypedArray<Node> particles = spawned->find_children( "*", "GPUParticles3D", true, false );
				if ( auto* self = Object::cast_to<GPUParticles3D>( spawned ) )
				{
					particles.push_back( self );
				}
				for ( int64_t i = 0; i < particles.size(); ++i )
				{
					if ( auto* emitter = Object::cast_to<GPUParticles3D>( particles[i] ) )
					{
						emitter->restart();
					}
				}
				if ( event )
				{
					if ( m_sceneLifetime > 0.0 && is_inside_tree() )
					{
						Ref<SceneTreeTimer> timer = get_tree()->create_timer( m_sceneLifetime );
						timer->connect( "timeout", Callable( spawned, "queue_free" ) );
					}
				}
				else
				{
					m_spawned = spawned->get_instance_id();
				}
			}
		}
		else if ( auto* spawned = Object::cast_to<Node>( ObjectDB::get_instance( m_spawned ) ) )
		{
			spawned->queue_free();
			m_spawned = ObjectID();
		}
	}

	// Sound: once, where the scene would go.
	if ( on && m_sound.is_empty() == false )
	{
		Node* parent = place != nullptr && place->parent != nullptr ? place->parent : ( root != nullptr ? root : get_parent() );
		PlaySound( parent, place );
	}
}

void CbItemLook::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "set_kind", "value" ), &CbItemLook::set_kind );
	ClassDB::bind_method( D_METHOD( "get_kind" ), &CbItemLook::get_kind );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "kind", PROPERTY_HINT_PLACEHOLDER_TEXT, "melee.bat" ), "set_kind", "get_kind" );
	ClassDB::bind_method( D_METHOD( "set_scene", "value" ), &CbItemLook::set_scene );
	ClassDB::bind_method( D_METHOD( "get_scene" ), &CbItemLook::get_scene );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "scene", PROPERTY_HINT_FILE, "*.tscn,*.scn" ), "set_scene", "get_scene" );
}

} // namespace cb::gd
