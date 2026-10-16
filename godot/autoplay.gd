extends RefCounted
## The unattended player (--autoplay=SECONDS): it plays for that long by a script of its own, saves
## screenshots along the way (--screenshot=FILE --screenshot-every=SECONDS), says what it saw and
## quits: 0 when nothing desynced.
##
## --view-probe[=N]: instead it goes into first person (slot N out) and walks and turns by a plan,
## and the run reports how far its hand moved on the screen in each part (it should stand still).
## --probe-frames=DIR keeps a run of frames from each part, --probe-use uses what it has out once,
## --probe-pitch=DEGREES looks that far up (down: negative) all the while.

## What a player asks of its slots (the simulation's SlotIntent).
const INTENT_SELECT := 1
const Host := preload("res://host.gd")

var game: Node
## How long the run lasts (0: nobody is playing unattended).
var seconds := 0.0
var view_probe := false
var _probe_slot := 0
var _probe_frames_dir := ""
var _probe_use := false
var _probe_body := {}
var _probe_frames := {} # part -> [Image]
var _probe_part_since := {} # part -> when it began
## view_probe: per part of the plan, the hand's place in the camera's frame: [least, most] of each axis.
var _probe := {}
var screenshot := ""
var playing_since := -1.0
var rng := RandomNumberGenerator.new()
var auto_move := Vector2.ZERO
var screenshot_every := 0.0
var _next_screenshot := 0.0
var _screenshots := 0
var _event_counts := {}
## The viewer's guesses (CbPrediction) made, and the ones the server never answered (taken back).
var _guesses := 0
var _taken_back := {}


func _init(owner: Node, args: Dictionary) -> void:
	game = owner
	seconds = float(args.get("autoplay", "0"))
	view_probe = args.has("view-probe")
	_probe_slot = int(args.get("view-probe", "0")) if view_probe else 0
	# --probe-frames=DIR: the probe also keeps what was drawn, a run of frames from each part of
	# its plan, so what is on the screen can be measured (tools outside the game compare them).
	_probe_frames_dir = args.get("probe-frames", "")
	_probe_use = args.has("probe-use")
	if view_probe and _probe_frames_dir != "":
		RenderingServer.frame_post_draw.connect(_probe_capture)
	screenshot = args.get("screenshot", "")
	screenshot_every = float(args.get("screenshot-every", "0"))
	rng.seed = Time.get_ticks_usec()
	var director: CbDirector = game.get_node("Client").get_director()
	director.predicted.connect(func(_cue): _guesses += 1)
	director.taken_back.connect(func(cue): _taken_back[cue] = _taken_back.get(cue, 0) + 1)


## What the unattended player does this frame: {move, jump, sprint, use, actions}. It turns the
## game's look (yaw, pitch, first person) itself.
func steer(delta: float) -> Dictionary:
	var move := Vector2.ZERO
	var jump := false
	var sprint := false
	var use := false
	var actions := 0
	var keys = game.keys
	# Scripted player for unattended runs: props first, then the pistol, then the bat (if the
	# server runs the melee mod).
	if rng.randf() < delta * 1.5:
		auto_move = Vector2(rng.randf_range(-1, 1), rng.randf_range(0.2, 1))
	move = auto_move
	sprint = true
	jump = rng.randf() < delta * 1.0
	game.yaw += delta * 0.6
	if view_probe:
		var plan := _probe_plan(Time.get_ticks_msec() / 1000.0 - playing_since if playing_since >= 0.0 else 0.0)
		game.yaw += delta * (float(plan[2]) - 0.6)
		move = plan[1]
		sprint = bool(plan[3])
		jump = false
		game.first_person = true
		if plan[0] == "looking up and down":
			game.pitch = 0.9 * sin((Time.get_ticks_msec() / 1000.0) * 3.0)
		if game.args.has("probe-pitch"):
			game.pitch = deg_to_rad(float(game.args["probe-pitch"]))
	var elapsed := Time.get_ticks_msec() / 1000.0 - playing_since if playing_since >= 0.0 else 0.0
	# A hook, if the server has one: a press of its own key throws it, the next lets it go (one
	# press every two seconds).
	if view_probe:
		# The probe's player only walks and turns; with "--view-probe=N" it has slot N out.
		if _probe_slot > 0:
			_select(_probe_slot - 1)
	# Before the inventory screen opens, to the nearest chest (a container), looking at it: the
	# screen then opens it too.
	var to_chest := not view_probe and _to_chest(elapsed)
	if to_chest:
		move = auto_move
		jump = false
		sprint = false
	elif int(elapsed / 2.0) != _hooks and game.client.get_mod_names().has("grapple"):
		_hooks = int(elapsed / 2.0)
		actions |= keys.action_bit("grapple.hook.use")
	if view_probe:
		# With --probe-use it uses what it has out once, a second into standing still: the
		# frames kept of that part are the use, from the press on.
		use = _probe_use and elapsed >= 4.0 and elapsed < 4.1
	elif elapsed < seconds * 0.4:
		if rng.randf() < delta * 2.0:
			actions |= keys.action_bit("spawn_prop")
		# A grenade now and then, if the server has them: its own key throws one, from wherever
		# it is carried.
		if rng.randf() < delta * 1.0:
			actions |= keys.action_bit("frag.grenade.use")
	elif elapsed < seconds * 0.7:
		_select(0)
		use = rng.randf() < delta * 3.0
	else:
		_select(1)
		use = rng.randf() < delta * 2.0
	# The inventory screen, if a mod brings one (a CbKey named "inventory"): opened in the
	# middle of the run, its first slot dragged onto its third (they trade places), closed.
	if not view_probe:
		_bag(elapsed - seconds * 0.45)
	# Hold the scores key at the end (a mod's CbKey names it), so screenshots show them too.
	if elapsed > seconds * 0.8 and InputMap.has_action("cb_scores") and not Input.is_action_pressed("cb_scores"):
		Input.action_press("cb_scores")
	return {"move": move, "jump": jump, "sprint": sprint, "use": use, "actions": actions}


## The unattended player's visit to the inventory screen: what it has done of it so far.
var _bag_step := 0


func _bag(since: float) -> void:
	if not InputMap.has_action("cb_inventory"):
		return
	var due := [0.0, 0.6, 0.7, 0.8, 2.6]
	if _bag_step >= due.size() or since < due[_bag_step]:
		return
	# (With a chest about, the screen opens once the player stands at it, or two seconds late.)
	if _bag_step == 0 and game.client.get_containers().size() > 0 and not _chest_near and since < 2.0:
		return
	# The bag's cells, in order (a CbDrag in each; the hotbar's are not the bag's).
	var cells := []
	for drag in game.get_tree().root.find_children("*", "CbDrag", true, false):
		var cell := drag.get_parent() as Control
		var in_bag := false
		var up: Node = cell
		while up != null and not in_bag:
			in_bag = up.name == "Bag"
			up = up.get_parent()
		if cell != null and in_bag and cell.is_visible_in_tree() and not cells.has(cell):
			cells.append(cell)
	match _bag_step:
		0, 4:
			# The key itself, as a keyboard sends it (down, then up): it switches the screen.
			for bound in InputMap.action_get_events("cb_inventory"):
				for pressed in [true, false]:
					var key := (bound as InputEvent).duplicate() as InputEventKey
					if key != null:
						key.pressed = pressed
						Input.parse_input_event(key)
			print("autoplay: pressed the inventory key")
		1, 2, 3:
			# The first slot dragged onto the third: pressed, moved there, let go (a frame apart).
			if cells.size() >= 3:
				var from := (cells[0] as Control).get_global_rect().get_center()
				var to := (cells[2] as Control).get_global_rect().get_center()
				if _bag_step == 1:
					_mouse_button(from, true)
					_mouse_motion(from + Vector2(12, 0))
				elif _bag_step == 2:
					_mouse_motion(from.lerp(to, 0.5))
					_mouse_motion(to)
				else:
					_mouse_button(to, false)
					print("autoplay: dragged slot 1 onto slot 3 of ", cells.size(), " in the inventory screen")
	_bag_step += 1


## Where the unattended player's mouse was last (a drag is seen from how far the mouse moved).
var _mouse := Vector2.ZERO


func _mouse_button(at: Vector2, pressed: bool) -> void:
	_mouse = at
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = pressed
	event.button_mask = MOUSE_BUTTON_MASK_LEFT if pressed else 0
	event.position = at
	event.global_position = at
	Input.parse_input_event(event)


func _mouse_motion(at: Vector2) -> void:
	var event := InputEventMouseMotion.new()
	event.button_mask = MOUSE_BUTTON_MASK_LEFT
	event.relative = at - _mouse
	_mouse = at
	event.position = at
	event.global_position = at
	Input.parse_input_event(event)


var _chest_near := false


## The unattended player walks to the nearest container and looks at it, from a little before the
## inventory screen opens until it closes: true meanwhile.
func _to_chest(elapsed: float) -> bool:
	if elapsed < seconds * 0.2 or elapsed > seconds * 0.45 + 4.8:
		return false
	var client = game.client
	var me: Node3D = client.get_entity_node(client.get_local_net_id())
	var nearest: Node3D = null
	for id in client.get_containers():
		var node: Node3D = client.get_entity_node(id)
		if node != null and me != null and (nearest == null or node.global_position.distance_to(me.global_position) < nearest.global_position.distance_to(me.global_position)):
			nearest = node
	if nearest == null or me == null:
		return false
	var to := nearest.global_position - (me.global_position + Vector3(0, 1.5, 0))
	var flat := Vector2(to.x, to.z).length()
	game.yaw = atan2(-to.x, -to.z)
	game.pitch = atan2(to.y, flat)
	_chest_near = flat <= 1.8
	auto_move = Vector2.ZERO if _chest_near else Vector2(0, 1)
	return true


## The unattended player takes a slot's item out, once.
var _slot := -1
var _hooks := 0


func _select(slot: int) -> void:
	if slot != _slot:
		_slot = slot
		game.client.send_intent(INTENT_SELECT, slot, 0)


## view_probe: what the unattended player does `at` seconds into the run: [name, move, turn rate, sprint].
func _probe_plan(at: float) -> Array:
	if at < 3.0:
		return ["settle", Vector2.ZERO, 0.0, false]
	if at < 6.0:
		return ["standing still", Vector2.ZERO, 0.0, false]
	if at < 9.0:
		return ["walking ahead", Vector2(0, 1), 0.0, false]
	if at < 12.0:
		return ["sprinting ahead", Vector2(0, 1), 0.0, true]
	if at < 15.0:
		return ["strafing left and right", Vector2(1 if fmod(at, 1.0) < 0.5 else -1, 0), 0.0, false]
	if at < 18.0:
		return ["standing, turning", Vector2.ZERO, 2.5, false]
	if at < 21.0:
		return ["walking and turning", Vector2(0, 1), 2.5, false]
	return ["looking up and down", Vector2.ZERO, 0.0, false]


## view_probe with --probe-frames: the frame just drawn, half size, 48 in a row from one second
## into each part of the plan.
func _probe_capture() -> void:
	if playing_since < 0.0 or not game.client.first_person:
		return
	var now := Time.get_ticks_msec() / 1000.0 - playing_since
	var part: String = _probe_plan(now)[0]
	if part == "settle":
		return
	if not _probe_part_since.has(part):
		_probe_part_since[part] = now
		_probe_frames[part] = []
	if now - float(_probe_part_since[part]) < 1.0 or (_probe_frames[part] as Array).size() >= 48:
		return
	var image: Image = game.get_viewport().get_texture().get_image()
	image.resize(image.get_width() / 2, image.get_height() / 2, Image.INTERPOLATE_BILINEAR)
	(_probe_frames[part] as Array).append(image)


## view_probe: where the right hand is in the camera's own frame, this drawn frame.
func probe_sample() -> void:
	var client = game.client
	if not view_probe or playing_since < 0.0 or not client.first_person:
		return
	var part: String = _probe_plan(Time.get_ticks_msec() / 1000.0 - playing_since)[0]
	if part == "settle":
		return
	var camera: Camera3D = game.camera
	var hand: Vector3 = camera.global_transform.affine_inverse() * client.get_bone_position(client.get_local_net_id(), "RightHand")
	# The body's own place and facing against the camera's, to tell what moves when the hand does.
	var body: Node3D = client.get_entity_node(client.get_local_net_id())
	var origin: Vector3 = camera.global_transform.affine_inverse() * body.global_position
	var turn: float = wrapf(body.global_rotation.y - camera.global_rotation.y, -PI, PI)
	if not _probe_body.has(part):
		_probe_body[part] = [origin, origin, turn, turn]
	var was: Array = _probe_body[part]
	_probe_body[part] = [Vector3(minf(was[0].x, origin.x), minf(was[0].y, origin.y), minf(was[0].z, origin.z)),
		Vector3(maxf(was[1].x, origin.x), maxf(was[1].y, origin.y), maxf(was[1].z, origin.z)), minf(was[2], turn), maxf(was[3], turn)]
	if not _probe.has(part):
		_probe[part] = [hand, hand, 0]
	var seen: Array = _probe[part]
	_probe[part] = [Vector3(minf(seen[0].x, hand.x), minf(seen[0].y, hand.y), minf(seen[0].z, hand.z)),
		Vector3(maxf(seen[1].x, hand.x), maxf(seen[1].y, hand.y), maxf(seen[1].z, hand.z)), seen[2] + 1]


func on_mod_event(name: String, _a: int, _b: int, _value: int, _position: Vector3, _vector: Vector3) -> void:
	_event_counts[name] = _event_counts.get(name, 0) + 1


## Screenshots along the way, and at the end what the run saw; then the game quits.
func finish() -> void:
	if seconds <= 0.0:
		return
	var client = game.client
	if playing_since < 0.0:
		if client.get_source_state() == "playing":
			playing_since = Time.get_ticks_msec() / 1000.0
		return
	var elapsed := Time.get_ticks_msec() / 1000.0 - playing_since
	if screenshot != "" and screenshot_every > 0.0 and elapsed >= _next_screenshot:
		_next_screenshot = elapsed + screenshot_every
		_screenshots += 1
		var path := "%s_%d.png" % [screenshot.trim_suffix(".png"), _screenshots]
		game.get_viewport().get_texture().get_image().save_png(path)
	if elapsed < seconds:
		return
	seconds = 0.0
	if screenshot != "":
		await RenderingServer.frame_post_draw
		game.get_viewport().get_texture().get_image().save_png(screenshot)
	var stats: Dictionary = client.get_stats()
	print("autoplay done: %s, checksums ok %d, desyncs %d, fingerprint %s, fp ok %s" % [
		stats.get("state"), stats.get("checksums_verified", 0), stats.get("desyncs", 0), stats.get("fingerprint", "none"), stats.get("fp_environment_ok", "not simulating")])
	print("mod events seen: ", _event_counts)
	print("guesses: %d, taken back %d %s, of those answered late %d" % [_guesses, _taken_back.values().reduce(func(a, b): return a + b, 0),
		_taken_back, client.get_director().get_late_answer_count()])
	# What the scene costs: the reactions and predictions listening (a copy lying in the world has
	# only its own).
	for node in game.get_tree().root.find_children("*", "CbDirector", true, false):
		print("listening: %d reactions and predictions" % node.get_listener_count())
		break
	for part in _probe_frames:
		DirAccess.make_dir_recursive_absolute(_probe_frames_dir)
		var frames: Array = _probe_frames[part]
		for i in frames.size():
			(frames[i] as Image).save_png("%s/%s_%02d.png" % [_probe_frames_dir, String(part).replace(" ", "_").replace(",", ""), i])
	for part in _probe:
		var moved: Vector3 = (_probe[part][1] - _probe[part][0]) * 100.0
		print("view probe, %-24s the hand moved %.2f mm across, %.2f mm up and down, %.2f mm in depth on the screen (%d frames)" % [
			part + ":", moved.x * 10.0, moved.y * 10.0, moved.z * 10.0, _probe[part][2]])
		var middle: Vector3 = (_probe[part][0] + _probe[part][1]) * 0.5
		print("            %-24s the hand is %.0f mm to the right, %.0f mm up, %.0f mm ahead of the camera" % ["", middle.x * 1000.0, middle.y * 1000.0, -middle.z * 1000.0])
		var body_moved: Vector3 = (_probe_body[part][1] - _probe_body[part][0]) * 1000.0
		print("            %-24s the body's origin moved %.2f / %.2f / %.2f mm against the camera, its facing %.3f degrees" % [
			"", body_moved.x, body_moved.y, body_moved.z, rad_to_deg(_probe_body[part][3] - _probe_body[part][2])])
	# What a press of the use button would predict now, and why not (each CbPrediction, by its path).
	print("use would predict: ", client.get_director().explain_press("use"))
	game._stop()
	# (A source that cannot desync, a view file, reports none.)
	Host.stop()
	game.get_tree().quit(0 if stats.get("desyncs", 0) == 0 and stats.get("state") == "playing" else 2)
