# What the simulation's data means, without the simulation: components, the map format, the mod
# schema, the baked state machines. Everything that only reads or draws the world links this
# (animation, presentation, the Godot viewer) and never steps a world.
add_library(cb_sim_data STATIC
	anim_controller.cpp
	anim_graph.cpp
	level.cpp
	map.cpp
	mod_schema.cpp
	reflect.cpp
	world_lifetime.cpp
)
target_include_directories(cb_sim_data PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_sim_data PUBLIC box3d flecs::flecs_static cb_expr)
cb_target_warnings(cb_sim_data)

# The simulation itself: the world, physics, snapshots, rollback.
add_library(cb_sim STATIC
	box3d_shim.c
	fingerprint.cpp
	physics_arena.cpp
	rollback.cpp
	simulation.cpp
)

target_link_libraries(cb_sim PUBLIC cb_sim_data)

# The shim reads Box3D's internal headers.
set_source_files_properties(box3d_shim.c PROPERTIES INCLUDE_DIRECTORIES "${CB_BOX3D_SOURCE_DIR}/src")

cb_target_warnings(cb_sim)
