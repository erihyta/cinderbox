extends SceneTree
## Checks what a character's `movement` bakes to, on the game's mannequin (nothing of it is changed:
## the bake goes to a folder under user://):
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_movement.gd
##
## A CbCharacter says how it moves with movement parameters by name ("walk_speed": 2.4). The bake
## writes them as "move.<name>" lines of anim.cfg, in the simulation's order; a name that is no
## parameter, or a value outside its range, refuses the bake (and is a warning on the node in the editor).
## Exit code 0 when everything held.

var _failures := 0


func _check(what: String, ok: bool, detail: String = "") -> void:
	print("  %-66s %s  %s" % [what, "ok  " if ok else "FAIL", detail])
	if not ok:
		_failures += 1


func _lines(folder: String) -> PackedStringArray:
	var out := PackedStringArray()
	for line in FileAccess.get_file_as_string(folder + "anim.cfg").split("\n"):
		if line.begins_with("move."):
			out.append(line.strip_edges())
	return out


func _initialize() -> void:
	var scene: PackedScene = load("res://characters/mannequin/character.tscn")
	var character: CbCharacter = scene.instantiate()
	root.add_child(character)
	var folder := "user://check_movement/"

	character.movement = {}
	var baked: Dictionary = character.bake_to(folder)
	_check("a character that says nothing bakes no move lines", baked.ok and _lines(folder).is_empty(), str(baked.get("error", "")))

	# Given out of order: the file has the simulation's order, whatever the dictionary's.
	character.movement = {"max_fall": 20.0, "walk_speed": 2.4}
	baked = character.bake_to(folder)
	var lines := _lines(folder)
	_check("movement bakes to move.<name> lines", baked.ok and lines.size() == 2, str(lines))
	_check("... in the simulation's order", lines.size() == 2 and lines[0] == "move.walk_speed = 2.4" and lines[1] == "move.max_fall = 20", str(lines))

	character.movement = {"fly_speed": 3.0}
	baked = character.bake_to(folder)
	_check("a name that is no parameter refuses the bake", not baked.ok and String(baked.error).contains("fly_speed"), str(baked.get("error", "")))
	_check("... and says which names there are", String(baked.get("error", "")).contains("walk_speed"))

	character.movement = {"gravity": -5.0}
	baked = character.bake_to(folder)
	_check("a value outside its range refuses the bake", not baked.ok and String(baked.error).contains("gravity"), str(baked.get("error", "")))

	character.free()
	var dir := DirAccess.open(folder)
	if dir != null:
		for file in dir.get_files():
			dir.remove(file)
		DirAccess.remove_absolute(ProjectSettings.globalize_path(folder))

	print("movement: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
