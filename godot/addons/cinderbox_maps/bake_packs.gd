extends SceneTree
## Bakes the animation packs of a mod's project, run inside that project by tools/pack_mod.ps1:
##
##   godot --headless --path server_mods/<mod>/client --script <repo>/godot/addons/cinderbox_maps/bake_packs.gd
##
## Every res://animation_packs/*.tscn whose root is a CbAnimPack is baked into res://anim/<its name>/ (the
## ozz skeleton and clips, the state machine): what the server reads and every client poses from.
## Saving the scene in the editor does the same; this makes sure the item that is published was
## baked from the scenes it has now. A bake that changes nothing writes nothing. Exit code 1 if a
## pack does not bake.

func _initialize() -> void:
	var baked := 0
	var failures := 0
	if DirAccess.dir_exists_absolute("res://animation_packs"):
		for file in DirAccess.get_files_at("res://animation_packs"):
			if not file.ends_with(".tscn"):
				continue
			var path := "res://animation_packs/" + file
			var packed := load(path) as PackedScene
			var root := packed.instantiate() if packed != null else null
			if root is CbAnimPack:
				var result: Dictionary = root.bake_to("")
				if result.get("ok", false):
					baked += 1
					if not String(result.get("warnings", "")).is_empty():
						push_warning("%s: %s" % [path, result["warnings"]])
				else:
					failures += 1
					printerr("%s: %s" % [path, result.get("error", "the bake failed")])
			if root != null:
				root.free()
	print("animation packs: %d baked%s" % [baked, "" if failures == 0 else ", %d FAILED" % failures])
	quit(0 if failures == 0 else 1)
