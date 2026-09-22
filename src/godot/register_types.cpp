#include "cinderbox_animator.h"
#include "cinderbox_client.h"
#include "cinderbox_effects.h"
#include "cinderbox_entity_nodes.h"
#include "cinderbox_map_nodes.h"
#include "cinderbox_skeleton.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace
{

void InitializeCinderbox( ModuleInitializationLevel level )
{
	if ( level != MODULE_INITIALIZATION_LEVEL_SCENE )
	{
		return;
	}
	GDREGISTER_CLASS( cb::gd::CinderboxSkeleton );
	GDREGISTER_CLASS( cb::gd::CinderboxAnimator );
	GDREGISTER_CLASS( cb::gd::CinderboxClient );
	// Map authoring: inert marker nodes plus the baker they are baked with.
	GDREGISTER_CLASS( cb::gd::CbStatic );
	GDREGISTER_CLASS( cb::gd::CbProp );
	GDREGISTER_CLASS( cb::gd::CbSpawn );
	GDREGISTER_CLASS( cb::gd::CbComponent );
	GDREGISTER_CLASS( cb::gd::CbTemplate );
	GDREGISTER_CLASS( cb::gd::CbEntity );
	GDREGISTER_CLASS( cb::gd::CinderboxMapBaker );
	// Effect bindings: data a mod ships to say what plays when.
	GDREGISTER_CLASS( cb::gd::CbEffect );
	GDREGISTER_CLASS( cb::gd::CbEffectTable );
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
