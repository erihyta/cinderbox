# Presentation: what a viewer does with frames. It links no simulation.
add_library(cb_present STATIC
	anim_lead.cpp
	camera.cpp
	fields.cpp
	mirror.cpp
	pose_tools.cpp
	scripts/player_animation.cpp
	scripts/ragdolls.cpp
	scripts/spawn_destroy_effects.cpp
	source_thread.cpp
	view.cpp
	view_codec.cpp
	view_file.cpp
)
target_include_directories(cb_present PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_present PUBLIC cb_sim_data cb_anim)
cb_target_warnings(cb_present)

# A simulation's state as a frame: for whoever has a simulation (sources, the server, tests).
add_library(cb_capture STATIC capture.cpp)
target_link_libraries(cb_capture PUBLIC cb_present cb_sim)
cb_target_warnings(cb_capture)
