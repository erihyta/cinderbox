# Every test is registered from its runner's own table (`const Test tests[] = { ... };` in
# test_main.cpp and net_test.cpp), so a test added there runs in ctest and CI with nothing else to
# edit (four once sat in the table unregistered: M263). Each line of a table must be
# `{ "name", TestFunction },`; any other line stops the configure rather than go unregistered.
function(cb_table_tests source out)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})
	file(READ ${source} text)
	string(FIND "${text}" "const Test tests[] = {" begin)
	if(begin EQUAL -1)
		message(FATAL_ERROR "${source}: no test table (const Test tests[] = {)")
	endif()
	string(SUBSTRING "${text}" ${begin} -1 text)
	string(FIND "${text}" "\n\t};" end)
	string(SUBSTRING "${text}" 0 ${end} text)
	string(REPLACE "\n" ";" table_lines "${text}")
	list(REMOVE_AT table_lines 0)
	set(names)
	foreach(line IN LISTS table_lines)
		if(line MATCHES "^[ \t]*$")
			continue()
		elseif(line MATCHES "^[ \t]*{ \"([a-z0-9_]+)\", Test[A-Za-z0-9_]* },?$")
			list(APPEND names ${CMAKE_MATCH_1})
		else()
			message(FATAL_ERROR "${source}: a test table line ctest cannot read: ${line}")
		endif()
	endforeach()
	set(${out} ${names} PARENT_SCOPE)
endfunction()

add_executable(cb_tests test_main.cpp)
target_link_libraries(cb_tests PRIVATE cb_sim cb_anim cb_capture)
target_compile_definitions(cb_tests PRIVATE CB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
cb_target_warnings(cb_tests)

cb_table_tests(${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp sim_tests)
foreach(name IN LISTS sim_tests)
	add_test(NAME ${name} COMMAND cb_tests ${name})
endforeach()

add_executable(cb_net_tests net_test.cpp)
target_link_libraries(cb_net_tests PRIVATE cb_server_mods cb_client_core miniz)
target_compile_definitions(cb_net_tests PRIVATE CB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(WIN32)
	target_link_libraries(cb_net_tests PRIVATE winmm)
endif()
cb_target_warnings(cb_net_tests)
# (A crash's stack names the functions: tests/net_test.cpp, SayWhereItCrashed.)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_options(cb_net_tests PRIVATE -rdynamic)
endif()
cb_table_tests(${CMAKE_CURRENT_SOURCE_DIR}/net_test.cpp net_tests)
foreach(name IN LISTS net_tests)
	add_test(NAME net_${name} COMMAND cb_net_tests ${name})
endforeach()

# The hybrid's clients joining the chaos session's recording every few ticks, with the characters'
# states late or never there: their physics is the server's (cb_replay joins; the client's physics
# reads no character).
set_tests_properties(net_chaos_hybrid PROPERTIES ENVIRONMENT "CB_CHAOS_REPLAY=${CMAKE_CURRENT_BINARY_DIR}/chaos_hybrid.cbr"
	FIXTURES_SETUP chaos_hybrid_recording)
add_test(NAME replay_joins COMMAND cb_replay joins ${CMAKE_CURRENT_BINARY_DIR}/chaos_hybrid.cbr --every 10 --follow 300 --state-lag 30)
add_test(NAME replay_joins_no_states COMMAND cb_replay joins ${CMAKE_CURRENT_BINARY_DIR}/chaos_hybrid.cbr --every 10 --follow 300 --no-states 1)
set_tests_properties(replay_joins replay_joins_no_states PROPERTIES FIXTURES_REQUIRED chaos_hybrid_recording)
