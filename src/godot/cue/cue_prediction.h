#pragma once

// A prediction: the viewer's own key press shown at once, as the cue the server will send for it.
//
// A mod's rules run on the server, so what a press did comes back a round trip later. A mod's look
// can say, as data, what its own server is going to answer:
//
//   CbPrediction   action "fire"   cue "pistol.fired"   conditions pistol.gun, pistol.ammo > 0
//
// When the viewer presses the action while the conditions hold on its own entity, the director
// plays that cue for it there and then (CbDirector.press), with the same reactions the server's
// cue plays: one reaction per cue, no second one for the local player. When the server's cue of
// that name arrives for the viewer, it is the echo of what was already shown: only the reactions
// that had to wait for it play (the ones that use what only the server knows: the cue's point,
// end, value or other entity). A cue that was not predicted plays in full, as ever.
//
// Both halves are the modder's: the server mod emits the cue, the look predicts it by the same
// name. The conditions are the look's guess of the server's rule; where the guess is wrong, a
// reaction played that should not have (it is not taken back), or the cue simply plays late.
// Looks only: nothing here changes the game.

#include "cue_paths.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

namespace cb::gd
{

class CbDirector;

class CbPrediction : public godot::Node
{
	GDCLASS( CbPrediction, godot::Node )

public:
	// For the director: why a press of `action` now would not predict this cue, or empty when it
	// would. Accepts() is the press itself: when it would, the cooldown starts.
	godot::String Refusal( const godot::String& action, const cue::Context& context, double now ) const;
	bool Accepts( const godot::String& action, const cue::Context& context, double now );
	const godot::String& Cue() const
	{
		return m_cue;
	}
	const godot::String& Action() const
	{
		return m_action;
	}

	void set_action( const godot::String& v )
	{
		m_action = v;
		update_configuration_warnings();
	}
	godot::String get_action() const
	{
		return m_action;
	}
	void set_cue( const godot::String& v )
	{
		m_cue = v;
		update_configuration_warnings();
	}
	godot::String get_cue() const
	{
		return m_cue;
	}
	void set_conditions( const godot::PackedStringArray& v )
	{
		m_conditions = v;
		m_parsed = false;
		update_configuration_warnings();
	}
	godot::PackedStringArray get_conditions() const
	{
		return m_conditions;
	}
	void set_cooldown( double v )
	{
		m_cooldown = v;
	}
	double get_cooldown() const
	{
		return m_cooldown;
	}

	void _notification( int what );
	godot::PackedStringArray _get_configuration_warnings() const override;

protected:
	static void _bind_methods();

private:
	bool Parse() const;

	godot::String m_action;
	godot::String m_cue;
	godot::PackedStringArray m_conditions;
	double m_cooldown = 0.0;

	CbDirector* m_director = nullptr; // while in its tree
	mutable bool m_parsed = false;
	mutable bool m_valid = false;
	mutable std::vector<cue::Condition> m_tests;
	double m_lastPredicted = -1e9;
};

} // namespace cb::gd
