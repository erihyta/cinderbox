#pragma once

// A mod's motions: what its players can do besides walking and jumping, with no code. A motion is a
// block of a rules sheet (docs/rules-language.md); a CbMotionSet is the scene a mod's sets of them
// live in, with its sheets under it:
//
//   Moves               CbMotionSet   set_name "dash.moves"       (motion_sets/dash_moves.tscn)
//   └── Rules           CbRules       file "res://motion_sets/dash_moves.rules"
//
// The set bakes to res://motions/<set_name>.cfg when its scene is saved (and when the mod is
// published); the server mod names it: declare.Motions( "dash.moves" ). The file is read by the
// server from the mod's item, sent to every client, and run by every simulation (sim/motions.h),
// so a player's own dash starts on the tick of the press, in its own prediction, and a rollback
// replays it. An item's own motions are the blocks of the sheets under its CbItem, baked with it.
//
// Until M268 a motion could also be a tree of nodes (CbMotion, CbProbe, CbImpulse, CbForce,
// CbLink, CbMove, CbLaunch, and CbCurve beside a sheet): withdrawn in M243, deleted in M268.
// The baking itself: sheet_motions.h.

#include <godot_cpp/classes/editor_inspector_plugin.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <string>

namespace cb::gd
{

// The root: a mod's motions, baked to one file.
class CbMotionSet : public godot::Node
{
	GDCLASS( CbMotionSet, godot::Node )

public:
	// The whole file: its header, then the motion blocks of every rules sheet under it, in tree
	// order. "" with `error` when it cannot be baked.
	std::string Bake( godot::String& error ) const;
	godot::PackedStringArray _get_configuration_warnings() const override;

	void set_set_name( const godot::String& v )
	{
		m_name = v;
		update_configuration_warnings();
	}
	godot::String get_set_name() const
	{
		return m_name;
	}

	// The set's file: { text, error, motions } (text is empty when it cannot be baked).
	godot::Dictionary bake() const;
	// What the Bake button and saving the scene do: writes res://motions/<set_name>.cfg, only when
	// its bytes change.
	void bake_to_project();
	godot::Callable get_bake_button();

protected:
	static void _bind_methods();

private:
	godot::String m_name;
};

// An info row at the top of a motion set's inspector, like a reaction's.
class CbMotionInspector : public godot::EditorInspectorPlugin
{
	GDCLASS( CbMotionInspector, godot::EditorInspectorPlugin )

public:
	bool _can_handle( godot::Object* object ) const override;
	void _parse_begin( godot::Object* object ) override;

protected:
	static void _bind_methods()
	{
	}
};

} // namespace cb::gd
