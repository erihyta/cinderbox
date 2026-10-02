# The networked client, and the view sources built on it (see present/view.h): everything a viewer
# needs to be handed frames, with no rendering.
add_library(cb_client_core STATIC
	game_client.cpp
	live_source.cpp
	replay_player.cpp
	replay_source.cpp
)
target_include_directories(cb_client_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_client_core PUBLIC cb_net cb_capture)
cb_target_warnings(cb_client_core)

if(NOT CB_BUILD_CLIENT)
	return()
endif()

add_executable(cb_client
	app/anim_viewer.cpp
	app/main.cpp
	app/presentation.cpp
	app/replay_viewer.cpp
)
target_include_directories(cb_client PRIVATE app)
target_compile_definitions(cb_client PRIVATE CB_SOURCE_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets/anim")
target_link_libraries(cb_client PRIVATE cb_client_core raylib)
cb_target_warnings(cb_client)
