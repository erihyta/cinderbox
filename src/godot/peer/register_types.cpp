#include "cinderbox_peer.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace
{

void InitializePeer( ModuleInitializationLevel level )
{
	if ( level == MODULE_INITIALIZATION_LEVEL_SCENE )
	{
		GDREGISTER_CLASS( cb::gd::CinderboxPeer );
	}
}

void UninitializePeer( ModuleInitializationLevel )
{
}

} // namespace

extern "C"
{
	GDExtensionBool GDE_EXPORT cinderbox_peer_library_init( GDExtensionInterfaceGetProcAddress getProcAddress,
															GDExtensionClassLibraryPtr library, GDExtensionInitialization* initialization )
	{
		GDExtensionBinding::InitObject init( getProcAddress, library, initialization );
		init.register_initializer( InitializePeer );
		init.register_terminator( UninitializePeer );
		init.set_minimum_library_initialization_level( MODULE_INITIALIZATION_LEVEL_SCENE );
		return init.init();
	}
}
