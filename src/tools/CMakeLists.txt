set(CB_TOOL_TIMER_LIBS "")
if(WIN32)
	set(CB_TOOL_TIMER_LIBS winmm)
endif()

add_executable(cb_netsim netsim_main.cpp)
target_link_libraries(cb_netsim PRIVATE cb_net ${CB_TOOL_TIMER_LIBS})
cb_target_warnings(cb_netsim)

add_executable(cb_replay replay_main.cpp)
target_link_libraries(cb_replay PRIVATE cb_net)
cb_target_warnings(cb_replay)

add_executable(cb_bot bot_main.cpp)
target_link_libraries(cb_bot PRIVATE cb_client_core ${CB_TOOL_TIMER_LIBS})
cb_target_warnings(cb_bot)
