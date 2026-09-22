# Cinderbox GDExtension: the Godot client (see cinderbox_client.h).

add_library(cinderbox_gd SHARED
	cinderbox_animator.cpp
	cinderbox_client.cpp
	cinderbox_effects.cpp
	cinderbox_entity_nodes.cpp
	cinderbox_map_nodes.cpp
	cinderbox_skeleton.cpp
	client_thread.cpp
	register_types.cpp
)

target_link_libraries(cinderbox_gd PRIVATE cb_client_core cb_present godot::cpp)
cb_target_warnings(cinderbox_gd)

get_target_property(CB_GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)

# The library goes into the Godot project; the .gdextension file lists it by this name.
set(CB_GODOT_BIN_DIR "${PROJECT_SOURCE_DIR}/godot/bin")
set_target_properties(cinderbox_gd PROPERTIES
	PREFIX "lib"
	OUTPUT_NAME "cinderbox${CB_GODOTCPP_SUFFIX}"
	CXX_VISIBILITY_PRESET hidden
	RUNTIME_OUTPUT_DIRECTORY "$<1:${CB_GODOT_BIN_DIR}>"
	LIBRARY_OUTPUT_DIRECTORY "$<1:${CB_GODOT_BIN_DIR}>"
	PDB_OUTPUT_DIRECTORY "$<1:${CB_GODOT_BIN_DIR}>"
)

if(MINGW)
	target_link_options(cinderbox_gd PRIVATE -static)
endif()
