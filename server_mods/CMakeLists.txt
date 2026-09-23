# Server gameplay mods, compiled into cb_server.
#
# Every folder here that contains <name>/<name>.cpp is a mod. That file defines
#     std::unique_ptr<cb::mods::ServerMod> CreateMod_<name>();
# and every .cpp in the folder is compiled with it. Adding a mod is adding a folder and
# re-running CMake; the registry below is generated from what it finds.
#
# A mod with a look has a client/ Godot project next to its code. tools\publish_mod.ps1 packs it into
# a workshop item and records the item's SHA-256 in <name>/client_item.cfg; the build copies that
# to bin/items/<name>.item, which cb_server reads to tell clients which item they need.

file(GLOB CB_MOD_ENTRIES LIST_DIRECTORIES true RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")
list(SORT CB_MOD_ENTRIES)

set(CB_MOD_SOURCES "")
set(CB_MOD_DECLARATIONS "")
set(CB_MOD_TABLE "")
set(CB_MOD_ITEM_COMMANDS "")
foreach(entry ${CB_MOD_ENTRIES})
	if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${entry}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${entry}/${entry}.cpp")
		file(GLOB mod_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${entry}/*.cpp")
		list(APPEND CB_MOD_SOURCES ${mod_sources})
		string(APPEND CB_MOD_DECLARATIONS "std::unique_ptr<cb::mods::ServerMod> CreateMod_${entry}();\n")
		set(has_client false)
		if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${entry}/client")
			set(has_client true)
			set(item "${CMAKE_CURRENT_SOURCE_DIR}/${entry}/client_item.cfg")
			if(EXISTS "${item}")
				list(APPEND CB_MOD_ITEM_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy_if_different "${item}" "${CMAKE_BINARY_DIR}/bin/items/${entry}.item")
			else()
				message(WARNING "server mod '${entry}' has client content but no published item: run tools/publish_mod.ps1 -Mod ${entry}")
			endif()
		endif()
		string(APPEND CB_MOD_TABLE "\t\t{ \"${entry}\", &CreateMod_${entry}, ${has_client} },\n")
	endif()
endforeach()

configure_file(registry.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/registry.cpp" @ONLY)

add_library(cb_server_mods STATIC ${CB_MOD_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/registry.cpp")
target_include_directories(cb_server_mods PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_server_mods PUBLIC cb_server_core)
cb_target_warnings(cb_server_mods)

# Runs on every build, so publishing an item and building is all it takes.
add_custom_target(cb_server_items ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/items"
	${CB_MOD_ITEM_COMMANDS}
	COMMENT "Copying published workshop item manifests"
	VERBATIM
)
