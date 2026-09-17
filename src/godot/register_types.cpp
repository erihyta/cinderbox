#include "cinderbox_client.h"
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
	GDREGISTER_CLASS( cb::gd::CinderboxClient );
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
