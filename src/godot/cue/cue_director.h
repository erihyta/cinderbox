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
//
// Reactions register themselves when they enter its tree. "While" reactions are checked every frame
// (update(), called from _process unless auto_update is off); cue reactions when a cue comes.

#include "cue_paths.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <unordered_map>
#include <vector>

namespace cb::gd
{

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

	// For reactions.
	void Register( CbReaction* reaction );
	void Unregister( CbReaction* reaction );
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
};

} // namespace cb::gd
