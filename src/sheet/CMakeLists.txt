# The sheet compiler (M283; docs/step6-plan.md): the one parser of the rules sheets and the bake of
# their motion blocks, with no Godot types, so the editor, the server, the cooker and the tests read a
# sheet the same way.
#
#   cb_sheet        the parser (and the golden files' dump): it depends on the expression language
#                   only, like the cue addon that uses it
#   cb_sheet_bake   a motion block as a motions file: it reads the simulation's data (the movement
#                   parameters, the motions file it bakes to and checks with the simulation's own
#                   compiler), cb_sim_data, never a world
add_library(cb_sheet STATIC
	sheet_dump.cpp
	sheet_rules.cpp
	sheet_text.cpp
	sheet_v1.cpp
	sheet_value.cpp
)
target_include_directories(cb_sheet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_sheet PUBLIC cb_expr)
set_target_properties(cb_sheet PROPERTIES POSITION_INDEPENDENT_CODE ON)
cb_target_warnings(cb_sheet)

add_library(cb_sheet_bake STATIC sheet_bake.cpp)
target_link_libraries(cb_sheet_bake PUBLIC cb_sheet cb_sim_data)
set_target_properties(cb_sheet_bake PROPERTIES POSITION_INDEPENDENT_CODE ON)
cb_target_warnings(cb_sheet_bake)

# The command line tool (docs/sheets.md, Tools): cb_sheet check | fmt | convert FILE...
add_executable(cb_sheet_tool cb_sheet.cpp)
set_target_properties(cb_sheet_tool PROPERTIES OUTPUT_NAME cb_sheet)
# (Its tests run the real server, headless: cb_sheet_test, src/server.)
target_link_libraries(cb_sheet_tool PRIVATE cb_sheet cb_sheet_test)
if(WIN32)
	target_link_libraries(cb_sheet_tool PRIVATE winmm)
endif()
cb_target_warnings(cb_sheet_tool)
