# The streaming client's core: a view source that is sent frames by a server (stream_source.h).
# Networking and the view codec; no simulation.
add_library(cb_stream STATIC stream_source.cpp)
target_include_directories(cb_stream PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_stream PUBLIC cb_net cb_present)
cb_target_warnings(cb_stream)
