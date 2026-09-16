add_library(cb_client_core STATIC game_client.cpp)
target_include_directories(cb_client_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_client_core PUBLIC cb_net)
cb_target_warnings(cb_client_core)

add_executable(cb_client
	app/main.cpp
	app/presentation.cpp
	app/scripts/player_motion.cpp
	app/scripts/spawn_destroy_effects.cpp
)
target_include_directories(cb_client PRIVATE app)
target_link_libraries(cb_client PRIVATE cb_client_core raylib)
cb_target_warnings(cb_client)
