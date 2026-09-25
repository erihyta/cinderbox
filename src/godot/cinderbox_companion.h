#pragma once

// Companion tracks: everything a character's animations do besides moving bones (particles, lights,
// materials, sounds, props, a weapon's own animation), played in step with the simulation's pose.
//
// The bake splits each animation: bone tracks become ozz clips (the pose the server hit-tests),
// every other track goes into companion.tres, an AnimationLibrary with the same clip names. At
// runtime a CbCompanionPlayer sits in the character and runs one hidden AnimationPlayer per channel
// (0: base locomotion, 1 + l: stance layer l), each set every frame to the clip and time the pose is
// playing, so a flame keyed at 0.13 s into the swing lights exactly then, on every screen.
//
// Rollback-safe: moving forward a little fires method and audio keys once; moving back, jumping far
// or switching clips only applies values (no key fires twice when a correction replays a moment).

#include <godot_cpp/classes/animation_library.hpp>
#include <godot_cpp/classes/animation_player.hpp>
#include <godot_cpp/classes/node.hpp>

#include <vector>

namespace cb::gd
{

// A clip's name in companion.tres. Animations from a named library ("mannequin/Swing") keep their
// name in the state machine, but a library cannot hold a '/' in a name.
inline godot::String CompanionName( const godot::String& clip )
{
	return clip.replace( "/", "." );
}

class CbCompanionPlayer : public godot::Node
{
	GDCLASS( CbCompanionPlayer, godot::Node )

public:
	// `root` is the node the tracks' paths start from (the character's AnimationPlayer's root node).
	void setup( const godot::Ref<godot::AnimationLibrary>& library, godot::Node* root );

	// Call begin_frame(), then play_at() for each playing clip, then end_frame(): channels not played
	// this frame go back to the library's RESET values.
	void begin_frame();
	void play_at( int channel, const godot::String& clip, double time, bool loops );
	void end_frame();

	// What a channel plays ("" when none), for tests and debugging.
	godot::String get_channel_clip( int channel ) const;

protected:
	static void _bind_methods();

private:
	struct Channel
	{
		godot::AnimationPlayer* player = nullptr;
		godot::String clip;
		double time = 0.0;
		double high = 0.0; // furthest time reached in this clip: keys up to here have fired
		bool touched = false;
	};

	Channel& ChannelAt( int channel );
	void Reset( Channel& channel );
	void Seek( Channel& channel, double time ); // values only, no keys fire

	godot::Ref<godot::AnimationLibrary> m_library;
	uint64_t m_root = 0;
	std::vector<Channel> m_channels;
	bool m_resetThisFrame = false;
};

} // namespace cb::gd
