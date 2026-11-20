extends SceneTree
## Cuts the breakables of scenes into chunks, as the editor's Fracture button does, and saves them:
## every CbBreakable that says how (its "cinderbox_fracture" metadata: pieces, seed, crowd, and
## optionally density, kg per cubic metre, for its chunks), its pieces replaced by the new ones.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/fracture_scenes.gd -- res://maps/arena_parts
##
## Each argument a scene or a folder (every .tscn in it). tools/make_arena.py writes the arena's
## walls so (godot/maps/arena_parts) and the arena places them; this cuts them, then bake the map.

const Fracture := preload("res://addons/cinderbox_maps/fracture.gd")


func _initialize() -> void:
	var scenes := []
	for arg in OS.get_cmdline_user_args():
		if arg.ends_with(".tscn"):
			scenes.append(arg)
		else:
			var dir := DirAccess.open(arg)
			if dir == null:
				printerr("fracture_scenes: cannot open ", arg)
				quit(2)
				return
			for file in dir.get_files():
				if file.ends_with(".tscn"):
					scenes.append(arg.path_join(file))
	if scenes.is_empty():
		printerr("fracture_scenes: name scenes or folders of them")
		quit(2)
		return
	var failed := 0
	for path in scenes:
		failed += _fracture_scene(path)
	quit(1 if failed > 0 else 0)


func _fracture_scene(path: String) -> int:
	var packed: PackedScene = load(path)
	if packed == null:
		printerr("cannot load ", path)
		return 1
	var root := packed.instantiate()
	var walls := []
	_find(root, root, walls)
	var made := 0
	for wall in walls:
		var settings: Dictionary = wall.get_meta("cinderbox_fracture")
		var result: Dictionary = Fracture.fracture(wall, int(settings.get("pieces", 24)), int(settings.get("seed", 1)), float(settings.get("crowd", 0.0)))
		if result.error != "":
			printerr("%s: %s: %s" % [path, wall.name, result.error])
			root.free()
			return 1
		for child in wall.get_children():
			if child is CbProp:
				wall.remove_child(child)
				child.free()
		for piece in result.pieces:
			if settings.has("density"):
				piece.density = float(settings.density)
			wall.add_child(piece)
			_own(piece, root)
		made += result.pieces.size()
	var scene := PackedScene.new()
	scene.pack(root)
	var saved := ResourceSaver.save(scene, path)
	root.free()
	print("%s: %d walls, %d chunks (%s)" % [path, walls.size(), made, error_string(saved)])
	return 0 if saved == OK else 1


# The breakables that say how to cut them: the scene's own nodes (not ones another scene places).
func _find(node: Node, root: Node, out: Array) -> void:
	if node is CbBreakable and node.has_meta("cinderbox_fracture") and (node == root or node.owner == root):
		out.append(node)
	for child in node.get_children():
		_find(child, root, out)


func _own(node: Node, owner: Node) -> void:
	node.owner = owner
	for child in node.get_children():
		_own(child, owner)
