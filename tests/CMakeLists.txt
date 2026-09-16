add_executable(cb_tests test_main.cpp)
target_link_libraries(cb_tests PRIVATE cb_sim)
cb_target_warnings(cb_tests)

foreach(name gameplay_sanity repeatability snapshot_roundtrip portable_snapshot rollback rollback_reset stress)
	add_test(NAME ${name} COMMAND cb_tests ${name})
endforeach()
