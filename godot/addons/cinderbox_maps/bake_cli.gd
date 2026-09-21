extends SceneTree
## Headless map baking, used by tools/bake_map.ps1:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/bake_cli.gd -- \
##         --scene=res://maps/example_arena.tscn [--out=res://maps/example_arena.cbmap]
##
## The baker accumulates transforms itself, so the scene does not need to be inside a tree.


func _initialize() -> void:
	var args := {}
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--") and arg.contains("="):
			var eq := arg.find("=")
			args[arg.substr(2, eq - 2)] = arg.substr(eq + 1)

	var scene_path: String = args.get("scene", "")
	if scene_path.is_empty():
		printerr("bake_cli: --scene=res://maps/<name>.tscn is required")
		quit(2)
		return

	var out_path: String = args.get("out", "res://maps/%s.cbmap" % scene_path.get_file().get_basename())
	var packed: PackedScene = load(scene_path)
	if packed == null:
		printerr("bake_cli: cannot load ", scene_path)
		quit(2)
		return

	var root := packed.instantiate()
	var result: Dictionary = CinderboxMapBaker.new().bake(root, out_path)
	for warning in result.get("warnings", []):
		print("warning: ", warning)
	root.free()

	if not result.get("ok", false):
		printerr("bake failed: ", result.get("error", "unknown error"))
		quit(2)
		return

	print("baked %s: name %s, %d statics, %d props, %d templates, %d instances, %d bytes, hash %s" % [
		out_path, result["name"], result["statics"], result["props"], result["templates"], result["instances"],
		result["bytes"], result["hash"]])
	quit(0)
