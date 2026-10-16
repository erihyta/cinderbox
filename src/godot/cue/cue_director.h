#pragma once

// CbDirector: the World node. Everything reactions may see lives under it, and it runs them from
// what it is told. Plain Godot: anything can drive it (the Cinderbox client, a replay, a test, an
// editor preview), with four calls:
//
//   add_entity( node, kind, template )    a node is an entity (a player, a prop, a held item)
//   set_state( node, { "melee.hot": true } )   an entity's named values (visible as its "state"
//                                          metadata in the inspector); set_world_state for the world's
//   set_local( node )                     the viewer's own entity ($local, is_local)
//   cue( "melee.hit", at, other, { value, strength, point, end } )   something happened
//   press( "fire" )                       the viewer pressed an action: the CbPrediction nodes under
//                                         it say which cue that will be, and it plays at once
//   set_ticks( tick, confirmed )          where the game is, and up to where the server has
//                                         answered: a guess it left unanswered is taken back
//
// Reactions register themselves when they enter its tree. "While" reactions are checked every frame
// (update(), called from _process unless auto_update is off); cue reactions when a cue comes.

#include "cue_paths.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "cue_reaction.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace cb::gd
{

class CbPrediction;
class CbReaction;

class CbDirector : public godot::Node3D
{
	GDCLASS( CbDirector, godot::Node3D )

public:
	// `id`: a number state values can name the entity by (a NetId); cue paths find it with "@field".
	void add_entity( godot::Node* node, const godot::String& kind, const godot::String& template_name, int64_t id = 0 );
	void set_state( godot::Node* entity, const godot::Dictionary& state );
	godot::Dictionary get_state( godot::Node* entity ) const;
	void set_world_state( const godot::Dictionary& state );
	// Names that exist even where no state holds them (?name is true for them).
	void set_known( const godot::PackedStringArray& names );
	void set_local( godot::Node* entity );
	godot::Node* get_local() const;
	void cue( const godot::String& name, godot::Node* at, godot::Node* other, const godot::Dictionary& args );
	// The viewer (the local entity) pressed an action. Every prediction for that action whose
	// conditions hold plays its cue now, at the local entity, for the reactions that need nothing
	// but that; the server's cue of the same name, when it comes for the local entity, then plays
	// only the reactions that waited for it. Returns how many cues were predicted.
	int press( const godot::String& action );
	// The action is still down (call it every frame it is): the predictions that are while_held
	// play their cue again each time their cooldown has passed. Returns how many did.
	int hold( const godot::String& action );
	// The predictions still waiting for the server's cue, oldest first:
	// [ { cue, age (seconds), changes, stance, stance_layer } ]. Whoever hosts the director applies
	// what they say for the local entity until they are answered or expire.
	godot::Array pending_predictions();
	// What that press would do, without doing it: { prediction path: "predicts <cue>" or why not }.
	godot::Dictionary explain_press( const godot::String& action ) const;
	// The tick the game is at (presses from now on go with it) and the first tick the server has
	// not confirmed yet: every tick before it is the server's word. A press the server confirmed
	// `answer_ticks` past its tick without sending its cue was a wrong guess: what its reactions
	// started is taken back (scenes and sounds freed, an animation stopped, a property put back;
	// not a shake or a method call), and its changes and stance end. Without ticks (0) a guess
	// waits a second for its answer, then is taken back the same way.
	void set_ticks( int64_t tick, int64_t confirmed );
	void set_answer_ticks( int64_t v )
	{
		m_answerTicks = v;
	}
	int64_t get_answer_ticks() const
	{
		return m_answerTicks;
	}
	// How many guesses were taken back so far, and how many of those the server answered after all
	// (its cue came within two seconds: it then played in full, late; a margin too short says so).
	int64_t get_taken_back_count() const
	{
		return m_takenBack;
	}
	int64_t get_late_answer_count() const
	{
		return m_lateAnswers;
	}
	// What that cue would do, without doing it: { reaction path: "acts" or why not }.
	godot::Dictionary explain( const godot::String& name, godot::Node* at, godot::Node* other, const godot::Dictionary& args );
	void update();

	// The guard (cue_guard.h), for scripts that instantiate looks themselves.
	// Why a scene must not be used ("" when it may): it has a node class that is not listed, a
	// script, a signal connection, a path that leaves it, an animation that calls an unlisted method.
	static godot::String check_scene( const godot::Ref<godot::PackedScene>& scene );
	// An instance of the scene, or null (with a warning) when it is refused.
	static godot::Node* instantiate( const godot::Ref<godot::PackedScene>& scene );

	void set_auto_update( bool v )
	{
		m_autoUpdate = v;
	}
	bool get_auto_update() const
	{
		return m_autoUpdate;
	}

	void _process( double delta ) override;

	// For reactions and predictions.
	void Register( CbReaction* reaction );
	// How many reactions and predictions are listening now (a measure of what the scene costs).
	int64_t get_listener_count() const
	{
		return int64_t( m_reactions.size() + m_predictions.size() );
	}
	void Unregister( CbReaction* reaction );
	int Predict( const godot::String& action, bool held );
	void Register( CbPrediction* prediction );
	void Unregister( CbPrediction* prediction );
	void Reindex()
	{
		m_dirty = true;
	}

protected:
	static void _bind_methods();

private:
	struct StringHash
	{
		size_t operator()( const godot::String& s ) const
		{
			return size_t( s.hash() );
		}
	};
	cue::Context BaseContext() const;
	cue::Context CueContext( godot::Node* at, godot::Node* other, const godot::Dictionary& args ) const;
	void Index();

	bool m_autoUpdate = true;
	godot::ObjectID m_local;
	std::vector<godot::ObjectID> m_reactions;
	bool m_dirty = true;
	std::vector<godot::ObjectID> m_whiles;
	std::unordered_map<godot::String, std::vector<godot::ObjectID>, StringHash> m_byCue;

	// Plays a cue: every reaction listening for it but the ones in `skip`; what the ones that acted
	// started is added to `started`.
	void Play( const godot::String& name, const cue::Context& context, const std::vector<godot::ObjectID>* skip,
			   std::vector<Started>* started );
	std::vector<godot::ObjectID> m_predictions;
	// Cues shown on a press, waiting for the server's: the reactions that already played.
	struct Shown
	{
		godot::String cue;
		double at = 0.0;
		uint32_t tick = 0; // 0: the host gives no ticks
		std::vector<Started> started;
		godot::PackedStringArray changes;
		godot::String stance;
		godot::String stanceLayer;
	};
	std::deque<Shown> m_shown;
	uint32_t m_tick = 0;
	uint32_t m_confirmed = 0;
	int64_t m_answerTicks = 8;
	int64_t m_takenBack = 0;
	int64_t m_lateAnswers = 0;
	std::deque<std::pair<godot::String, double>> m_takenBackRecently; // cue, when
	void TakeBack( const Shown& shown );

public:
	// The same for C++ hosts: expired ones are dropped first.
	const std::deque<Shown>& Pending();
};

} // namespace cb::gd
