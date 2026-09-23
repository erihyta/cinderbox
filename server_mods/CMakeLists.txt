# Server gameplay mods, compiled into cb_server.
#
# Every folder here that contains <name>/<name>.cpp is a mod. That file defines
#     std::unique_ptr<cb::mods::ServerMod> CreateMod_<name>();
# and every .cpp in the folder is compiled with it. Adding a mod is adding a folder and
# re-running CMake; the registry below is generated from what it finds.

file(GLOB CB_MOD_ENTRIES LIST_DIRECTORIES true RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")
list(SORT CB_MOD_ENTRIES)

set(CB_MOD_SOURCES "")
set(CB_MOD_DECLARATIONS "")
set(CB_MOD_TABLE "")
foreach(entry ${CB_MOD_ENTRIES})
	if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${entry}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${entry}/${entry}.cpp")
		file(GLOB mod_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${entry}/*.cpp")
		list(APPEND CB_MOD_SOURCES ${mod_sources})
		string(APPEND CB_MOD_DECLARATIONS "std::unique_ptr<cb::mods::ServerMod> CreateMod_${entry}();\n")
		string(APPEND CB_MOD_TABLE "\t\t{ \"${entry}\", &CreateMod_${entry} },\n")
	endif()
endforeach()

configure_file(registry.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/registry.cpp" @ONLY)

add_library(cb_server_mods STATIC ${CB_MOD_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/registry.cpp")
target_include_directories(cb_server_mods PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_server_mods PUBLIC cb_server_core)
cb_target_warnings(cb_server_mods)
