# Floating point settings required for cross-platform / cross-compiler bit-exact simulation.
# See https://box2d.org/posts/2024/08/determinism/
#
# - No fast-math, no FMA contraction (the one value-changing transform compilers enable by default).
# - x64 always uses SSE2 for float math, so x87 extended precision is never involved.
# - ARM64 contraction is disabled by the same flag.
#
# These are applied directory-wide so every dependency (Box3D, flecs, ozz) is built the same way.

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
		# clang-cl silently ignores the GNU spelling
		add_compile_options(/clang:-ffp-contract=off /clang:-fno-fast-math)
	else()
		add_compile_options(-ffp-contract=off -fno-fast-math)
	endif()
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|X86|amd64|AMD64" AND CMAKE_SIZEOF_VOID_P EQUAL 4)
		message(FATAL_ERROR "32-bit x86 is not supported: x87 math breaks determinism")
	endif()
elseif(MSVC)
	# /fp:precise is the default, but be explicit. It does not contract to FMA unless /fp:contract is given.
	add_compile_options(/fp:precise)
endif()

if(MINGW)
	# Self-contained executables: otherwise whichever libstdc++-6.dll comes first on PATH (e.g. Git
	# for Windows ships an older one) is loaded, and the program fails to start.
	add_link_options(-static)
endif()

if(WIN32)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Sanitizers (the CI's sanitizer jobs, docs/testing.md): "address,undefined" or "thread", on every
# target and dependency alike. Clang or GCC on Linux or macOS. Off by default. A sanitizer adds no
# floating-point transform, so the simulation computes what it does without one.
set(CB_SANITIZE "" CACHE STRING "Sanitizers for every target: address,undefined or thread (empty: none)")
if(CB_SANITIZE)
	if(MSVC OR WIN32)
		message(FATAL_ERROR "CB_SANITIZE is for Clang or GCC on Linux or macOS")
	endif()
	add_compile_options(-fsanitize=${CB_SANITIZE} -fno-omit-frame-pointer)
	add_link_options(-fsanitize=${CB_SANITIZE})
	if(CB_SANITIZE MATCHES "undefined")
		# Undefined behaviour stops the test instead of being printed and passed over.
		add_compile_options(-fno-sanitize-recover=undefined)
		if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
			# (Calls through a function pointer cast to another type: flecs' callbacks are made so.)
			add_compile_options(-fno-sanitize=function)
		endif()
	endif()
endif()

# Warnings for our own targets only.
function(cb_target_warnings target)
	if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_options(${target} PRIVATE /W4 /permissive-)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
	endif()
endfunction()
