add_library(cb_anim STATIC
	anim_set.cpp
	hitboxes.cpp
	joint_math.cpp
	stances.cpp
	pose.cpp
	profile.cpp
)
target_include_directories(cb_anim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_anim PUBLIC cb_sim ozz_animation PRIVATE ozz_animation_offline)
cb_target_warnings(cb_anim)
