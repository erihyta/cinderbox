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

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Must come before any dependency is added: the float flags have to apply to Box3D too.
include(Determinism)
include(Dependencies)

add_subdirectory(src/sim)
add_subdirectory(src/anim)
add_subdirectory(src/net)
add_subdirectory(src/server)
if(CB_BUILD_CLIENT)
	add_subdirectory(src/client)
endif()

if(CB_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
