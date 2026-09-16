# Third-party dependencies, pinned to exact commits. Changing any of these can change simulation
# results, so bump them deliberately and re-run the cross-build determinism check.

include(FetchContent)

# Keep downloads next to the build tree so a clean rebuild of one preset does not re-download.
if(NOT FETCHCONTENT_BASE_DIR OR FETCHCONTENT_BASE_DIR STREQUAL "${CMAKE_BINARY_DIR}/_deps")
	set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/../_deps" CACHE PATH "" FORCE)
endif()

# --- flecs v4.1.6 ---
set(FLECS_STATIC ON CACHE BOOL "" FORCE)
set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_PIC OFF CACHE BOOL "" FORCE)
set(FLECS_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(flecs
	GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
	GIT_TAG fb55f3c25660425cfe1bc4cf5e6bff8b3f18a9b8 # v4.1.6
	GIT_SHALLOW FALSE
)

# --- Box3D (main @ 2026-08-29) ---
set(BOX3D_DISABLE_SIMD OFF CACHE BOOL "" FORCE)
set(BOX3D_DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
FetchContent_Declare(box3d
	GIT_REPOSITORY https://github.com/erincatto/box3d.git
	GIT_TAG 47d7f7cc7e091142c08d11dc7d2e493c5d34f536
	GIT_SHALLOW FALSE
)

FetchContent_MakeAvailable(flecs box3d)

# Box3D internal headers are needed by the physics snapshot shim (see src/sim/box3d_shim.c).
set(CB_BOX3D_SOURCE_DIR "${box3d_SOURCE_DIR}" CACHE INTERNAL "")
