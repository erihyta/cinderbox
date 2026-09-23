#pragma once

// Drives a Godot AnimationTree from the simulation's animation state.
//
// The simulation already decides what a character is doing and when: the mode (locomotion, jump,
// fall, land), how long it has been in it, its smoothed ground speed, and a locomotion phase that
// every client agrees on. CinderboxSkeleton turns that into a pose with ozz. This node instead
// hands it to Godot's own animation system, so a character can be built the way a Godot artist
// expects — imported clips, a blend space, a state machine, retargeting, IK — while its timing
// still comes from the simulation and stays the same on every machine.
//
// Put one in a player prefab next to an AnimationTree and point it at the tree. Nothing here runs
// on the server, and a mod can ship a prefab that uses it without any code.

#include <godot_cpp/classes/animation_tree.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace cb
{
struct AnimState;
}

namespace cb::gd
{

class CinderboxAnimator : public godot::Node3D
{
	GDCLASS( CinderboxAnimator, godot::Node3D )

public:
	void _ready() override;

	// Called by CinderboxClient every frame with the simulation's state for this character.
	void ApplyState( const AnimState& state );

	// The same thing from a script, for previewing a character or driving one by hand:
	//   mode 0 locomotion, 1 jump, 2 fall, 3 land.
	void apply_state( int mode, float mode_time, float locomotion_phase, float ground_speed );
	// Name of the state the tree is playing, "" when it has no state machine.
	godot::String get_current_state() const;

	// A server mod announced something about this character ("pistol.fired"). If event_parameters
	// names a parameter for it, that parameter is fired: set to 1, which is what an
	// AnimationNodeOneShot's request takes. This is how a shot plays a recoil clip with no code.
	void on_mod_event( const godot::String& name );
	// Sets one tree parameter, if the tree exists (used by state bindings).
	void set_tree_parameter( const godot::String& path, const godot::Variant& value );

	void set_event_parameters( const godot::Dictionary& value )
	{
		m_eventParameters = value;
	}
	godot::Dictionary get_event_parameters() const
	{
		return m_eventParameters;
	}

	void set_animation_tree_path( const godot::NodePath& path );
	godot::NodePath get_animation_tree_path() const
	{
		return m_treePath;
	}
	void set_state_machine( const godot::String& value )
	{
		m_stateMachine = value;
	}
	godot::String get_state_machine() const
	{
		return m_stateMachine;
	}
	void set_speed_parameter( const godot::String& value )
	{
		m_speedParameter = value;
	}
	godot::String get_speed_parameter() const
	{
		return m_speedParameter;
	}
	void set_locomotion_state( const godot::String& value )
	{
		m_locomotionState = value;
	}
	godot::String get_locomotion_state() const
	{
		return m_locomotionState;
	}
	void set_jump_state( const godot::String& value )
	{
		m_jumpState = value;
	}
	godot::String get_jump_state() const
	{
		return m_jumpState;
	}
	void set_fall_state( const godot::String& value )
	{
		m_fallState = value;
	}
	godot::String get_fall_state() const
	{
		return m_fallState;
	}
	void set_land_state( const godot::String& value )
	{
		m_landState = value;
	}
	godot::String get_land_state() const
	{
		return m_landState;
	}
	void set_sync_threshold( float value )
	{
		m_syncThreshold = value;
	}
	float get_sync_threshold() const
	{
		return m_syncThreshold;
	}

protected:
	static void _bind_methods();

private:
	godot::AnimationTree* Tree();
	godot::String StateFor( int mode ) const;
	// Where the simulation says this character should be inside its current clip, in seconds.
	float ExpectedTime( const AnimState& state ) const;

	godot::NodePath m_treePath = godot::NodePath( ".." );
	// The state machine's parameter prefix and the states it contains.
	godot::String m_stateMachine = "parameters/playback";
	godot::String m_speedParameter = "parameters/locomotion/blend_position";
	godot::String m_locomotionState = "locomotion";
	godot::String m_jumpState = "jump";
	godot::String m_fallState = "fall";
	godot::String m_landState = "land";
	// Resync when the tree drifts this far from the simulation, in seconds. 0 never resyncs, which
	// leaves Godot to play the clips at its own pace.
	float m_syncThreshold = 0.12f;

	// Mod event name -> AnimationTree parameter to fire.
	godot::Dictionary m_eventParameters;

	godot::ObjectID m_tree;
	int m_lastMode = -1;
	bool m_warned = false;
};

} // namespace cb::gd
