# Applies a patch to a fetched dependency (a FetchContent PATCH_COMMAND). Runs in the dependency's
# source directory. Safe to run again: a patch that is already applied is left alone.
#
#   cmake -DGIT=<git> -DPATCH=<file.patch> -P ApplyPatch.cmake
#
# A patch applied here leaves a mark (its hash, under the dependency's .git/cinderbox-patches/),
# and the mark says it is applied. Asking git alone ("does it apply in reverse?") is not enough once
# several patches change one file: a later patch changes this one's lines, its reverse no longer
# applies, and a reconfigure (which runs the patch step again) failed (Box3D's snapshot patches).
# Without a mark (a source patched before marks), the reverse check is still asked.

get_filename_component(name "${PATCH}" NAME)
set(marks "${CMAKE_CURRENT_SOURCE_DIR}/.git/cinderbox-patches")
set(mark "${marks}/${name}")
file(SHA256 "${PATCH}" want)
if(EXISTS "${mark}")
	file(READ "${mark}" had)
	if(had STREQUAL want)
		message(STATUS "Patch already applied: ${PATCH}")
		return()
	endif()
endif()

execute_process(
	COMMAND "${GIT}" apply --reverse --check "${PATCH}"
	RESULT_VARIABLE already
	OUTPUT_QUIET ERROR_QUIET
)
if(already EQUAL 0)
	message(STATUS "Patch already applied: ${PATCH}")
	file(MAKE_DIRECTORY "${marks}")
	file(WRITE "${mark}" "${want}")
	return()
endif()

execute_process(COMMAND "${GIT}" apply "${PATCH}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "Cannot apply ${PATCH} in ${CMAKE_CURRENT_SOURCE_DIR}. If it was patched before (a patch changed "
		"since, or one applied without a mark), delete that folder and configure again: it is fetched and patched anew.")
endif()
file(MAKE_DIRECTORY "${marks}")
file(WRITE "${mark}" "${want}")
message(STATUS "Applied ${PATCH}")
