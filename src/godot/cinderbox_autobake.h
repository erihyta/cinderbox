#pragma once

// Bakes a character when its scene is saved in the editor.
//
// The simulation poses bodies from baked files (ozz clips, the state machine, hitboxes), so a
// character edited in Godot has to be baked before the game shows the change. Saving the scene does
// it: when the saved scene's root is a CbCharacter (or a CbAnimPack), its bake runs. A bake that
// changes nothing writes nothing. The Bake button stays for what a scene save does not see (an
// animation saved to its own file), and packing an item bakes its characters again
// (tools/pack_mod.ps1), so a published item is never stale.

#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/variant/string.hpp>

namespace cb::gd
{

class CbAutoBakePlugin : public godot::EditorPlugin
{
	GDCLASS( CbAutoBakePlugin, godot::EditorPlugin )

public:
	void _enter_tree() override;
	void _exit_tree() override;
	void on_scene_saved( const godot::String& path );

protected:
	static void _bind_methods();
};

} // namespace cb::gd
