#include "cinderbox_reaction.h"

#include "entity_path.h"

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
#undef CB_REACTION_PROP

	BIND_ENUM_CONSTANT( WHEN_EVENT );
	BIND_ENUM_CONSTANT( WHEN_WHILE );
	BIND_ENUM_CONSTANT( SIDE_A );
	BIND_ENUM_CONSTANT( SIDE_B );
	BIND_ENUM_CONSTANT( SIDE_EITHER );
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
	Act( true, root, limit );
}

void CbReaction::SetOnIn( bool on, Node* root, Node* limit )
{
	if ( on == m_on )
	{
		return;
	}
	m_on = on;
	Act( on, root, limit );
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

void CbReaction::Act( bool on, Node* root, Node* limit )
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
