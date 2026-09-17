add_library(cb_present STATIC
	frame.cpp
	mirror.cpp
	scripts/player_animation.cpp
	scripts/spawn_destroy_effects.cpp
)
target_include_directories(cb_present PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_present PUBLIC cb_sim cb_anim)
cb_target_warnings(cb_present)
