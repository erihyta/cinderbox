extends SceneTree
## Bakes a mod's items, run inside the mod's client project:
##
##   godot --headless --path server_mods/melee/client --script <repo>/godot/addons/cinderbox_maps/bake_items.gd
##
## Every scene of the project whose root is a CbItem is an item. Its body (the CollisionShape3D under
## it: a box or a sphere, placed from the grip), its mass, its grips and its properties are written
## to res://items/<kind>.cfg with the scene's path, its name and its first-person view. The server
## reads the body from that file in the mod's workshop item; the game finds the scene by it. So the
## item drawn in the editor is the item in the game.
##
## Saving the scene in the editor does the same; tools\pack_mod.ps1 runs this before packing, so the
## item that is published was baked from the scenes it has now. The files are committed with the
## mod, so tests and servers built from source have them too. Exit code 1 if an item does not bake.

var _failures := 0
var _baked := 0


## Scenes whose root is a CbItem, found by their text (a scene is not loaded to learn it is not one).
func _item_scenes(dir: String, out: Array) -> void:
	for sub in DirAccess.get_directories_at(dir):
		if sub.begins_with(".") or sub == "addons" or sub == "bin":
			continue
		_item_scenes(dir.path_join(sub), out)
	for file in DirAccess.get_files_at(dir):
		if not file.ends_with(".tscn"):
			continue
		var path := dir.path_join(file)
		var text := FileAccess.get_file_as_string(path)
		# The root is the first node, and the only one without a parent.
		var at := text.find("[node ")
		var line := text.substr(at, text.find("]", at) - at) if at >= 0 else ""
		if line.contains("type=\"CbItem\"") and not line.contains("parent="):
			out.append(path)


func _initialize() -> void:
	var scenes := []
	_item_scenes("res://", scenes)
	scenes.sort()
	var kinds := {}
	for path in scenes:
		var packed := load(path) as PackedScene
		var item := packed.instantiate() as CbItem if packed != null else null
		if item == null:
			push_error("%s: its root is not a CbItem the game knows" % path)
			_failures += 1
			continue
		# An instance made here does not know its file: the bake writes the path into the item.
		item.scene_file_path = path
		var result: Dictionary = item.bake()
		if String(result["text"]).is_empty():
			push_error("%s: %s" % [path, result["error"]])
			_failures += 1
		elif kinds.has(item.kind):
			push_error("%s: the kind \"%s\" is %s's already" % [path, item.kind, kinds[item.kind]])
			_failures += 1
		else:
			kinds[item.kind] = path
			item.bake_to_project()
			_baked += 1
			print("  %s -> res://items/%s.cfg" % [path, item.kind])
		item.free()
	print("items: %d baked%s" % [_baked, "" if _failures == 0 else ", %d FAILED" % _failures])
	quit(0 if _failures == 0 else 1)
