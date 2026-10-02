extends SceneTree
## Bakes the characters of an item's project, run inside that project by tools/pack_mod.ps1:
##
##   godot --headless --path characters/robot/client --script <repo>/godot/addons/cinderbox_maps/bake_characters.gd
##
## Every res://characters/<name>/character.tscn whose root is a CbCharacter is baked into its own
## folder (the ozz skeleton and clips, the state machine, the hitboxes): what the server and every
## simulating client pose the body from. Saving the scene in the editor does the same; this makes
## sure the item that is published was baked from the scene that is published. A bake that changes
## nothing writes nothing. Exit code 1 if a character does not bake.

var _failures := 0


func _initialize() -> void:
	var baked := 0
	if DirAccess.dir_exists_absolute("res://characters"):
		for folder in DirAccess.get_directories_at("res://characters"):
			var path := "res://characters/%s/character.tscn" % folder
			if not ResourceLoader.exists(path):
				continue
			var packed := load(path) as PackedScene
			var root := packed.instantiate() if packed != null else null
			if root is CbCharacter:
				var result: Dictionary = root.bake_to("")
				if result.get("ok", false):
					baked += 1
					if not String(result.get("warnings", "")).is_empty():
						push_warning("%s: %s" % [path, result["warnings"]])
				else:
					_failures += 1
					printerr("%s: %s" % [path, result.get("error", "the bake failed")])
			if root != null:
				root.free()
	print("characters: %d baked%s" % [baked, "" if _failures == 0 else ", %d FAILED" % _failures])
	quit(0 if _failures == 0 else 1)
