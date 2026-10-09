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
// A prediction can also say what the server's answer will change for the viewer's own player, so
// that shows at once too, until the server's cue comes or the prediction expires (a second):
//
//   changes        "pistol.ammo -= 1"   a field of the viewer's player: the HUD and conditions
//                                        read the changed value (-=, += and = with a number)
//   stance         "melee_swing" on stance_layer "full"   what the server's mod will set: the
//                                        character's state machine starts its swing now
//
// An action that works for as long as it is held (automatic fire) is predicted the same way, again
// and again:
//
//   while_held     with cooldown 0.1: the cue is predicted on the press and then every 0.1 s while
//                  the action stays down and the conditions hold, the rate the server's mod fires at
//
// The cue itself reaches the state machine as well (a recoil state entered on "pistol.fired").
// The host of the director applies these (the Cinderbox viewer does; see present/anim_lead.h).
//
// Where it sits says whose it is. Under the World (a mod's reactions scene) it speaks for the
// viewer whatever it holds, and its conditions say when ("pistol.gun"). Inside an item's own
// scene it speaks only for the copy of that item the viewer holds, while it is in use (the
// item's state "in_use"): no condition has to name the item, and a hundred pistols in the
// world are one prediction.
//
// Both halves are the modder's: the server mod emits the cue, the look predicts it by the same
// name. The conditions are the look's guess of the server's rule; where the guess is wrong, a
// reaction played that should not have (it is not taken back), or the cue simply plays late;
// changes and stances are put back when the prediction expires. Looks only: nothing here changes
// the game.

#include "cue_paths.h"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

namespace cb::cue
{

// A predicted change of a field: "pistol.ammo -= 1", "pistol.reloading = 1".
struct Change
{
	godot::String field;
	char op = '='; // '-', '+' or '='
	double value = 0.0;
};
bool ParseChange( const godot::String& text, Change& out, godot::String* error = nullptr );

} // namespace cb::cue

namespace cb::gd
{

class CbDirector;

class CbPrediction : public godot::Node
{
	GDCLASS( CbPrediction, godot::Node )

public:
	// For the director: why a press of `action` now would not predict this cue, or empty when it
	// would. Accepts() is the press itself: when it would, the cooldown starts. `held` is the
	// action still down from an earlier press: only a while_held prediction takes that.
	godot::String Refusal( const godot::String& action, const cue::Context& context, double now, bool held = false ) const;
	bool Accepts( const godot::String& action, const cue::Context& context, double now, bool held = false );
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
	void set_while_held( bool v )
	{
		m_whileHeld = v;
		update_configuration_warnings();
	}
	bool get_while_held() const
	{
		return m_whileHeld;
	}
	void set_changes( const godot::PackedStringArray& v )
	{
		m_changes = v;
		update_configuration_warnings();
	}
	godot::PackedStringArray get_changes() const
	{
		return m_changes;
	}
	void set_stance( const godot::String& v )
	{
		m_stance = v;
	}
	godot::String get_stance() const
	{
		return m_stance;
	}
	void set_stance_layer( const godot::String& v )
	{
		m_stanceLayer = v;
	}
	godot::String get_stance_layer() const
	{
		return m_stanceLayer;
	}

	void _notification( int what );
	godot::PackedStringArray _get_configuration_warnings() const override;
	void _validate_property( godot::PropertyInfo& property ) const;

protected:
	static void _bind_methods();

private:
	bool Parse() const;

	godot::String m_action;
	godot::String m_cue;
	godot::PackedStringArray m_conditions;
	double m_cooldown = 0.0;
	bool m_whileHeld = false;
	godot::PackedStringArray m_changes;
	godot::String m_stance;
	godot::String m_stanceLayer;

	CbDirector* m_director = nullptr; // while in its tree
	mutable bool m_parsed = false;
	mutable bool m_valid = false;
	mutable std::vector<cue::Condition> m_tests;
	double m_lastPredicted = -1e9;
};

} // namespace cb::gd
