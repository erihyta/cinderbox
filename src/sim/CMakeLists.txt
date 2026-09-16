add_library(cb_sim STATIC
	box3d_shim.c
	fingerprint.cpp
	level.cpp
	physics_arena.cpp
	rollback.cpp
	simulation.cpp
)

target_include_directories(cb_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_sim PUBLIC flecs::flecs_static box3d)

# The shim reads Box3D's internal headers.
set_source_files_properties(box3d_shim.c PROPERTIES INCLUDE_DIRECTORIES "${CB_BOX3D_SOURCE_DIR}/src")

cb_target_warnings(cb_sim)
