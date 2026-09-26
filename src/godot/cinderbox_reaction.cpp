#include "cinderbox_reaction.h"

#include <godot_cpp/classes/animation_player.hpp>
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

#define CB_REACTION_PROP( type, name, hint, hintText )                                                                             \
	ClassDB::bind_method( D_METHOD( "set_" #name, "value" ), &CbReaction::set_##name );                                            \
	ClassDB::bind_method( D_METHOD( "get_" #name ), &CbReaction::get_##name );                                                     \
	ADD_PROPERTY( PropertyInfo( type, #name, hint, hintText ), "set_" #name, "get_" #name );

	ADD_GROUP( "When", "" );
	CB_REACTION_PROP( Variant::INT, when, PROPERTY_HINT_ENUM, "On event,While" )
	CB_REACTION_PROP( Variant::STRING, event, PROPERTY_HINT_PLACEHOLDER_TEXT, "melee.hit" )
	CB_REACTION_PROP( Variant::PACKED_STRING_ARRAY, conditions, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::INT, subject, PROPERTY_HINT_ENUM, "This entity,Its holder" )
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
#undef CB_REACTION_PROP

	BIND_ENUM_CONSTANT( WHEN_EVENT );
	BIND_ENUM_CONSTANT( WHEN_WHILE );
	BIND_ENUM_CONSTANT( SUBJECT_SELF );
	BIND_ENUM_CONSTANT( SUBJECT_HOLDER );
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
	if ( m_animation.is_empty() && m_property.is_empty() && m_method.is_empty() && m_scene.is_empty() )
	{
		warnings.push_back( "It does nothing yet: set an animation, a property, a method or a scene." );
	}
	if ( m_method.is_empty() == false && m_target.is_empty() )
	{
		warnings.push_back( "Calling a method needs a target node." );
	}
	return warnings;
}

void CbReaction::fire()
{
	Act( true );
}

void CbReaction::set_on( bool on )
{
	if ( on == m_on )
	{
		return;
	}
	m_on = on;
	Act( on );
}

void CbReaction::Act( bool on )
{
	bool event = m_when == WHEN_EVENT;

	// Animation: the one for on; for a "while" ending, the off one if there is one.
	String animation = on ? m_animation : m_animationOff;
	if ( animation.is_empty() == false )
	{
		if ( auto* player = Object::cast_to<AnimationPlayer>( get_node_or_null( m_player ) ); player != nullptr && player->has_animation( animation ) )
		{
			player->stop();
			player->play( animation );
		}
	}

	Node* target = m_target.is_empty() ? nullptr : get_node_or_null( m_target );
	// Property: set it; a "while" puts back what was there when it ends.
	if ( target != nullptr && m_property.is_empty() == false )
	{
		NodePath path = NodePath( m_property ).get_as_property_path();
		if ( on )
		{
			if ( event == false && m_haveOriginal == false )
			{
				m_original = target->get_indexed( path );
				m_haveOriginal = true;
			}
			target->set_indexed( path, m_value );
		}
		else if ( m_haveOriginal )
		{
			target->set_indexed( path, m_original );
			m_haveOriginal = false;
		}
	}
	// Method: built-in methods with no arguments only ("restart", "play", "show").
	if ( on && target != nullptr && m_method.is_empty() == false && target->has_method( m_method ) )
	{
		target->call( m_method );
	}

	// Scene: added under the parent (this reaction's parent by default).
	if ( m_scene.is_empty() == false )
	{
		if ( on )
		{
			Ref<PackedScene> packed = ResourceLoader::get_singleton()->load( m_scene, "PackedScene" );
			Node* parent = m_sceneParent.is_empty() ? get_parent() : get_node_or_null( m_sceneParent );
			Node* spawned = packed.is_valid() && parent != nullptr ? packed->instantiate() : nullptr;
			if ( spawned != nullptr )
			{
				parent->add_child( spawned );
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
}

} // namespace cb::gd
