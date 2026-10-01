extends SceneTree
## Bakes the bodies of a mod's items, run inside the mod's client project:
##
##   godot --headless --path server_mods/melee/client --script <repo>/godot/addons/cinderbox_maps/bake_items.gd
##
## For every CbItemLook in res://vfx/reactions*.tscn, the CbItemBody in its scene (a box or a sphere,
## placed from the grip, with a mass) is written to res://items/<kind>.cfg. The server reads that
## file from the mod's workshop item, so the body an item has when it lies in the world is the one
## drawn in the editor. tools\pack_mod.ps1 runs this before packing; the files are committed with
## the mod, so tests and servers built from source have them too.
##
## An item without a CbItemBody is left alone: it keeps the body its mod declared in code, or a
## small box.

var _failures := 0


func _looks(node: Node, out: Array) -> void:
	if node is CbItemLook:
		out.append(node)
	for child in node.get_children():
		_looks(child, out)


func _body(node: Node) -> CbItemBody:
	if node is CbItemBody:
		return node
	for child in node.get_children():
		var found := _body(child)
		if found != null:
			return found
	return null


func _initialize() -> void:
	var baked := 0
	if DirAccess.dir_exists_absolute("res://vfx"):
		for file in DirAccess.get_files_at("res://vfx"):
			if not (file.begins_with("reactions") and file.ends_with(".tscn")):
				continue
			var packed := load("res://vfx/%s" % file) as PackedScene
			if packed == null:
				continue
			var root := packed.instantiate()
			var looks := []
			_looks(root, looks)
			for look in looks:
				baked += _bake(look.kind, look.scene)
			root.free()
	print("item bodies: %d baked%s" % [baked, "" if _failures == 0 else ", %d FAILED" % _failures])
	quit(0 if _failures == 0 else 1)


func _bake(kind: String, scene_path: String) -> int:
	var packed := load(scene_path) as PackedScene if ResourceLoader.exists(scene_path) else null
	if kind.is_empty() or packed == null:
		return 0
	var item := packed.instantiate()
	var body := _body(item)
	if body == null:
		print("  %s: no CbItemBody in %s (it keeps its declared body)" % [kind, scene_path])
		item.free()
		return 0
	var result: Dictionary = body.bake()
	item.free()
	if String(result["text"]).is_empty():
		push_error("%s: %s (%s)" % [kind, result["error"], scene_path])
		_failures += 1
		return 0
	DirAccess.make_dir_recursive_absolute("res://items")
	var file := FileAccess.open("res://items/%s.cfg" % kind, FileAccess.WRITE)
	file.store_string(result["text"])
	file.close()
	print("  %s -> res://items/%s.cfg" % [kind, kind])
	return 1
