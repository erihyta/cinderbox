#include "cinderbox_animator.h"
#include "cinderbox_character.h"
#include "cinderbox_client.h"
#include "cinderbox_autobake.h"
#include "cinderbox_track_player.h"
#include "cinderbox_entity_nodes.h"
#include "cinderbox_hud.h"
#include "cinderbox_map_nodes.h"
#include "cinderbox_item_look.h"
#include "cue_director.h"
#include "cue_prediction.h"
#include "cue_preview.h"
#include "cue_reaction.h"
#include "cinderbox_skeleton.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace
{

void InitializeCinderbox( ModuleInitializationLevel level )
{
	if ( level == MODULE_INITIALIZATION_LEVEL_EDITOR )
	{
		// Cue Preview: plays the edited scene's reactions in a bottom panel, with no game running.
		GDREGISTER_INTERNAL_CLASS( cb::gd::CbInfoButton );
		GDREGISTER_INTERNAL_CLASS( cb::gd::CbReactionInspector );
		GDREGISTER_INTERNAL_CLASS( cb::gd::CbCuePreviewDock );
		GDREGISTER_INTERNAL_CLASS( cb::gd::CbCuePreviewPlugin );
		EditorPlugins::add_by_type<cb::gd::CbCuePreviewPlugin>();
		// A character is baked when its scene is saved.
		GDREGISTER_INTERNAL_CLASS( cb::gd::CbAutoBakePlugin );
		EditorPlugins::add_by_type<cb::gd::CbAutoBakePlugin>();
		return;
	}
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE )
	{
		return;
	}
	GDREGISTER_CLASS( cb::gd::CinderboxSkeleton );
	GDREGISTER_CLASS( cb::gd::CbPoseModifier );
	GDREGISTER_CLASS( cb::gd::CinderboxAnimator );
	GDREGISTER_CLASS( cb::gd::CinderboxClient );
	// Character authoring: hit zones on bones, and the node that bakes a character for the game.
	GDREGISTER_CLASS( cb::gd::CbHitbox );
	GDREGISTER_CLASS( cb::gd::CbCharacter );
	GDREGISTER_CLASS( cb::gd::CbSocket );
	GDREGISTER_CLASS( cb::gd::CbAnimPack );
	GDREGISTER_CLASS( cb::gd::CbDirector );
	GDREGISTER_CLASS( cb::gd::CbReaction );
	GDREGISTER_CLASS( cb::gd::CbPrediction );
	GDREGISTER_CLASS( cb::gd::CbTrackPlayer );
	// Map authoring: inert marker nodes plus the baker they are baked with.
	GDREGISTER_CLASS( cb::gd::CbStatic );
	GDREGISTER_CLASS( cb::gd::CbProp );
	GDREGISTER_CLASS( cb::gd::CbSpawn );
	GDREGISTER_CLASS( cb::gd::CbComponent );
	GDREGISTER_CLASS( cb::gd::CbTemplate );
	GDREGISTER_CLASS( cb::gd::CbEntity );
	GDREGISTER_CLASS( cb::gd::CinderboxMapBaker );
	// Effect bindings: data a mod ships to say what plays when.
	GDREGISTER_CLASS( cb::gd::CbItemLook );
	GDREGISTER_CLASS( cb::gd::CbItemBody );
	// HUD nodes that read the mods' board.
	GDREGISTER_CLASS( cb::gd::CbFieldLabel );
	GDREGISTER_CLASS( cb::gd::CbFieldBinding );
	GDREGISTER_CLASS( cb::gd::CbEventFeed );
	GDREGISTER_CLASS( cb::gd::CbScoreboard );
	GDREGISTER_CLASS( cb::gd::CbPromptLabel );
}

void UninitializeCinderbox( ModuleInitializationLevel )
{
}

} // namespace

extern "C"
{
	GDExtensionBool GDE_EXPORT cinderbox_library_init( GDExtensionInterfaceGetProcAddress getProcAddress,
													   GDExtensionClassLibraryPtr library, GDExtensionInitialization* initialization )
	{
		GDExtensionBinding::InitObject init( getProcAddress, library, initialization );
		init.register_initializer( InitializeCinderbox );
		init.register_terminator( UninitializeCinderbox );
		init.set_minimum_library_initialization_level( MODULE_INITIALIZATION_LEVEL_SCENE );
		return init.init();
	}
}
