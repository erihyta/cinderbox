add_library(cb_net STATIC
	netsim.cpp
	replay.cpp
	protocol.cpp
	transport.cpp
)

target_include_directories(cb_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# ENet does not export its include directory.
target_include_directories(cb_net PRIVATE "${enet_SOURCE_DIR}/include")
# (What the data means only: the wire carries inputs, maps and schemas, and never steps a world.)
target_link_libraries(cb_net PUBLIC cb_sim_data PRIVATE enet)
if(WIN32)
	target_link_libraries(cb_net PRIVATE ws2_32 winmm)
endif()

cb_target_warnings(cb_net)
