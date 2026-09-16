add_library(cb_client_core STATIC game_client.cpp)
target_include_directories(cb_client_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_client_core PUBLIC cb_net)
cb_target_warnings(cb_client_core)

add_executable(cb_client
	app/anim_viewer.cpp
	app/main.cpp
	app/presentation.cpp
	app/scripts/player_animation.cpp
	app/scripts/spawn_destroy_effects.cpp
)
target_include_directories(cb_client PRIVATE app)
target_compile_definitions(cb_client PRIVATE CB_SOURCE_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets/anim")
target_link_libraries(cb_client PRIVATE cb_client_core cb_anim raylib)
cb_target_warnings(cb_client)
