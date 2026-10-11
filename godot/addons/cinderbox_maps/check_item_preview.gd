extends SceneTree
## Draws an item's in-hand preview (CbItem.preview_hold) outside the editor and saves the picture:
##
##   godot --path server_mods/<mod>/client --script <repo>/godot/addons/cinderbox_maps/check_item_preview.gd -- --scene=res://prefabs/bat.tscn --out=<file.png> [--aim]
##
## The stand-in figure should hold the item by its carrying grip. Exit code 1 when there is no figure.

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
	if _frames < 20:
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
	if _out != "":
		root.get_texture().get_image().save_png(_out)
	quit(0)
	return true
