extends SceneTree
## Checks the names a project's scenes use against what the server's mods declare, run inside that
## project by tools/pack_mod.ps1 before a look is packed:
##
##   godot --headless --path server_mods/<mod>/client --script <repo>/godot/addons/cinderbox_maps/check_names.gd
##
## Every .tscn of the project is opened and every node in it asked what it names that nobody
## declares (CbNames, the same check the editor shows as a warning in the scene tree): a reaction's
## event, a field in a condition or a format, a motion's action, a launch's item kind.
## The names are in res://cinderbox_names.cfg, which the server's build writes (cb_server
## --dump-names). Without that file there is nothing to check against, and nothing is said.
## Exit code 1 if a name is not declared.

func _scenes(dir: String, into: PackedStringArray) -> void:
	for file in DirAccess.get_files_at(dir):
		if file.ends_with(".tscn"):
			into.append(dir.path_join(file))
	for sub in DirAccess.get_directories_at(dir):
		if not sub.begins_with(".") and sub != "addons" and sub != "bin":
			_scenes(dir.path_join(sub), into)


func _initialize() -> void:
	var names := CbNames.new()
	if not names.is_known():
		print("names: not checked (no res://cinderbox_names.cfg: build the server to get one)")
		quit(0)
		return
	var scenes := PackedStringArray()
	_scenes("res://", scenes)
	var problems := 0
	for path in scenes:
		var packed := load(path) as PackedScene
		var root := packed.instantiate() if packed != null else null
		if root == null:
			continue
		for line in names.check_scene(root):
			problems += 1
			printerr("%s: %s" % [path, line])
		root.free()
	print("names: %d scenes checked%s" % [scenes.size(), "" if problems == 0 else ", %d names nobody declares" % problems])
	quit(0 if problems == 0 else 1)
