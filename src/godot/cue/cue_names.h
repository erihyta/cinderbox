#pragma once

// The names a server's mods declare, known to the editor.
//
// A look is written against names a server mod declares ("combat.health", "melee.hit", "dash"),
// typed by hand: one typed wrong reads as 0, never fires, and says nothing. The server's build
// writes what every mod compiled into it declares to res://cinderbox_names.cfg (cb_server
// --dump-names); this reads it, so that
//
//   - a property that holds one name (a reaction's event, a motion's action, a launch's item kind)
//     offers the names there are while typing (Hint), and
//   - a node says, in the scene tree, which of the names it uses nobody declares (Problems): in a
//     name, an expression ("dash.charges > 0"), a format ("{pistol.ammo} / 12"), a change
//     ("dash.charges -= 1"), with the closest name there is ("did you mean ...").
//
// Which properties of which node hold what is one table (cue_names.cpp), by class and property
// name, so a node needs no code of its own for it. Without the file nothing is offered and nothing
// is said: a look can be written with no server at hand.
//
// CbNames is the same for tools: publishing a look checks every scene of the project with it.

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace cb::gd::names
{

// True when the project has a names file.
bool Known();

// For a node's _validate_property: a property that holds one name gets the names to choose from.
void Hint( const godot::Object* object, godot::PropertyInfo& property );

// What the node names that nobody declares, one line each (empty: nothing, or no names file).
godot::PackedStringArray Problems( const godot::Node* node );

// The same for a node and everything under it, each line with the node's path.
godot::PackedStringArray ProblemsUnder( const godot::Node* root );

} // namespace cb::gd::names

namespace cb::gd
{

// For scripts and tools (tools/pack_mod.ps1 runs check_names.gd before a look is packed).
class CbNames : public godot::RefCounted
{
	GDCLASS( CbNames, godot::RefCounted )

public:
	// False when the project has no res://cinderbox_names.cfg (build the server to get one).
	bool is_known() const;
	// Every name of a kind: field, event, action, item, stance, layer, socket, pack, motions, mod.
	godot::PackedStringArray get_names( const godot::String& kind ) const;
	// What `root` and the nodes under it name that nobody declares: "Path/To/Node: ...".
	godot::PackedStringArray check_scene( godot::Node* root ) const;

protected:
	static void _bind_methods();
};

} // namespace cb::gd
