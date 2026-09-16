# Third-party dependencies, pinned to exact commits. Changing any of these can change simulation
# results, so bump them deliberately and re-run the cross-build determinism check.

include(FetchContent)

# Dependencies are downloaded and built inside each build directory (the FetchContent default,
# <build>/_deps). Do not share this directory between presets: FetchContent also puts the
# dependencies' build trees there, so different compilers would overwrite each other's objects.

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

# --- ENet v1.3.18 (networking) ---
FetchContent_Declare(enet
	GIT_REPOSITORY https://github.com/lsalzman/enet.git
	GIT_TAG 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a # v1.3.18
	GIT_SHALLOW FALSE
)

# --- ozz-animation 0.17.0 (scalar math so poses are bit-exact everywhere) ---
set(ozz_build_simd_ref ON CACHE BOOL "" FORCE)
set(ozz_build_tools ON CACHE BOOL "" FORCE)
set(ozz_build_gltf ON CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_data OFF CACHE BOOL "" FORCE)
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_postfix OFF CACHE BOOL "" FORCE)
set(ozz_build_msvc_rt_dll ON CACHE BOOL "" FORCE) # match the rest of the project (DLL CRT)
FetchContent_Declare(ozz
	GIT_REPOSITORY https://github.com/guillaumeblanc/ozz-animation.git
	GIT_TAG 744eb9d99f606eda849acb0b1204f7a3dc20bca1 # 0.17.0
	GIT_SHALLOW FALSE
)

FetchContent_MakeAvailable(flecs box3d enet ozz)

# ozz builds with warnings-as-errors; a newer compiler must not break our build.
foreach(t ozz_base ozz_animation ozz_animation_offline ozz_animation_tools ozz_options ozz_geometry gltf2ozz dump2ozz)
	if(TARGET ${t})
		set_target_properties(${t} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
	endif()
endforeach()
if(TARGET gltf2ozz)
	# ozz sets per-configuration output directories, which win over the generic property.
	foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
		set_target_properties(gltf2ozz PROPERTIES RUNTIME_OUTPUT_DIRECTORY${cfg} "${CMAKE_BINARY_DIR}/bin")
	endforeach()
endif()

# --- raylib 5.5 (client only) ---
if(CB_BUILD_CLIENT)
	set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
	set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(raylib
		GIT_REPOSITORY https://github.com/raysan5/raylib.git
		GIT_TAG c1ab645ca298a2801097931d1079b10ff7eb9df8 # 5.5
		GIT_SHALLOW FALSE
	)
	FetchContent_MakeAvailable(raylib)
endif()

# Box3D internal headers are needed by the physics snapshot shim (see src/sim/box3d_shim.c).
set(CB_BOX3D_SOURCE_DIR "${box3d_SOURCE_DIR}" CACHE INTERNAL "")
