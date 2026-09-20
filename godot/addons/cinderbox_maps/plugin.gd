@tool
extends EditorPlugin
## Bakes the edited map scene into a .cbmap the server can load.
##
## Editor tooling only: it is excluded from exported games, and mods never contain scripts.
## The nodes it bakes (CbStatic, CbProp, CbSpawn) come from the Cinderbox extension.

const MAP_DIR := "res://maps"

var _button: Button


func _enter_tree() -> void:
	_button = Button.new()
	_button.text = "Bake Map"
	_button.tooltip_text = "Write %s/<scene name>.cbmap from CbStatic / CbProp / CbSpawn nodes" % MAP_DIR
	_button.pressed.connect(_bake)
	add_control_to_container(CONTAINER_SPATIAL_EDITOR_MENU, _button)


func _exit_tree() -> void:
	remove_control_from_container(CONTAINER_SPATIAL_EDITOR_MENU, _button)
	_button.queue_free()


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

	print("Baked %s: %d statics, %d props, %d bytes, hash %s" % [
		out_path, result["statics"], result["props"], result["bytes"], result["hash"]])
	EditorInterface.get_resource_filesystem().scan()
