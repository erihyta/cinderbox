extends SceneTree
## Bakes the motions of a mod's project, run inside that project by tools/pack_mod.ps1:
##
##   godot --headless --path server_mods/<mod>/client --script <repo>/godot/addons/cinderbox_maps/bake_motions.gd
##
## Every res://motion_sets/*.tscn whose root is a CbMotionSet is baked into res://motions/<its
## set_name>.cfg: what the server reads from the mod's item, sends to every client, and every
## simulation runs. Saving the scene in the editor does the same; this makes sure the item that is
## published was baked from the scenes it has now. A bake that changes nothing writes nothing.
## Exit code 1 if a set does not bake.

func _initialize() -> void:
	var baked := 0
	var failures := 0
	if DirAccess.dir_exists_absolute("res://motion_sets"):
		for file in DirAccess.get_files_at("res://motion_sets"):
			if not file.ends_with(".tscn"):
				continue
			var path := "res://motion_sets/" + file
			var packed := load(path) as PackedScene
			var root := packed.instantiate() if packed != null else null
			if root is CbMotionSet:
				var result: Dictionary = root.bake()
				if String(result.get("text", "")).is_empty():
					failures += 1
					printerr("%s: %s" % [path, result.get("error", "the bake failed")])
				else:
					root.bake_to_project()
					baked += 1
			if root != null:
				root.free()
	print("motion sets: %d baked%s" % [baked, "" if failures == 0 else ", %d FAILED" % failures])
	quit(0 if failures == 0 else 1)
