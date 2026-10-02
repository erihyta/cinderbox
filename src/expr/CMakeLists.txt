# The one expression language (conditions and values): a parser and an evaluator with no
# dependencies, used by the simulation's state machines, the HUD's field readers and the reaction
# addon alike.
add_library(cb_expr STATIC expr.cpp)
target_include_directories(cb_expr PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
cb_target_warnings(cb_expr)
