#include "cinderbox_animator.h"

#include "anim_controller.h"
#include "components.h"

#include <godot_cpp/classes/animation_node_state_machine_playback.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;

namespace cb::gd
{

void CinderboxAnimator::_bind_methods()
{
	ClassDB::bind_method( D_METHOD( "apply_state", "mode", "mode_time", "locomotion_phase", "ground_speed" ),
						  &CinderboxAnimator::apply_state );
	ClassDB::bind_method( D_METHOD( "get_current_state" ), &CinderboxAnimator::get_current_state );
	ClassDB::bind_method( D_METHOD( "set_animation_tree_path", "path" ), &CinderboxAnimator::set_animation_tree_path );
	ClassDB::bind_method( D_METHOD( "get_animation_tree_path" ), &CinderboxAnimator::get_animation_tree_path );
	ClassDB::bind_method( D_METHOD( "set_state_machine", "value" ), &CinderboxAnimator::set_state_machine );
	ClassDB::bind_method( D_METHOD( "get_state_machine" ), &CinderboxAnimator::get_state_machine );
	ClassDB::bind_method( D_METHOD( "set_speed_parameter", "value" ), &CinderboxAnimator::set_speed_parameter );
	ClassDB::bind_method( D_METHOD( "get_speed_parameter" ), &CinderboxAnimator::get_speed_parameter );
	ClassDB::bind_method( D_METHOD( "set_locomotion_state", "value" ), &CinderboxAnimator::set_locomotion_state );
	ClassDB::bind_method( D_METHOD( "get_locomotion_state" ), &CinderboxAnimator::get_locomotion_state );
	ClassDB::bind_method( D_METHOD( "set_jump_state", "value" ), &CinderboxAnimator::set_jump_state );
	ClassDB::bind_method( D_METHOD( "get_jump_state" ), &CinderboxAnimator::get_jump_state );
	ClassDB::bind_method( D_METHOD( "set_fall_state", "value" ), &CinderboxAnimator::set_fall_state );
	ClassDB::bind_method( D_METHOD( "get_fall_state" ), &CinderboxAnimator::get_fall_state );
	ClassDB::bind_method( D_METHOD( "set_land_state", "value" ), &CinderboxAnimator::set_land_state );
	ClassDB::bind_method( D_METHOD( "get_land_state" ), &CinderboxAnimator::get_land_state );
	ClassDB::bind_method( D_METHOD( "set_sync_threshold", "value" ), &CinderboxAnimator::set_sync_threshold );
	ClassDB::bind_method( D_METHOD( "get_sync_threshold" ), &CinderboxAnimator::get_sync_threshold );

	ADD_PROPERTY( PropertyInfo( Variant::NODE_PATH, "animation_tree_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "AnimationTree" ),
				  "set_animation_tree_path", "get_animation_tree_path" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "state_machine" ), "set_state_machine", "get_state_machine" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "speed_parameter" ), "set_speed_parameter", "get_speed_parameter" );
	ADD_GROUP( "States", "" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "locomotion_state" ), "set_locomotion_state", "get_locomotion_state" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "jump_state" ), "set_jump_state", "get_jump_state" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "fall_state" ), "set_fall_state", "get_fall_state" );
	ADD_PROPERTY( PropertyInfo( Variant::STRING, "land_state" ), "set_land_state", "get_land_state" );
	ADD_GROUP( "", "" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "sync_threshold", PROPERTY_HINT_RANGE, "0,1,0.01" ), "set_sync_threshold",
				  "get_sync_threshold" );
}

void CinderboxAnimator::_ready()
{
	// The tree drives itself off the simulation, not off Godot's clock, so it is advanced by hand.
	if ( AnimationTree* tree = Tree() )
	{
		tree->set_callback_mode_process( AnimationMixer::ANIMATION_CALLBACK_MODE_PROCESS_MANUAL );
	}
}

void CinderboxAnimator::set_animation_tree_path( const NodePath& path )
{
	m_treePath = path;
	m_tree = ObjectID();
}

AnimationTree* CinderboxAnimator::Tree()
{
	if ( auto* cached = Object::cast_to<AnimationTree>( ObjectDB::get_instance( m_tree ) ) )
	{
		return cached;
	}
	if ( is_inside_tree() == false || m_treePath.is_empty() )
	{
		return nullptr;
	}
	auto* tree = Object::cast_to<AnimationTree>( get_node_or_null( m_treePath ) );
	if ( tree == nullptr )
	{
		if ( m_warned == false )
		{
			m_warned = true;
			UtilityFunctions::push_warning( "CinderboxAnimator: no AnimationTree at ", m_treePath );
		}
		return nullptr;
	}
	m_tree = tree->get_instance_id();
	return tree;
}

String CinderboxAnimator::StateFor( int mode ) const
{
	switch ( AnimMode( mode ) )
	{
		case AnimMode::JumpStart:
			return m_jumpState;
		case AnimMode::Fall:
			return m_fallState;
		case AnimMode::Land:
			return m_landState;
		case AnimMode::Locomotion:
		default:
			return m_locomotionState;
	}
}

float CinderboxAnimator::ExpectedTime( const AnimState& state ) const
{
	if ( state.mode != AnimMode::Locomotion )
	{
		// One-shot modes are simply however long the simulation has been in them.
		return state.modeTime;
	}
	// Walk and run share one phase so their feet line up; turning it back into seconds needs the
	// cycle length of whichever clip the blend is mostly playing.
	float cycle = state.groundSpeed > anim_tuning::kWalkSpeed ? anim_tuning::kRunCycleSeconds : anim_tuning::kWalkCycleSeconds;
	return state.locomotionPhase * cycle;
}

void CinderboxAnimator::apply_state( int mode, float mode_time, float locomotion_phase, float ground_speed )
{
	AnimState state;
	state.mode = AnimMode( std::min( std::max( mode, 0 ), int( AnimMode::Land ) ) );
	state.modeTime = mode_time;
	state.locomotionPhase = locomotion_phase;
	state.groundSpeed = ground_speed;
	ApplyState( state );
}

String CinderboxAnimator::get_current_state() const
{
	auto* tree = Object::cast_to<AnimationTree>( ObjectDB::get_instance( m_tree ) );
	if ( tree == nullptr )
	{
		return String();
	}
	Ref<AnimationNodeStateMachinePlayback> playback = tree->get( m_stateMachine );
	if ( playback.is_valid() == false )
	{
		return String();
	}
	return String( playback->get_current_node() );
}

void CinderboxAnimator::ApplyState( const AnimState& state )
{
	AnimationTree* tree = Tree();
	if ( tree == nullptr )
	{
		return;
	}

	// The 1D blend is in metres per second, the same units the simulation smooths.
	if ( m_speedParameter.is_empty() == false )
	{
		tree->set( m_speedParameter, state.groundSpeed );
	}

	Ref<AnimationNodeStateMachinePlayback> playback = tree->get( m_stateMachine );
	int mode = int( state.mode );
	bool changed = mode != m_lastMode;
	m_lastMode = mode;

	if ( playback.is_valid() == false )
	{
		// No state machine: the blend parameter alone is still useful.
		tree->advance( double( get_process_delta_time() ) );
		return;
	}

	String want = StateFor( mode );
	if ( changed || playback->get_current_node() != want )
	{
		// travel() takes the transitions an author set up; start() would ignore them.
		playback->travel( want );
	}

	float expected = ExpectedTime( state );
	float elapsed = 0.0f;
	if ( m_syncThreshold > 0.0f && playback->get_current_node() == want )
	{
		float current = float( playback->get_current_play_position() );
		float drift = expected - current;
		// A clip that loops makes a late joiner look far behind when it is really just wrapped.
		float length = float( playback->get_current_length() );
		if ( length > 0.0f && state.mode == AnimMode::Locomotion )
		{
			drift = std::fmod( drift + length * 1.5f, length ) - length * 0.5f;
		}
		if ( std::fabs( drift ) > m_syncThreshold )
		{
			// Catching up by advancing keeps the transition and its blending intact, which
			// restarting the state would throw away.
			elapsed = drift > 0.0f ? drift : 0.0f;
		}
	}

	tree->advance( double( get_process_delta_time() + elapsed ) );
}

} // namespace cb::gd
