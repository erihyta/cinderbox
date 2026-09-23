cmake_minimum_required(VERSION 3.24)

project(cinderbox VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(CB_BUILD_TESTS "Build tests" ON)
option(CB_BUILD_CLIENT "Build the raylib client" ON)
option(CB_BUILD_GODOT "Build the Godot GDExtension client (writes into godot/bin; enable in one preset only)" OFF)
set(CB_GODOT_TARGET "template_debug" CACHE STRING "godot-cpp target: template_debug (editor and debug exports) or template_release")

# Everything may end up inside the Godot extension (a shared library). PIC does not change
# floating-point semantics, so all builds use it to stay identical.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Must come before any dependency is added: the float flags have to apply to Box3D too.
include(Determinism)
include(Dependencies)

add_subdirectory(src/sim)
add_subdirectory(src/anim)
add_subdirectory(src/present)
add_subdirectory(src/net)
add_subdirectory(src/server)
add_subdirectory(server_mods)
target_link_libraries(cb_server PRIVATE cb_server_mods)
add_subdirectory(src/tools)
add_subdirectory(src/client) # the core is always needed; the raylib app is optional inside
if(CB_BUILD_GODOT)
	add_subdirectory(src/godot)
endif()

if(CB_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
