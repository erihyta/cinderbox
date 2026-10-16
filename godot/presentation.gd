extends RefCounted
## What is shown of the server's mods besides the world: world reactions, items, the mods' HUDs and
## the items' own, and the screen effects reactions ask for (a shake, a flash).
##
## What plays when is data: every res://vfx/reactions*.tscn is loaded once and handed to the client,
## which runs its CbReaction nodes on every event (spawns, jumps, impacts, mod events, the local
## player's presses). A mod adds reactions by adding a file of its own instead of replacing the
## game's. Screen effects come back here, since the camera and the overlay are the game's.

var game: Node
var item_huds: Array[Node] = []
## Items' own HUDs (CbItem.hud), by kind: {scene, carried, node}. Shown by update_kind_huds.
var _kind_huds := {}
## The camera's shake now (view_camera.gd adds it), and how fast it fades.
var shake := 0.0
var _shake_decay := 1.0
var _flash := 0.0
var _flash_decay := 1.0
var _flash_color := Color.WHITE
var _flash_rect: ColorRect


func _init(owner: Node) -> void:
	game = owner


func load_reactions() -> void:
	var names := []
	for file in DirAccess.get_files_at("res://vfx"):
		# Exported games list resources under their remapped names.
		var clean: String = file.trim_suffix(".remap")
		if clean.begins_with("reactions") and (clean.ends_with(".tscn") or clean.ends_with(".scn")):
			names.append(clean)
	names.sort()
	for file in names:
		var scene := ResourceLoader.load("res://vfx/%s" % file, "PackedScene", ResourceLoader.CACHE_MODE_REPLACE) as PackedScene
		if scene == null:
			push_warning("vfx/%s is not a scene" % file)
			continue
		var reactions := CbDirector.instantiate(scene)
		if reactions:
			game.client.add_world_scene(reactions)
	print("world reactions: ", names)
	load_item_kinds()


## Items: every res://items/<kind>.cfg (baked from a scene whose root is a CbItem) says which scene
## that kind is drawn as, what it is called and how it sits in first person.
func load_item_kinds() -> void:
	var kinds := []
	_kind_huds.clear()
	if DirAccess.dir_exists_absolute("res://items"):
		for file in DirAccess.get_files_at("res://items"):
			if file.ends_with(".cfg"):
				var kind: String = file.trim_suffix(".cfg")
				var config := FileAccess.get_file_as_string("res://items/%s" % file)
				game.client.add_item(kind, config)
				kinds.append(kind)
				# Its own HUD ("hud <scene>", "hudshows hand|carried"): shown by update_kind_huds.
				var hud := ""
				var carried := false
				for line in config.split("\n"):
					var words := line.strip_edges().split(" ", false)
					if words.size() == 2 and words[0] == "hud":
						hud = words[1]
					elif words.size() == 2 and words[0] == "hudshows":
						carried = words[1] == "carried"
				if hud != "":
					_kind_huds[kind] = {"scene": hud, "carried": carried, "node": null}
	kinds.sort()
	print("items: ", kinds, "" if _kind_huds.is_empty() else ", with their own HUDs: %s" % ", ".join(_kind_huds.keys()))


## Reactions and item HUDs, loaded again now that items may have added or replaced some.
func reload() -> void:
	game.client.clear_world_scenes()
	for kind in _kind_huds:
		if _kind_huds[kind]["node"] != null:
			_kind_huds[kind]["node"].queue_free()
	load_reactions()
	for node in item_huds:
		node.queue_free()
	item_huds.clear()
	var names := []
	for file in DirAccess.get_files_at("res://ui"):
		var clean: String = file.trim_suffix(".remap")
		if clean.begins_with("hud_") and clean.ends_with(".tscn"):
			names.append(clean)
	names.sort()
	for file in names:
		var scene := ResourceLoader.load("res://ui/%s" % file, "PackedScene", ResourceLoader.CACHE_MODE_REPLACE) as PackedScene
		var node := CbDirector.instantiate(scene)
		if node:
			game.add_child(node)
			item_huds.append(node)
	print("item HUDs: ", names)


## An item's own HUD (CbItem.hud): shown once for the viewer's own player while it has one in the
## hand, or carries one at all (hud_shows), and freed when it does not.
func update_kind_huds() -> void:
	for kind in _kind_huds:
		var entry: Dictionary = _kind_huds[kind]
		var asked: String = ("carried." if entry["carried"] else "") + kind
		var want: bool = game._joined and game.client.check_local_conditions(PackedStringArray([asked]))
		var node: Node = entry["node"]
		if want and node == null:
			var scene := ResourceLoader.load(entry["scene"], "PackedScene") as PackedScene
			node = CbDirector.instantiate(scene) if scene else null
			if node:
				game.add_child(node)
				entry["node"] = node
		elif not want and node != null:
			node.queue_free()
			entry["node"] = null


func make_flash_overlay() -> void:
	# Its own layer, so a mod replacing the HUD cannot remove it by accident.
	var layer := CanvasLayer.new()
	layer.layer = 100
	game.add_child(layer)
	_flash_rect = ColorRect.new()
	_flash_rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	_flash_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_flash_rect.color = Color(1, 1, 1, 0)
	_flash_rect.visible = false
	layer.add_child(_flash_rect)


func on_screen_effect(amount: float, shake_time: float, flash_color: Color, flash_time: float) -> void:
	if amount > 0.0:
		shake = max(shake, amount)
		_shake_decay = amount / max(shake_time, 0.05)
	if flash_color.a > 0.0:
		_flash_color = flash_color
		_flash = flash_color.a
		_flash_decay = flash_color.a / max(flash_time, 0.02)


func update_screen_effects(delta: float) -> void:
	shake = max(0.0, shake - _shake_decay * delta)
	if _flash > 0.0:
		_flash = max(0.0, _flash - _flash_decay * delta)
		_flash_rect.color = Color(_flash_color.r, _flash_color.g, _flash_color.b, _flash)
		_flash_rect.visible = _flash > 0.0


func on_visual_spawned(_visual_id: int, net_id: int, kind: String, node: Node3D, _position: Vector3,
		_with_effect: bool, _template_name: String) -> void:
	if kind == "prop" and node != null and node.has_meta("tint_by_net_id"):
		_tint(node, net_id)


func _tint(node: Node3D, net_id: int) -> void:
	var mesh := node as MeshInstance3D
	if mesh == null:
		return
	var h := hash(net_id)
	var color := Color.from_hsv(float(h % 360) / 360.0, 0.45, 0.85)
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = 0.7
	mesh.material_override = material
