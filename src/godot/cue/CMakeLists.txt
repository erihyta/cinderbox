# Cue: reactions driven by cues and entity state (CbDirector, CbReaction). Plain Godot: it depends
# on godot-cpp, the expression language (cb_expr, which depends on nothing) and the sheet parser
# (cb_sheet, which depends on cb_expr), never on the rest of Cinderbox, so anything can drive it.

add_library(cb_cue STATIC
	cue_director.cpp
	cue_guard.cpp
	cue_info.cpp
	cue_names.cpp
	cue_paths.cpp
	cue_prediction.cpp
	cue_preview.cpp
	cue_reaction.cpp
	cue_rules.cpp
)
target_include_directories(cb_cue PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cb_cue PUBLIC godot::cpp cb_expr cb_sheet)
set_target_properties(cb_cue PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
cb_target_warnings(cb_cue)
