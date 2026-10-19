@tool
extends EditorPlugin
## Bakes the edited map scene into a .cbmap the server can load.
##
## Editor tooling only: it is excluded from exported games, and mods never contain scripts.
## The nodes it bakes (CbStatic, CbProp, CbSpawn) come from the Cinderbox extension.

const MAP_DIR := "res://maps"
const Fracture := preload("res://addons/cinderbox_maps/fracture.gd")

var _button: Button
# Fracture: shown while one CbBreakable is selected; cuts its shape into chunks (fracture.gd).
var _fracture: Button
var _dialog: ConfirmationDialog
var _count: SpinBox
var _seed: SpinBox
var _crowd: SpinBox
var _wall: Node3D


func _enter_tree() -> void:
	_button = Button.new()
	_button.text = "Bake Map"
	_button.tooltip_text = "Write %s/<scene name>.cbmap from the CbStatic / CbProp / CbSpawn / CbTemplate / CbEntity nodes" % MAP_DIR
	_button.pressed.connect(_bake)
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, _button)

	_fracture = Button.new()
	_fracture.text = "Fracture"
	_fracture.tooltip_text = "Cut the selected CbBreakable's shape into irregular chunks (its pieces), replacing the pieces it has"
	_fracture.pressed.connect(_ask_fracture)
	_fracture.visible = false
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, _fracture)
	_dialog = ConfirmationDialog.new()
	_dialog.title = "Fracture"
	var grid := GridContainer.new()
	grid.columns = 2
	_count = _field(grid, "Pieces", 2, 256, 1, "How many chunks")
	_seed = _field(grid, "Seed", 0, 99999, 1, "Another number, other chunks; the same, the same chunks")
	_crowd = _field(grid, "Crowd", 0, 1, 0.05,
		"The share of chunks crowded around a child Node3D named FractureFocus (else the centre): small there, big farther out")
	_dialog.add_child(grid)
	_dialog.confirmed.connect(_do_fracture)
	EditorInterface.get_base_control().add_child(_dialog)
	EditorInterface.get_selection().selection_changed.connect(_selection_changed)


func _exit_tree() -> void:
	remove_control_from_container(CONTAINER_SPATIAL_EDITOR_MENU, _button)
	_button.queue_free()
	EditorInterface.get_selection().selection_changed.disconnect(_selection_changed)
	remove_control_from_container(CONTAINER_SPATIAL_EDITOR_MENU, _fracture)
	_fracture.queue_free()
	_dialog.queue_free()


func _field(grid: GridContainer, label: String, low: float, high: float, step: float, tip: String) -> SpinBox:
	var name := Label.new()
	name.text = label
	name.tooltip_text = tip
	grid.add_child(name)
	var box := SpinBox.new()
	box.min_value = low
	box.max_value = high
	box.step = step
	box.tooltip_text = tip
	box.custom_minimum_size.x = 120
	grid.add_child(box)
	return box


func _selection_changed() -> void:
	var selected := EditorInterface.get_selection().get_selected_nodes()
	_fracture.visible = selected.size() == 1 and selected[0] is CbBreakable


func _ask_fracture() -> void:
	var selected := EditorInterface.get_selection().get_selected_nodes()
	if selected.size() != 1 or not selected[0] is CbBreakable:
		return
	_wall = selected[0]
	# The settings it was cut with last (kept on the node), else as many as it has pieces.
	var last: Dictionary = _wall.get_meta("cinderbox_fracture", {})
	var pieces := _wall.get_children().filter(func(c): return c is CbProp).size()
	_count.value = last.get("pieces", pieces if pieces >= 2 else 16)
	_seed.value = last.get("seed", 1)
	_crowd.value = last.get("crowd", 0.0)
	_dialog.popup_centered()


func _do_fracture() -> void:
	if not is_instance_valid(_wall):
		return
	var made: Dictionary = Fracture.fracture(_wall, int(_count.value), int(_seed.value), _crowd.value)
	if made.error != "":
		push_error("Fracture: %s" % made.error)
		return
	var root := EditorInterface.get_edited_scene_root()
	var old := _wall.get_children().filter(func(c): return c is CbProp)
	var settings := {"pieces": int(_count.value), "seed": int(_seed.value), "crowd": _crowd.value}
	var undo := get_undo_redo()
	undo.create_action("Fracture %s" % _wall.name)
	for piece in old:
		undo.add_do_method(_wall, "remove_child", piece)
		undo.add_undo_reference(piece)
	for piece in made.pieces:
		undo.add_do_method(_wall, "add_child", piece, true)
		for node in [piece] + piece.get_children():
			undo.add_do_method(node, "set_owner", root)
		undo.add_do_reference(piece)
		undo.add_undo_method(_wall, "remove_child", piece)
	for piece in old:
		undo.add_undo_method(_wall, "add_child", piece)
		undo.add_undo_method(piece, "set_owner", root)
		for node in _descendants(piece):
			undo.add_undo_method(node, "set_owner", root)
	undo.add_do_method(_wall, "set_meta", "cinderbox_fracture", settings)
	undo.add_undo_method(_wall, "set_meta", "cinderbox_fracture", _wall.get_meta("cinderbox_fracture", {}))
	undo.commit_action()
	print("Fracture: %s cut into %d pieces" % [_wall.name, made.pieces.size()])


func _descendants(node: Node) -> Array:
	var all := []
	for child in node.get_children():
		all.append(child)
		all.append_array(_descendants(child))
	return all


func _bake() -> void:
	var root := EditorInterface.get_edited_scene_root()
	if root == null:
		push_warning("Bake Map: no scene is open")
		return
	if root.scene_file_path.is_empty():
		push_warning("Bake Map: save the scene first")
		return

	var map_name := root.scene_file_path.get_file().get_basename()
	DirAccess.make_dir_recursive_absolute(MAP_DIR)
	var out_path := "%s/%s.cbmap" % [MAP_DIR, map_name]

	var result: Dictionary = CinderboxMapBaker.new().bake(root, out_path)
	for warning in result.get("warnings", []):
		push_warning("Bake Map: %s" % warning)
	if not result.get("ok", false):
		push_error("Bake Map failed: %s" % result.get("error", "unknown error"))
		return

	print("Baked %s: %d statics, %d props, %d templates, %d instances, %d bytes, hash %s" % [
		out_path, result["statics"], result["props"], result["templates"], result["instances"],
		result["bytes"], result["hash"]])
	EditorInterface.get_resource_filesystem().scan()
