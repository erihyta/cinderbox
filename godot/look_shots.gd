extends RefCounted
## Pictures of the same moments of a view file from the same places (--look-shots=DIR with --view=FILE),
## to compare how the world is drawn before and after a change to the drawing (M260): at each of
## --look-shots-at=SECONDS,... (default 6,10,14) the playback is sought there and paused, and from each
## place the frame is saved as DIR/<seconds>_<place>.png. Then the game quits.
##
## The places, from where the players are at that moment: the crowd from above ("crowd"), the player
## nearest its middle from the front and from its right ("front", "side", 2.4 m off at chest height),
## and the crowd from 40 m off and above ("far": the far characters' look, their shadows).

var game: Node
var dir := ""
var moments: Array[float] = []
var _moment := 0
var _state := "wait" # wait, settle, shoot, done
var _frames := 0
var _places: Array = [] # [name, Transform3D]
var _place := 0
var _camera := Transform3D()
var _have_camera := false


func _init(owner: Node, args: Dictionary) -> void:
	game = owner
	dir = String(args["look-shots"])
	for part in String(args.get("look-shots-at", "6,10,14")).split(",", false):
		moments.append(float(part))
	DirAccess.make_dir_recursive_absolute(dir)


## Placed just before the frame is drawn, after the game's own camera (RenderingServer.frame_pre_draw).
func place_camera() -> void:
	if _have_camera:
		game.camera.global_transform = _camera
		# (Passed on to the renderer now: the frame's moves already were.)
		game.camera.force_update_transform()


func _looking(from: Vector3, at: Vector3) -> Transform3D:
	return Transform3D(Basis(), from).looking_at(at, Vector3.UP)


func _plan_places() -> void:
	var client = game.client
	var positions: Array[Vector3] = []
	var nodes: Array[Node3D] = []
	for id in client.get_players():
		var node: Node3D = client.get_entity_node(id)
		if node != null and node.is_visible_in_tree():
			positions.append(node.global_position)
			nodes.append(node)
	_places.clear()
	if positions.is_empty():
		return
	var middle := Vector3.ZERO
	for p in positions:
		middle += p
	middle /= float(positions.size())
	var nearest := 0
	for i in positions.size():
		if positions[i].distance_to(middle) < positions[nearest].distance_to(middle):
			nearest = i
	var body: Node3D = nodes[nearest]
	var at := body.global_position
	var facing := body.global_transform.basis.z.normalized() # (a character faces +Z)
	var right := -body.global_transform.basis.x.normalized()
	_places.append(["crowd", _looking(middle + Vector3(0, 9, 12), middle + Vector3(0, 1, 0))])
	_places.append(["front", _looking(at + facing * 2.4 + Vector3(0, 1.3, 0), at + Vector3(0, 1.0, 0))])
	_places.append(["side", _looking(at + right * 2.4 + Vector3(0, 1.3, 0), at + Vector3(0, 1.0, 0))])
	_places.append(["far", _looking(middle + Vector3(0, 24, 30), middle + Vector3(0, 1, 0))])


func step() -> void:
	var client = game.client
	match _state:
		"wait":
			# Until the playback has its players.
			if client.get_players().size() == 0:
				return
			if game.hud != null:
				game.hud.visible = false
			game.show_debug = false
			_seek()
		"settle":
			_frames -= 1
			if _frames > 0:
				return
			_plan_places()
			_place = 0
			_aim()
		"shoot":
			_frames -= 1
			if _frames > 0:
				return
			var name: String = _places[_place][0]
			var path := "%s/%s_%s.png" % [dir, String.num(moments[_moment], 1).replace(".", "_"), name]
			game.get_viewport().get_texture().get_image().save_png(path)
			print("shot: ", path)
			_place += 1
			if _place < _places.size():
				_aim()
				return
			_moment += 1
			if _moment < moments.size():
				_seek()
				return
			_state = "done"
			print("shots done: ", dir)
			game.get_tree().quit(0)


func _seek() -> void:
	_have_camera = false
	game.client.control("pause", 0)
	game.client.control("seek", moments[_moment])
	game.client.control("pause", 1)
	# Long enough for the frame to be drawn as it settles (the catch-up of clips first seen late,
	# what interpolates).
	_frames = 45
	_state = "settle"


func _aim() -> void:
	if _place >= _places.size():
		_moment += 1
		if _moment < moments.size():
			_seek()
		else:
			game.get_tree().quit(0)
		return
	_camera = _places[_place][1]
	_have_camera = true
	_frames = 6
	_state = "shoot"
