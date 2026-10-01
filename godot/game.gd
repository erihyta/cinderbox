extends Node3D
## Base game presentation: joining and leaving, watching recordings, input, camera, HUD binding,
## and loading world reactions.
##
## Everything visual it uses is loaded by path, so mods can replace it:
##   res://ui/hud.tscn          HUD layout. Optional unique nodes: %Stats, %Banner, %Help.
##   res://ui/menu.tscn         the menus (join, in-game, settings), driven by menu.gd.
##   res://ui/hud_*.tscn        HUDs that come with workshop items, laid over the game's.
##   res://vfx/reactions*.tscn  world reactions (CbReaction nodes: scenes, sounds and screen effects
##                              on events) and item looks (CbItemLook nodes), run by the client.
##   res://prefabs/*.tscn       entity visuals (loaded by CinderboxClient).
##
## The game rules live in the server's mods. This script only knows the engine's own controls
## (move, sprint, jump, camera); everything else is an action the server declared, bound to the key
## it suggested, and every event plays whatever the reactions say.
##
## Joining: the game starts in the menu (an address, the servers joined last), unless --host or
## --port says where to go. An attempt that fails comes back to the menu and says why: the address
## does not exist, nobody answered, the server refused, or its workshop items are missing. The
## server announces the workshop items its mods need. They must all be in the local workshop
## (workshop.gd), exactly as announced. Loaded items come before the player's own mods, which are
## loaded again after them.
##
## Leaving reloads this scene, so nothing of one server is left for the next. Resource packs cannot
## be unloaded, though: a server that does not use an item loaded earlier gets a restarted game.
##
## Watching: --replay=FILE plays a recording (cb_server --record) instead of joining. The client
## draws it like a server: the recording names the same workshop items, and the player it follows
## is the local one, with its HUD. Nothing is sent anywhere; the keys steer the playback.
##
## Command line (after `--`): --host=H --port=P --name=NAME --rollback=N --animations=DIR
##                            --replay=FILE
##                            --autoplay=SECONDS --screenshot=FILE --screenshot-every=SECONDS
##                            --mods=DIR --workshop=DIR --config=FILE
## With --screenshot-every, autoplay also saves FILE_1.png, FILE_2.png, ... along the way.

const MOUSE_SENSITIVITY := 0.003
const ACTION_PREFIX := "cb_"
const Boot := preload("res://boot.gd")
const Workshop := preload("res://workshop.gd")
const Menu := preload("res://menu.gd")
## How long a server may take to answer before the attempt is given up.
const JOIN_TIMEOUT := 10.0
## The camera keeps this far off the map's surfaces.
const CAMERA_RADIUS := 0.25

## These outlive the scene, which is reloaded when a server is left.
static var _started := false # the command line's --host has been used
static var _menu_message := "" # why the last server was left, for the menu
static var _loaded_items := {} # sha256 -> true: workshop items loaded into this process

@onready var client: CinderboxClient = $Client
@onready var camera: Camera3D = $Camera

var yaw := PI # facing +Z like the server's spawn orientation
var pitch := -0.35
var distance := 6.0
var _camera_distance := 6.0 # after the map got in the way
var args := {}
var hud: Node
var show_debug := true

var autoplay := 0.0
var screenshot := ""
var playing_since := -1.0
var auto_rng := RandomNumberGenerator.new()
var auto_move := Vector2.ZERO
var screenshot_every := 0.0
var _next_screenshot := 0.0
var _screenshots := 0
var _event_counts := {}
var _item_huds: Array[Node] = []
var _presented := false # this scene has loaded the items' reactions and HUDs
var _refused := "" # why this server cannot be joined

var menu: Menu
var _address := "" # the server being joined or played on, as typed
var _joining_since := -1.0 # seconds; -1: not trying
var _joined := false # this attempt reached "playing"
var _replay := "" # the recording being watched ("": playing on a server)
var _leaving := false # the scene is being replaced

var _actions: Array = [] # [{ name, bit, key }] from the server's mods

var _shake := 0.0
var _shake_decay := 1.0
var _flash := 0.0
var _flash_decay := 1.0
var _flash_color := Color.WHITE
var _flash_rect: ColorRect


func _ready() -> void:
	args = _parse_args()
	if args.has("rollback"):
		client.rollback_min = int(args["rollback"])
		client.rollback_max = int(args["rollback"])
	if args.has("animations"):
		client.animation_dir = args["animations"]
	if not InputMap.has_action("scoreboard"):
		InputMap.add_action("scoreboard")
		var tab := InputEventKey.new()
		tab.physical_keycode = KEY_TAB
		InputMap.action_add_event("scoreboard", tab)
	autoplay = float(args.get("autoplay", "0"))
	screenshot = args.get("screenshot", "")
	screenshot_every = float(args.get("screenshot-every", "0"))
	auto_rng.seed = Time.get_ticks_usec()

	client.visual_spawned.connect(_on_visual_spawned)
	client.mod_event.connect(_on_mod_event)
	client.get_director().screen_effect.connect(_on_screen_effect)
	client.schema_changed.connect(_on_schema_changed)
	client.source_state_changed.connect(func(state): print("source: ", state))

	_load_reactions()
	_make_flash_overlay()

	var hud_scene: PackedScene = load("res://ui/hud.tscn")
	if hud_scene:
		hud = hud_scene.instantiate()
		add_child(hud)

	menu = Menu.new()
	menu.name = "MenuDriver"
	add_child(menu)
	menu.join_requested.connect(_join)
	menu.cancel_requested.connect(_leave.bind(""))
	menu.leave_requested.connect(_leave.bind(""))
	menu.resume_requested.connect(_resume)
	menu.quit_requested.connect(_quit)

	# The command line's server is joined once (its recording watched once); leaving it lands in
	# the menu like any other.
	var direct: bool = not _started and (args.has("host") or args.has("port") or autoplay > 0.0)
	var watch: bool = not _started and args.has("replay")
	_started = true
	if watch:
		_watch(args["replay"])
	elif direct:
		_join(args.get("host", "127.0.0.1"), int(args.get("port", str(Menu.DEFAULT_PORT))))
	else:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
		menu.show_main(_menu_message)
	_menu_message = ""


func _parse_args() -> Dictionary:
	var result := {}
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--") and arg.contains("="):
			var eq := arg.find("=")
			result[arg.substr(2, eq - 2)] = arg.substr(eq + 1)
	return result


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		yaw -= event.relative.x * MOUSE_SENSITIVITY * menu.sensitivity
		pitch = clamp(pitch - event.relative.y * MOUSE_SENSITIVITY * menu.sensitivity, -1.3, 0.4)
	elif event is InputEventMouseButton and event.pressed:
		match event.button_index:
			MOUSE_BUTTON_WHEEL_UP:
				distance = max(distance - 0.5, 2.0)
			MOUSE_BUTTON_WHEEL_DOWN:
				distance = min(distance + 0.5, 20.0)
			MOUSE_BUTTON_LEFT:
				if _joined and not menu.is_open():
					Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	elif event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_ESCAPE:
				_toggle_pause()
			KEY_F1:
				show_debug = not show_debug
			_:
				if _replay != "" and _joined and not menu.is_open():
					_replay_key(event.keycode)


func _process(delta: float) -> void:
	_watch_connection()
	if _leaving:
		return
	_send_input(delta)
	_update_screen_effects(delta)
	_update_camera()
	_update_hud()
	_autoplay_finish()


# --- Joining and leaving --------------------------------------------------------------------------

func _join(host: String, port: int) -> void:
	_address = Menu.format_address(host, port)
	client.host = host
	client.port = port
	# The server learns names when a player joins.
	client.player_name = args["name"] if args.has("name") else menu.player_name()
	_refused = ""
	_joined = false
	_joining_since = Time.get_ticks_msec() / 1000.0
	if autoplay > 0.0:
		menu.close()
	else:
		menu.show_connecting(_address)
	client.connect_to_server()


## Plays a recording instead of joining a server.
func _watch(path: String) -> void:
	_replay = path
	_address = path.get_file()
	_refused = ""
	_joined = false
	_joining_since = Time.get_ticks_msec() / 1000.0
	menu.close()
	client.open_replay(path)


## The playback's keys. The source knows the commands (src/client/replay_source.h).
func _replay_key(key: Key) -> void:
	var stats: Dictionary = client.get_stats()
	match key:
		KEY_SPACE:
			client.control("pause", 0.0 if stats.get("replay_paused", false) else 1.0)
		KEY_RIGHT:
			client.control("skip", 5.0)
		KEY_LEFT:
			client.control("skip", -5.0)
		KEY_UP:
			client.control("speed", float(stats.get("replay_speed", 1.0)) * 2.0)
		KEY_DOWN:
			client.control("speed", float(stats.get("replay_speed", 1.0)) * 0.5)
		KEY_PERIOD:
			client.control("step", 1.0)
		KEY_COMMA:
			client.control("step", -1.0)
		KEY_HOME:
			client.control("seek", 0.0)
		KEY_N:
			client.control("follow_next", 1.0)


## Back to the menu, saying why. The scene is loaded again, so nothing of this server stays.
func _leave(message: String) -> void:
	if message != "":
		push_warning(message.replace("\n", " "))
	client.stop()
	if autoplay > 0.0:
		# Unattended runs have nobody to read a menu.
		print("autoplay refused: ", message.replace("\n", " "))
		autoplay = 0.0
		get_tree().quit(3)
		return
	_menu_message = message
	_leaving = true
	_joining_since = -1.0
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	get_tree().reload_current_scene()


func _quit() -> void:
	_leaving = true
	client.stop()
	get_tree().quit()


func _resume() -> void:
	menu.close()
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


func _toggle_pause() -> void:
	if not _joined:
		return
	if menu.is_open():
		_resume()
	else:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
		menu.show_pause("%s     %s" % [_address, client.get_map_name()])


## Follows an attempt until it is playing, or gives it up with the reason.
func _watch_connection() -> void:
	if _joining_since < 0.0:
		return
	var stats: Dictionary = client.get_stats()
	var state: String = stats.get("state", "")
	if _refused != "":
		_leave(_refused)
	elif state == "rejected" and _replay != "":
		_leave("The recording %s could not be played: %s." % [_address, stats.get("reject_reason", "no reason given")])
	elif state == "rejected":
		_leave("%s refused the connection: %s." % [_address, stats.get("reject_reason", "no reason given")])
	elif _joined:
		return
	elif state == "playing":
		_joined = true
		if autoplay <= 0.0:
			if _replay == "":
				menu.remember_server(client.host, client.port, client.get_map_name())
			menu.close()
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	elif autoplay > 0.0 or _replay != "":
		return # unattended runs wait for their server; a recording has no connection to fail
	elif int(stats.get("connect_failures", 0)) > 0:
		_leave("The address \"%s\" could not be found. Check the spelling." % client.host)
	elif Time.get_ticks_msec() / 1000.0 - _joining_since > JOIN_TIMEOUT:
		_leave("No answer from %s. Check the address and port, that the server is running, and that its port (UDP) is open." % _address)


func _on_schema_changed() -> void:
	if _load_items():
		_bind_actions()


## Loads the workshop items the server announced. False (and leaves) if any is missing or refused.
func _load_items() -> bool:
	var missing: Array[String] = []
	var paths: Array[String] = []
	for item in client.get_required_items():
		var path := Workshop.find_item(item["mod"], item["sha256"])
		if path == "":
			missing.append("%s (%s)" % [item["mod"], String(item["sha256"]).substr(0, 12)])
		else:
			paths.append(path)
	if not missing.is_empty():
		_refuse("This server needs workshop items you do not have:\n%s\nSubscribe to them and join again." % ", ".join(missing))
		return false
	# Packs cannot be unloaded: an item from an earlier server that this one does not use would
	# keep its HUD and reactions. A fresh process joins instead.
	var wanted := {}
	for path in paths:
		wanted[path.get_file().get_basename()] = true
	for sha in _loaded_items:
		if not wanted.has(sha):
			_restart_and_join()
			return false

	var added := false
	for path in paths:
		var sha := path.get_file().get_basename()
		if _loaded_items.has(sha):
			continue
		var problem: String = Boot.check_mod(path)
		if problem != "":
			_refuse("Workshop item %s was refused: %s" % [path.get_base_dir().get_file(), problem])
			return false
		if not ProjectSettings.load_resource_pack(path, true):
			_refuse("Workshop item %s could not be loaded" % path.get_base_dir().get_file())
			return false
		_loaded_items[sha] = true
		added = true
		print("workshop item loaded: %s/%s" % [path.get_base_dir().get_file(), path.get_file()])
	if added:
		# The player's own mods come last, so they can restyle what an item ships.
		for mod in Boot.player_mods:
			ProjectSettings.load_resource_pack(mod, true)
	# Also when the items were loaded by an earlier visit: this scene has not shown them yet.
	if added or (not _presented and not paths.is_empty()):
		_reload_presentation()
	_presented = true
	# The server's character (one of the items just loaded), or the built-in rig.
	var character_problem: String = client.use_character(client.get_character())
	if character_problem != "":
		_refuse("This server's character could not be used: %s" % character_problem)
		return false
	return true


## The connection watcher leaves with this reason.
func _refuse(reason: String) -> void:
	_refused = reason


## Starts the game again, straight into the server being joined (or the recording being watched).
func _restart_and_join() -> void:
	var restart := OS.get_cmdline_args()
	var cut := restart.find("--")
	if cut != -1:
		restart = restart.slice(0, cut)
	restart.append("--")
	for arg in OS.get_cmdline_user_args():
		if not (arg.begins_with("--host=") or arg.begins_with("--port=") or arg.begins_with("--replay=")):
			restart.append(arg)
	if _replay != "":
		restart.append("--replay=%s" % _replay)
	else:
		restart.append("--host=%s" % client.host)
		restart.append("--port=%d" % client.port)
	print("restarting to join %s without the previous server's items" % _address)
	client.stop()
	_joining_since = -1.0
	_leaving = true
	OS.set_restart_on_exit(true, restart)
	get_tree().quit()


## Reactions and item HUDs, loaded again now that items may have added or replaced some.
func _reload_presentation() -> void:
	client.clear_world_scenes()
	_load_reactions()
	for node in _item_huds:
		node.queue_free()
	_item_huds.clear()
	var names := []
	for file in DirAccess.get_files_at("res://ui"):
		var clean: String = file.trim_suffix(".remap")
		if clean.begins_with("hud_") and clean.ends_with(".tscn"):
			names.append(clean)
	names.sort()
	for file in names:
		var scene := ResourceLoader.load("res://ui/%s" % file, "PackedScene", ResourceLoader.CACHE_MODE_REPLACE) as PackedScene
		if scene:
			var node := scene.instantiate()
			add_child(node)
			_item_huds.append(node)
	print("item HUDs: ", names)


# --- Input ----------------------------------------------------------------------------------------
#
# The server's mods declare their actions and suggest a key for each. They become InputMap actions
# named cb_<action>, so the usual Godot input remapping works on them too.

func _bind_actions() -> void:
	for action in InputMap.get_actions():
		if String(action).begins_with(ACTION_PREFIX):
			InputMap.erase_action(action)
	_actions = client.get_actions()
	for action in _actions:
		var name: String = ACTION_PREFIX + String(action["name"])
		InputMap.add_action(name)
		var event := _event_for_key(String(action["key"]))
		if event != null:
			InputMap.action_add_event(name, event)
	print("mod actions: ", ", ".join(_actions.map(func(a): return "%s (%s)" % [a["name"], a["key"]])))
	_update_help()


func _event_for_key(key: String) -> InputEvent:
	match key:
		"MouseLeft", "MouseRight", "MouseMiddle":
			var mouse := InputEventMouseButton.new()
			mouse.button_index = {"MouseLeft": MOUSE_BUTTON_LEFT, "MouseRight": MOUSE_BUTTON_RIGHT,
				"MouseMiddle": MOUSE_BUTTON_MIDDLE}[key]
			return mouse
	var code := OS.find_keycode_from_string(key)
	if code == KEY_NONE:
		push_warning("mod action key \"%s\" is not a key Godot knows" % key)
		return null
	var ev := InputEventKey.new()
	ev.physical_keycode = code
	return ev


func _action_bits() -> int:
	var bits := 0
	for action in _actions:
		if Input.is_action_pressed(ACTION_PREFIX + String(action["name"])):
			bits |= 1 << int(action["bit"])
	return bits


func _action_bit(name: String) -> int:
	for action in _actions:
		if action["name"] == name:
			return 1 << int(action["bit"])
	return 0


func _send_input(delta: float) -> void:
	var move := Vector2.ZERO
	var jump := false
	var sprint := false
	var actions := 0
	if autoplay > 0.0:
		# Scripted player for unattended runs: props first, then the pistol, then the bat (if the
		# server runs the melee mod).
		if auto_rng.randf() < delta * 1.5:
			auto_move = Vector2(auto_rng.randf_range(-1, 1), auto_rng.randf_range(0.2, 1))
		move = auto_move
		sprint = true
		jump = auto_rng.randf() < delta * 1.0
		yaw += delta * 0.6
		var elapsed := Time.get_ticks_msec() / 1000.0 - playing_since if playing_since >= 0.0 else 0.0
		if elapsed < autoplay * 0.4:
			if auto_rng.randf() < delta * 2.0:
				actions |= _action_bit("spawn_prop")
		elif elapsed < autoplay * 0.7 or _action_bit("slot_3") == 0:
			actions |= _action_bit("slot_2")
			if auto_rng.randf() < delta * 3.0:
				actions |= _action_bit("fire")
		else:
			actions |= _action_bit("slot_3")
			if auto_rng.randf() < delta * 2.0:
				actions |= _action_bit("fire")
		# Hold Tab at the end, so screenshots show the scoreboard too.
		if elapsed > autoplay * 0.8 and not Input.is_action_pressed("scoreboard"):
			Input.action_press("scoreboard")
	elif get_window().has_focus() and not menu.is_open():
		move.x = float(Input.is_physical_key_pressed(KEY_D)) - float(Input.is_physical_key_pressed(KEY_A))
		move.y = float(Input.is_physical_key_pressed(KEY_W)) - float(Input.is_physical_key_pressed(KEY_S))
		jump = Input.is_physical_key_pressed(KEY_SPACE)
		sprint = Input.is_physical_key_pressed(KEY_SHIFT)
		# Actions only count while the game has the mouse, so the click that captures it is not a shot.
		if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
			actions = _action_bits()
	client.set_input(move, yaw, pitch, jump, sprint, actions)


func _update_camera() -> void:
	# The player's head, or its ragdoll while dead. The server casts the crosshair ray through the
	# same point, so what is under the crosshair is what gets hit.
	var target: Vector3 = client.get_camera_target()
	camera.rotation = Vector3(pitch, yaw, 0)
	# The map pulls the camera in at once, and it eases back out when the way is clear.
	var back: Vector3 = camera.global_transform.basis.z
	var free: float = client.get_camera_distance(target, back, distance, CAMERA_RADIUS)
	_camera_distance = free if free < _camera_distance else minf(free, _camera_distance + 12.0 * get_process_delta_time())
	camera.global_position = target + back * _camera_distance
	if _shake > 0.0:
		camera.global_position += Vector3(
			auto_rng.randf_range(-_shake, _shake),
			auto_rng.randf_range(-_shake, _shake),
			auto_rng.randf_range(-_shake, _shake))


func _update_help() -> void:
	if hud == null:
		return
	var help := hud.get_node_or_null("%Help") as Label
	if help == null:
		return
	if _replay != "":
		help.text = "Space pause   Left/Right -/+5 s   Up/Down speed   , . step   Home restart   N next player   Tab scores   Mouse orbit   Wheel zoom   Esc menu   F1 stats"
		return
	var text := "WASD move   Shift sprint   Space jump"
	for action in _actions:
		var key: String = String(action["key"]).replace("Mouse", "Mouse ")
		text += "   %s %s" % [key, String(action["name"]).replace("_", " ")]
	help.text = text + "   Tab scores   Mouse orbit   Wheel zoom   Esc menu   F1 stats"


func _update_hud() -> void:
	if hud == null:
		return
	var stats: Dictionary = client.get_stats()
	var label := hud.get_node_or_null("%Stats") as Label
	if label:
		label.visible = show_debug and (_joined or autoplay > 0.0)
		label.text = "%d FPS   %s\ntick %d   confirmed %d   window %d\nrtt %d ms   rollbacks %d (last %d)   stalled %.1f s\nchecksums ok %d   desyncs %d\nentities %d   animation: %s\nmods: %s" % [
			Engine.get_frames_per_second(), stats.get("state", ""),
			stats.get("tick", 0), stats.get("confirmed_tick", 0), stats.get("rollback_window", 0),
			stats.get("rtt_ms", 0), stats.get("rollbacks", 0), stats.get("last_rollback_depth", 0), stats.get("stalled_seconds", 0.0),
			stats.get("checksums_verified", 0), stats.get("desyncs", 0),
			stats.get("entities", 0), stats.get("animation", ""), ", ".join(client.get_mod_names())]
	var help := hud.get_node_or_null("%Help") as Label
	if help:
		help.visible = _joined
	# Before the first join the menu says what is going on; after it, only a lost connection is news.
	var banner := hud.get_node_or_null("%Banner") as Label
	if banner:
		var state: String = stats.get("state", "")
		banner.visible = state != "playing" and (_joined or autoplay > 0.0)
		if _replay != "" and state == "playing":
			# A recording says where it is whenever it is not simply playing on.
			var follow: int = stats.get("replay_follow", -1)
			var note := "end of the recording" if stats.get("replay_ended", false) else "paused" if stats.get("replay_paused", false) else ""
			var speed: float = stats.get("replay_speed", 1.0)
			if note == "" and not is_equal_approx(speed, 1.0):
				note = "x%s" % String.num(speed, 3)
			banner.visible = note != ""
			banner.text = "REPLAY   %.1f / %.1f s   %s\n%s" % [stats.get("replay_seconds", 0.0), stats.get("replay_length_seconds", 0.0), note,
				"following %s" % client.get_player_name(client.get_local_net_id()) if follow >= 0 else "nobody to follow"]
		elif _joined:
			banner.text = "Connection lost - time is paused, reconnecting...\nEsc: menu"
		else:
			banner.text = "Connecting..."


func _autoplay_finish() -> void:
	if autoplay <= 0.0:
		return
	if playing_since < 0.0:
		if client.get_source_state() == "playing":
			playing_since = Time.get_ticks_msec() / 1000.0
		return
	var elapsed := Time.get_ticks_msec() / 1000.0 - playing_since
	if screenshot != "" and screenshot_every > 0.0 and elapsed >= _next_screenshot:
		_next_screenshot = elapsed + screenshot_every
		_screenshots += 1
		var path := "%s_%d.png" % [screenshot.trim_suffix(".png"), _screenshots]
		get_viewport().get_texture().get_image().save_png(path)
	if elapsed < autoplay:
		return
	autoplay = 0.0
	if screenshot != "":
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(screenshot)
	var stats: Dictionary = client.get_stats()
	print("autoplay done: %s, checksums ok %d, desyncs %d, fingerprint %s, fp ok %s" % [
		stats.get("state"), stats.get("checksums_verified"), stats.get("desyncs"), stats.get("fingerprint"), stats.get("fp_environment_ok")])
	print("mod events seen: ", _event_counts)
	client.stop()
	get_tree().quit(0 if stats.get("desyncs", 1) == 0 and stats.get("state") == "playing" else 2)


func _make_flash_overlay() -> void:
	# Its own layer, so a mod replacing the HUD cannot remove it by accident.
	var layer := CanvasLayer.new()
	layer.layer = 100
	add_child(layer)
	_flash_rect = ColorRect.new()
	_flash_rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	_flash_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_flash_rect.color = Color(1, 1, 1, 0)
	_flash_rect.visible = false
	layer.add_child(_flash_rect)


# --- World reactions -----------------------------------------------------------------------------
#
# What plays when is data: every res://vfx/reactions*.tscn is loaded once and handed to the client,
# which runs its CbReaction nodes on every event (spawns, jumps, impacts, mod events, the local
# player's presses). A mod adds reactions by adding a file of its own instead of replacing the
# game's. Screen effects come back here, since the camera and the overlay are the game's.

func _load_reactions() -> void:
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
		client.add_world_scene(scene.instantiate())
	print("world reactions: ", names)


func _on_mod_event(name: String, _a: int, _b: int, _value: int, _position: Vector3, _vector: Vector3) -> void:
	_event_counts[name] = _event_counts.get(name, 0) + 1


func _on_screen_effect(shake: float, shake_time: float, flash_color: Color, flash_time: float) -> void:
	if shake > 0.0:
		_shake = max(_shake, shake)
		_shake_decay = shake / max(shake_time, 0.05)
	if flash_color.a > 0.0:
		_flash_color = flash_color
		_flash = flash_color.a
		_flash_decay = flash_color.a / max(flash_time, 0.02)


func _update_screen_effects(delta: float) -> void:
	_shake = max(0.0, _shake - _shake_decay * delta)
	if _flash > 0.0:
		_flash = max(0.0, _flash - _flash_decay * delta)
		_flash_rect.color = Color(_flash_color.r, _flash_color.g, _flash_color.b, _flash)
		_flash_rect.visible = _flash > 0.0


func _on_visual_spawned(_visual_id: int, net_id: int, kind: String, node: Node3D, _position: Vector3,
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
