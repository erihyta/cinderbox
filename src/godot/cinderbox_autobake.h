#pragma once

// Bakes a character, or a mod's motions, when its scene is saved in the editor.
//
// The simulation poses bodies from baked files (ozz clips, the state machine, hitboxes), so a
// character edited in Godot has to be baked before the game shows the change. Saving the scene does
// it: when the saved scene's root is a CbCharacter (or a CbAnimPack), its bake runs. A bake that
// changes nothing writes nothing. The Bake button stays for what a scene save does not see (an
// animation saved to its own file), and packing an item bakes its characters again
// (tools/pack_mod.ps1), so a published item is never stale.
//
// A scene whose root is a CbMotionSet is baked the same way (cinderbox_motion.h), and the plugin
// gives the motion nodes their inspector help.
//
// It also puts the Cinderbox nodes a mod is made of into the Favorites of the editor's Create New
// Node dialog, so they are one list on its left instead of a search each: once per project and
// per node (one taken out of the favorites stays out).

#include "cinderbox_motion.h"

#include <godot_cpp/classes/button.hpp>
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
	void on_test_mod();

protected:
	static void _bind_methods();

private:
	void OfferNodes();
	// In a mod's client project (server_mods/<mod>/client of a checkout): a "Test mod" button in
	// the toolbar, which saves the scenes and runs tools/test_mod.ps1 for this mod (build, publish,
	// start the game hosting a server of its own).
	void OfferTest();
	godot::Button* m_testButton = nullptr;
	godot::String m_testScript;
	godot::String m_testMod;

	godot::Ref<CbMotionInspector> m_motionInspector;
};

} // namespace cb::gd
