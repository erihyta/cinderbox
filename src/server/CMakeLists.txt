add_library(cb_server_core STATIC game_server.cpp mod_api.cpp character_item.cpp hit_test.cpp sha256.cpp)
target_include_directories(cb_server_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_server_core PUBLIC cb_net cb_anim cb_capture PRIVATE miniz)
cb_target_warnings(cb_server_core)

add_executable(cb_server main.cpp)
# In a checkout the server reads the game's own characters where the editor bakes them.
target_compile_definitions(cb_server PRIVATE CB_CHARACTERS_SOURCE_DIR="${PROJECT_SOURCE_DIR}/godot/characters")
target_link_libraries(cb_server PRIVATE cb_server_core)
if(WIN32)
	target_link_libraries(cb_server PRIVATE winmm)
endif()
cb_target_warnings(cb_server)

# What the mods declare, for the editor (docs/looks.md, "Names"): after every build of the server,
# the names of every mod compiled into it are written next to each project that authors a look
# (the game, the SDK, every mod's and character's client project) as cinderbox_names.cfg. The
# extension completes them while typing and says which ones nobody declares; publishing a look
# checks against the same file. Written only when it changed.
file(GLOB CB_NAMES_PROJECTS LIST_DIRECTORIES true CONFIGURE_DEPENDS
	"${PROJECT_SOURCE_DIR}/server_mods/*/client" "${PROJECT_SOURCE_DIR}/characters/*/client")
list(APPEND CB_NAMES_PROJECTS "${PROJECT_SOURCE_DIR}/godot" "${PROJECT_SOURCE_DIR}/sdk")
set(CB_NAMES_COMMANDS "")
foreach(project ${CB_NAMES_PROJECTS})
	if(EXISTS "${project}/project.godot")
		list(APPEND CB_NAMES_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_BINARY_DIR}/bin/cinderbox_names.cfg" "${project}/cinderbox_names.cfg")
	endif()
endforeach()
add_custom_command(TARGET cb_server POST_BUILD
	COMMAND cb_server --dump-names "${CMAKE_BINARY_DIR}/bin/cinderbox_names.cfg"
	${CB_NAMES_COMMANDS}
	COMMENT "Writing the mods' names for the editor"
	VERBATIM
)
