# The Godot client, as GDExtensions with bytes between them (present/view_codec.h):
#   cinderbox         the viewer (cinderbox_client.h), the cue addon, HUD nodes, authoring and bake
#                     nodes. It links no simulation and no networking; a mod's project needs only it.
#   cinderbox_peer    the sources that simulate (peer/): a live connection, a recording.

add_subdirectory(cue)

add_library(cinderbox_gd SHARED
	cinderbox_character.cpp
	cinderbox_client.cpp
	cinderbox_client_items.cpp
	cinderbox_client_values.cpp
	cinderbox_client_world.cpp
	cinderbox_autobake.cpp
	cinderbox_track_player.cpp
	cinderbox_entity_nodes.cpp
	cinderbox_hud.cpp
	cinderbox_body.cpp
	cinderbox_item_look.cpp
	cinderbox_map_nodes.cpp
	cinderbox_motion.cpp
	cinderbox_skeleton.cpp
	object_source.cpp
	register_types.cpp
)

# The class reference shown in the editor (hover texts, F1): compiled into the extension.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
target_doc_sources(cinderbox_gd
	"${CMAKE_CURRENT_SOURCE_DIR}/cue/doc_classes/CbReaction.xml;${CMAKE_CURRENT_SOURCE_DIR}/cue/doc_classes/CbDirector.xml;${CMAKE_CURRENT_SOURCE_DIR}/cue/doc_classes/CbPrediction.xml;${CMAKE_CURRENT_SOURCE_DIR}/cue/doc_classes/CbNames.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbPromptLabel.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbBody.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbItem.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbViewCamera.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbMotion.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbMotionSet.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbLinkLook.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbMotionPart.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbProbe.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbLaunch.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbMotionEffect.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbImpulse.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbForce.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbLink.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbList.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbKey.xml;${CMAKE_CURRENT_SOURCE_DIR}/doc_classes/CbClick.xml")

target_link_libraries(cinderbox_gd PRIVATE cb_present cb_cue godot::cpp ozz_animation_offline)
cb_target_warnings(cinderbox_gd)

get_target_property(CB_GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)

# The library goes into the Godot project; the .gdextension file lists it by this name.
set(CB_GODOT_BIN_DIR "${PROJECT_SOURCE_DIR}/godot/bin")
set_target_properties(cinderbox_gd PROPERTIES
	PREFIX "lib"
	OUTPUT_NAME "cinderbox${CB_GODOTCPP_SUFFIX}"
	CXX_VISIBILITY_PRESET hidden
	RUNTIME_OUTPUT_DIRECTORY "$<1:${CB_GODOT_BIN_DIR}>"
	LIBRARY_OUTPUT_DIRECTORY "$<1:${CB_GODOT_BIN_DIR}>"
	PDB_OUTPUT_DIRECTORY "$<1:${CB_GODOT_BIN_DIR}>"
)

if(MINGW)
	target_link_options(cinderbox_gd PRIVATE -static)
endif()

# The peer: the client core (simulation, networking, the sources) behind one class.
add_library(cinderbox_peer_gd SHARED
	packet_handoff.cpp
	peer/cinderbox_peer.cpp
	peer/register_types.cpp
)
target_include_directories(cinderbox_peer_gd PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(cinderbox_peer_gd PRIVATE cb_client_core godot::cpp)
cb_target_warnings(cinderbox_peer_gd)
set_target_properties(cinderbox_peer_gd PROPERTIES
	PREFIX "lib"
	OUTPUT_NAME "cinderbox_peer${CB_GODOTCPP_SUFFIX}"
	CXX_VISIBILITY_PRESET hidden
	RUNTIME_OUTPUT_DIRECTORY "$<1:${CB_GODOT_BIN_DIR}>"
	LIBRARY_OUTPUT_DIRECTORY "$<1:${CB_GODOT_BIN_DIR}>"
	PDB_OUTPUT_DIRECTORY "$<1:${CB_GODOT_BIN_DIR}>"
)
if(MINGW)
	target_link_options(cinderbox_peer_gd PRIVATE -static)
endif()
