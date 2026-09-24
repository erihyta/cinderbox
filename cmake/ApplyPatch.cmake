# Applies a patch to a fetched dependency (a FetchContent PATCH_COMMAND). Runs in the dependency's
# source directory. Safe to run again: a patch that is already applied is left alone.
#
#   cmake -DGIT=<git> -DPATCH=<file.patch> -P ApplyPatch.cmake

execute_process(
	COMMAND "${GIT}" apply --reverse --check "${PATCH}"
	RESULT_VARIABLE already
	OUTPUT_QUIET ERROR_QUIET
)
if(already EQUAL 0)
	message(STATUS "Patch already applied: ${PATCH}")
	return()
endif()

execute_process(COMMAND "${GIT}" apply "${PATCH}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "Cannot apply ${PATCH} in ${CMAKE_CURRENT_SOURCE_DIR}")
endif()
message(STATUS "Applied ${PATCH}")
