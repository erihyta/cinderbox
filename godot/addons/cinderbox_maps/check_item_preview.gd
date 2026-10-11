extends SceneTree
## Draws an item's in-hand preview (CbItem.preview_hold) outside the editor and saves the picture:
##
##   godot --path server_mods/<mod>/client --script <repo>/godot/addons/cinderbox_maps/check_item_preview.gd -- --scene=res://prefabs/bat.tscn --out=<file.png> [--aim]
##
## The stand-in figure should hold the item by its carrying grip. Exit code 1 when there is no figure,
## or when what is in its hand is not exactly one copy of the item.

var _item: Node3D
var _out := ""
var _frames := 0


func _initialize() -> void:
	var scene := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--scene="):
			scene = arg.substr(8)
		elif arg.begins_with("--out="):
			_out = arg.substr(6)
	_item = (load(scene) as PackedScene).instantiate()
	_item.set_meta("cb_preview", true)
	root.add_child(_item)
	_item.set("preview_aim", OS.get_cmdline_user_args().has("--aim"))
	_item.set("preview_hold", true)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-50, 30, 0)
	root.add_child(light)
	var camera := Camera3D.new()
	root.add_child(camera)
	camera.look_at_from_position(Vector3(-0.6, 0.5, -2.4), Vector3(0.6, -0.1, 0))


func _process(_delta: float) -> bool:
	_frames += 1
	# (Long enough for the copy in the hand to have been made again a few times.)
	if _frames < 100:
		return false
	var figure: Node = null
	for child in _item.get_children(true):
		if child.name == "HoldPreview":
			figure = child
	if figure == null:
		printerr("no figure: the preview did not start")
		quit(1)
		return true
	print("figure at ", (figure as Node3D).position)
	# One copy of the item in the hand, with as many meshes as the item has: no more.
	var own := _item.find_children("*", "MeshInstance3D", true, true).size()
	var copies := 0
	var parts := 0
	var kept := _item.get_children(false)
	for child in _item.get_children(true):
		# (The preview's own children are internal ones; the copy is the one that is not the figure.)
		if not kept.has(child) and child != figure and not child.is_queued_for_deletion():
			copies += 1
			parts += child.get_child_count()
	print("copies in the hand: %d, with %d meshes (the item has %d)" % [copies, parts, own])
	if copies != 1 or parts != own:
		printerr("the copy in the hand is not one copy of the item")
		quit(1)
		return true
	# An item whose other hand is at a marker: that hand is at the marker of the copy in the hand
	# (within a few centimetres: the wrist is at the marker, or the palm when it is turned with it).
	var other := _item.get_node_or_null(_item.get("other_grip")) as Node3D if int(_item.get("other_hand")) in [1, 2] else null
	if other != null:
		var copy: Node3D = null
		for child in _item.get_children(true):
			if not kept.has(child) and child != figure and not child.is_queued_for_deletion():
				copy = child
		var marker: Vector3 = copy.global_transform * (_item.global_transform.affine_inverse() * other.global_position)
		var hand: Vector3 = figure.call("get_joint_global_transform", "LeftHand").origin
		print("the other hand is %.0f mm from its marker" % (hand.distance_to(marker) * 1000.0))
		if hand.distance_to(marker) > 0.12:
			printerr("the other hand is not at its marker")
			quit(1)
			return true
	if _out != "":
		root.get_texture().get_image().save_png(_out)
	quit(0)
	return true
