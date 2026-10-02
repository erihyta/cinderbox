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
