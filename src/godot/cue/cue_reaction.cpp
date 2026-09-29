#include "cue_reaction.h"

#include "cue_director.h"

#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/gpu_particles3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/scene_tree_timer.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

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
	CB_REACTION_PROP( Variant::INT, when, PROPERTY_HINT_ENUM, "On a cue,While" )
	CB_REACTION_PROP( Variant::STRING, event, PROPERTY_HINT_PLACEHOLDER_TEXT, "melee.hit, footstep, pressed:fire" )
	CB_REACTION_PROP( Variant::INT, event_side, PROPERTY_HINT_ENUM, "A: the cue is at the subject,B: the subject is the other one,Either" )
	CB_REACTION_PROP( Variant::NODE_PATH, subject, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::STRING, subject_kind, PROPERTY_HINT_ENUM_SUGGESTION, "any,player,prop,static,ragdoll,item" )
	CB_REACTION_PROP( Variant::STRING, subject_template, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::PACKED_STRING_ARRAY, conditions, PROPERTY_HINT_NONE, "" )
	CB_REACTION_PROP( Variant::FLOAT, cooldown, PROPERTY_HINT_RANGE, "0,10,0.01,suffix:s" )
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
	CB_REACTION_PROP( Variant::INT, place, PROPERTY_HINT_ENUM, "Under its parent,Cue point,Cue end,Beam (to the cue end),At place_node,Follow the subject" )
	CB_REACTION_PROP( Variant::NODE_PATH, place_node, PROPERTY_HINT_NONE, "" )
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
	BIND_ENUM_CONSTANT( PLACE_NODE );
	BIND_ENUM_CONSTANT( PLACE_FOLLOW );
}

void CbReaction::_notification( int what )
{
	if ( what == NOTIFICATION_ENTER_TREE )
	{
		for ( Node* n = get_parent(); n != nullptr; n = n->get_parent() )
		{
			if ( auto* director = Object::cast_to<CbDirector>( n ) )
			{
				m_director = director;
				director->Register( this );
				break;
			}
		}
	}
	else if ( what == NOTIFICATION_EXIT_TREE )
	{
		if ( m_director != nullptr )
		{
			m_director->Unregister( this );
			m_director = nullptr;
		}
		m_on = false;
		m_haveOriginal = false;
	}
}

void CbReaction::Changed()
{
	m_parsed = false;
	if ( m_director != nullptr )
	{
		m_director->Reindex();
	}
}

bool CbReaction::Parse()
{
	if ( m_parsed )
	{
		return m_valid;
	}
	m_parsed = true;
	m_tests.clear();
	m_valid = true;
	for ( const NodePath& path : { m_subject, m_player, m_target, m_sceneParent, m_placeNode } )
	{
		m_valid &= cue::CheckPath( path, nullptr );
	}
	for ( int64_t i = 0; i < m_conditions.size(); ++i )
	{
		cue::Condition condition;
		m_valid &= cue::ParseCondition( m_conditions[i], condition, nullptr );
		m_tests.push_back( condition );
	}
	return m_valid;
}

PackedStringArray CbReaction::_get_configuration_warnings() const
{
	PackedStringArray warnings;
	if ( m_when == WHEN_EVENT && m_event.strip_edges().is_empty() )
	{
		warnings.push_back( "Name the cue this reacts to (a mod's event like melee.hit, or footstep, pressed:fire)." );
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
	if ( m_method.is_empty() == false && m_target.is_empty() )
	{
		warnings.push_back( "Calling a method needs a target node." );
	}
	if ( Refused( m_method, m_property ) )
	{
		warnings.push_back( "The game refuses \"free\", \"queue_free\" and \"script\"." );
	}
	if ( m_when == WHEN_WHILE && m_place != PLACE_PARENT && m_place != PLACE_FOLLOW && m_place != PLACE_NODE )
	{
		warnings.push_back( "A \"while\" has no cue to place things at: use Under its parent, At place_node or Follow the subject." );
	}
	String error;
	const char* names[] = { "Subject", "Animation player", "Target", "Scene parent", "Place node" };
	const NodePath paths[] = { m_subject, m_player, m_target, m_sceneParent, m_placeNode };
	for ( int i = 0; i < 5; ++i )
	{
		if ( cue::CheckPath( paths[i], &error ) == false )
		{
			warnings.push_back( String( names[i] ) + ": " + error );
		}
	}
	for ( int64_t i = 0; i < m_conditions.size(); ++i )
	{
		cue::Condition condition;
		if ( cue::ParseCondition( m_conditions[i], condition, &error ) == false )
		{
			warnings.push_back( "Condition \"" + m_conditions[i] + "\": " + error );
		}
	}
	return warnings;
}

bool CbReaction::Refused( const String& method, const String& property )
{
	return method == "free" || method == "queue_free" || property == "script" || property.begins_with( "script:" );
}

Node* CbReaction::Subject( const cue::Context& context ) const
{
	static const NodePath kOwnEntity( "^" );
	return cue::Resolve( m_subject.is_empty() ? kOwnEntity : m_subject, const_cast<CbReaction*>( this ), context );
}

bool CbReaction::Holds( const cue::Context& context, Node* subject ) const
{
	// Kind and template: what the director was told about the subject's entity.
	Node* entity = cue::EntityOf( subject, context.director );
	if ( m_subjectKind.is_empty() == false && m_subjectKind != "any" &&
		 ( entity == nullptr || String( entity->get_meta( cue::kKindMeta, "" ) ) != m_subjectKind ) )
	{
		return false;
	}
	if ( m_subjectTemplate.is_empty() == false &&
		 ( entity == nullptr || String( entity->get_meta( cue::kTemplateMeta, "" ) ) != m_subjectTemplate ) )
	{
		return false;
	}
	for ( const cue::Condition& c : m_tests )
	{
		Node* whose = entity;
		if ( c.hasPath )
		{
			Node* found = cue::Resolve( c.path, const_cast<CbReaction*>( this ), context );
			if ( found == nullptr )
			{
				return false;
			}
			whose = found == context.director ? nullptr : cue::EntityOf( found, context.director );
		}
		auto lookup = [&]( const String& name, Variant& out ) { return cue::LookUp( name, whose, context, out ); };
		if ( cue::Test( c.test, lookup ) == false )
		{
			return false;
		}
	}
	return true;
}

bool CbReaction::Fire( const cue::Context& context, double now )
{
	if ( m_when != WHEN_EVENT || Parse() == false )
	{
		return false;
	}
	Node* subject = Subject( context );
	if ( subject == nullptr )
	{
		return false;
	}
	// The cue names the subject on this reaction's side (a subject from the cue: any cue).
	bool atA = context.at != nullptr && subject == context.at;
	bool atB = context.other != nullptr && subject == context.other;
	bool named = m_eventSide == SIDE_A ? atA : m_eventSide == SIDE_B ? atB : ( atA || atB );
	if ( ( cue::FromCue( m_subject ) == false && named == false ) || Holds( context, subject ) == false )
	{
		return false;
	}
	// Keeps a busy cue (twenty props at once) from stacking twenty sounds.
	if ( m_cooldown > 0.0 && now - m_lastFired < m_cooldown )
	{
		return false;
	}
	m_lastFired = now;
	Act( true, context );
	return true;
}

void CbReaction::Update( const cue::Context& context )
{
	if ( m_when != WHEN_WHILE || Parse() == false )
	{
		return;
	}
	Node* subject = Subject( context );
	bool on = subject != nullptr && Holds( context, subject );
	// Still on, but what it acts on is another node now (another item in the hand): end there first.
	if ( on && m_on )
	{
		Node* target = cue::Resolve( m_target, this, context );
		Node* player = cue::Resolve( m_player, this, context );
		bool moved = ( m_haveOriginal && ( target == nullptr || ObjectID( target->get_instance_id() ) != m_onTarget ) ) ||
					 ( m_onPlayer.is_valid() && ( player == nullptr || ObjectID( player->get_instance_id() ) != m_onPlayer ) );
		if ( moved )
		{
			m_on = false;
			Act( false, context );
		}
	}
	if ( on != m_on )
	{
		m_on = on;
		Act( on, context );
	}
}

void CbReaction::fire()
{
	cue::Context context;
	context.director = m_director;
	context.local = m_director != nullptr ? m_director->get_local() : nullptr;
	Act( true, context );
}

void CbReaction::set_on( bool on )
{
	if ( on == m_on )
	{
		return;
	}
	cue::Context context;
	context.director = m_director;
	context.local = m_director != nullptr ? m_director->get_local() : nullptr;
	m_on = on;
	Act( on, context );
}

void CbReaction::PlaySound( Node* parent, bool global, const Vector3& where )
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
	if ( global )
	{
		player->set_global_position( where );
	}
	else
	{
		player->set_position( m_offset );
	}
	player->connect( "finished", Callable( player, "queue_free" ) );
	player->play();
}

void CbReaction::Act( bool on, const cue::Context& context )
{
	bool event = m_when == WHEN_EVENT;
	bool refused = Refused( m_method, m_property );

	// Animation: the one for on; for a "while" ending, the off one (where the on one played).
	String animation = on ? m_animation : m_animationOff;
	auto* player = on ? Object::cast_to<AnimationPlayer>( cue::Resolve( m_player, this, context ) )
					  : Object::cast_to<AnimationPlayer>( ObjectDB::get_instance( m_onPlayer ) );
	if ( on && event == false )
	{
		m_onPlayer = player != nullptr ? ObjectID( player->get_instance_id() ) : ObjectID();
	}
	if ( on == false )
	{
		m_onPlayer = ObjectID();
	}
	if ( player != nullptr && animation.is_empty() == false && player->has_animation( animation ) )
	{
		player->stop();
		player->play( animation );
	}

	// Property: set it; a "while" puts back what was there when it ends (on the node it set).
	Node* target = on ? cue::Resolve( m_target, this, context ) : Object::cast_to<Node>( ObjectDB::get_instance( m_onTarget ) );
	if ( target != nullptr && m_property.is_empty() == false && refused == false )
	{
		NodePath path = NodePath( m_property ).get_as_property_path();
		if ( on )
		{
			if ( event == false && m_haveOriginal == false )
			{
				m_original = target->get_indexed( path );
				m_haveOriginal = true;
				m_onTarget = ObjectID( target->get_instance_id() );
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

	// Where a scene and a sound go.
	Node* parent = m_sceneParent.is_empty() ? get_parent() : cue::Resolve( m_sceneParent, this, context );
	bool global = false;
	Transform3D where;
	auto nodeAt = [&]( const NodePath& path ) -> Node3D* { return Object::cast_to<Node3D>( cue::Resolve( path, this, context ) ); };
	switch ( m_place )
	{
		case PLACE_EVENT_POINT:
		case PLACE_EVENT_END:
			global = context.event;
			where.origin = ( m_place == PLACE_EVENT_END ? context.end : context.point ) + m_offset;
			break;
		case PLACE_NODE:
			if ( Node3D* at = nodeAt( m_placeNode ) )
			{
				global = true;
				where.origin = at->get_global_position() + m_offset;
			}
			break;
		case PLACE_BEAM:
		{
			// A one-metre scene along its -Z, stretched from place_node (or the cue point) to the end.
			Node3D* at = nodeAt( m_placeNode );
			Vector3 from = ( at != nullptr ? at->get_global_position() : context.point ) + m_offset;
			Vector3 along = context.end - from;
			real_t length = along.length();
			if ( length > 0.01f )
			{
				Vector3 up = Math::abs( along.normalized().y ) < 0.99f ? Vector3( 0, 1, 0 ) : Vector3( 1, 0, 0 );
				where.basis = Basis::looking_at( along, up ).scaled_local( Vector3( 1, 1, length ) );
			}
			where.origin = from;
			global = context.event;
			break;
		}
		case PLACE_FOLLOW:
			parent = Subject( context );
			break;
		case PLACE_PARENT:
		default:
			break;
	}
	if ( global )
	{
		parent = context.director != nullptr ? context.director : parent; // stays where it happened
	}

	// Scene: added under the parent; a cue's goes after its lifetime, a "while"'s when it ends.
	if ( m_scene.is_empty() == false )
	{
		if ( on )
		{
			Ref<PackedScene> packed = ResourceLoader::get_singleton()->load( m_scene, "PackedScene" );
			Node* spawned = packed.is_valid() && parent != nullptr ? packed->instantiate() : nullptr;
			if ( spawned != nullptr )
			{
				parent->add_child( spawned );
				if ( auto* spatial = Object::cast_to<Node3D>( spawned ) )
				{
					if ( global )
					{
						spatial->set_global_transform( where );
					}
					else
					{
						spatial->set_position( m_offset );
					}
				}
				// One-shot particles start over, so the burst plays.
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
		PlaySound( parent, global, where.origin );
	}
}

} // namespace cb::gd
