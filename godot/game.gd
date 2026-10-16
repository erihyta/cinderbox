extends Node3D
## Base game presentation: joining and leaving, watching recordings, input and the HUD. Its parts
## each have a script of their own:
##   autoplay.gd      the unattended player (--autoplay, --view-probe), screenshots
##   presentation.gd  world reactions, items and their HUDs, the mods' HUDs, screen effects
##   view_camera.gd   the camera (behind, over a shoulder, first person) and the view model
##   keys.gd          the game's keys, the server's actions, the slots' keys, the help line
##
## Everything visual it uses is loaded by path, so mods can replace it:
##   res://ui/hud.tscn          HUD layout. Optional unique nodes: %Stats, %Banner, %Help.
##   res://ui/menu.tscn         the menus (join, in-game, settings), driven by menu.gd.
##   res://ui/hud_*.tscn        HUDs that come with workshop items, laid over the game's.
##   res://vfx/reactions*.tscn  world reactions (CbReaction nodes: scenes, sounds and screen effects
##                              on events), run by the client. Items are res://items/*.cfg, baked from CbItem scenes.
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
## Watching: --replay=FILE plays a recording (cb_server --record) instead of joining, and
## --view=FILE a view file (cb_server --record-view: the frames as they were, no simulation). The
## client draws either like a server: the file names the same workshop items, and the player it
## follows is the local one, with its HUD. Nothing is sent anywhere; the keys steer the playback.
##
## Extensions: this script's `client` (CinderboxClient, the viewer) draws frames and holds no
## simulation. `peer` (CinderboxPeer, the peer extension) joins servers by simulating them
## (prediction, rollback) and re-simulates recordings. Without it, the game still watches
## view files.
##
## Command line (after `--`): --host=H --port=P --name=NAME --rollback=N
##                            --replay=FILE --view=FILE
##                            --host-local[=MODS] --bots=N   start a server here, join it, stop it on leaving (host.gd)
##                            --autoplay=SECONDS --screenshot=FILE --screenshot-every=SECONDS
##                            --mods=DIR --workshop=DIR --config=FILE
## With --screenshot-every, autoplay also saves FILE_1.png, FILE_2.png, ... along the way.

const MOUSE_SENSITIVITY := 0.003
## How far up and down the camera looks, in every view: nearly straight up and straight down (86
## degrees; the simulation takes up to 88). Behind the player the map pulls the camera in where it
## would go under the floor.
const MAX_PITCH := 1.5
const Boot := preload("res://boot.gd")
const Workshop := preload("res://workshop.gd")
const Menu := preload("res://menu.gd")
const Host := preload("res://host.gd")
const Autoplay := preload("res://autoplay.gd")
const Presentation := preload("res://presentation.gd")
const ViewCamera := preload("res://view_camera.gd")
const Keys := preload("res://keys.gd")
const NO_PEER := "This copy of the game has no peer extension, so it cannot %s."
const NO_LINK := "This copy of the game does not have the peer extension, so it cannot join servers."
## How long a server may take to answer before the attempt is given up.
const JOIN_TIMEOUT := 10.0

## These outlive the scene, which is reloaded when a server is left.
static var _started := false # the command line's --host has been used
static var _menu_message := "" # why the last server was left, for the menu
static var _loaded_items := {} # sha256 -> the mod it is of: workshop items loaded into this process

@onready var client: CinderboxClient = $Client
## The peer extension's CinderboxPeer, or null when this copy of the game does not have it.
var peer: RefCounted
@onready var camera: Camera3D = $Camera

var yaw := PI # facing +Z like the server's spawn orientation
var pitch := -0.35
var distance := 6.0
## Looking out of the character's head instead of from behind it (the key left of 1 toggles it).
var first_person := false
## Third person: the camera over a shoulder (1 right, -1 left) or straight behind (0). Z cycles.
var shoulder := 0
var args := {}
var hud: Node
var show_debug := true

## The parts of the game, each in a script of its own.
var auto: Autoplay # the unattended player
var presentation: Presentation # reactions, items and their HUDs, screen effects
var view_camera: ViewCamera # the camera and the view model
var keys: Keys # the game's keys, the server's actions, the slots' keys, the help line

## A screen of a mod's (an inventory) has the mouse: the cursor is free and the game does not use it.
var _ui_cursor := false
var _presented := false # this scene has loaded the items' reactions and HUDs
var _refused := "" # why this server cannot be joined

var menu: Menu
var _address := "" # the server being joined or played on, as typed
var _joining_since := -1.0 # seconds; -1: not trying
var _joined := false # this attempt reached "playing"
var _replay := "" # the recording or view file being watched ("": playing on a server)
var _replay_is_view := false
var _leaving := false # the scene is being replaced


func _ready() -> void:
	args = _parse_args()
	# The camera is placed after everything has moved this frame (the viewer is a child and updates
	# the drawn world after this node): a frame late, your own body would run ahead of your eyes.
	auto = Autoplay.new(self, args)
	presentation = Presentation.new(self)
	keys = Keys.new(self)
	view_camera = ViewCamera.new(self, camera)
	RenderingServer.frame_pre_draw.connect(view_camera.update)
	view_camera.make_view_model()
	if ClassDB.class_exists("CinderboxPeer"):
		peer = ClassDB.instantiate("CinderboxPeer")
	if args.has("rollback") and peer != null:
		peer.rollback_min = int(args["rollback"])
		peer.rollback_max = int(args["rollback"])
	# --reconcile-every=N: what the server confirms is worked in every N ticks, not every tick: less
	# work with many players and latency, and what others do is seen up to N ticks later.
	if args.has("reconcile-every") and peer != null:
		peer.reconcile_every = int(args["reconcile-every"])
	# --physics-threads=N: the physics runs on N more threads (what is simulated stays the same).
	if args.has("physics-threads") and peer != null:
		peer.physics_threads = int(args["physics-threads"])

	client.visual_spawned.connect(presentation.on_visual_spawned)
	client.mod_event.connect(auto.on_mod_event)
	client.get_director().screen_effect.connect(presentation.on_screen_effect)
	client.schema_changed.connect(_on_schema_changed)
	client.source_state_changed.connect(func(state): print("source: ", state))

	presentation.load_reactions()
	presentation.make_flash_overlay()

	# Everything a mod can replace goes through the guard (CbDirector.instantiate): a scene with
	# anything but listed node classes and data is refused, and null comes back.
	hud = CbDirector.instantiate(load("res://ui/hud.tscn") as PackedScene)
	if hud:
		add_child(hud)

	menu = Menu.new()
	menu.name = "MenuDriver"
	add_child(menu)
	menu.join_requested.connect(_join)
	menu.host_requested.connect(_host)
	menu.cancel_requested.connect(_leave.bind(""))
	menu.leave_requested.connect(_leave.bind(""))
	menu.resume_requested.connect(_resume)
	menu.quit_requested.connect(_quit)
	keys.bind_game_keys()

	# The command line's server is joined once (its recording watched once); leaving it lands in
	# the menu like any other.
	var local: bool = not _started and args.has("host-local")
	var direct: bool = not _started and not local and (args.has("host") or args.has("port") or auto.seconds > 0.0)
	var watch: bool = not _started and (args.has("replay") or args.has("view"))
	_started = true
	if watch:
		_watch(args.get("view", args.get("replay", "")), args.has("view"))
	elif local:
		_host()
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
		elif arg.begins_with("--"):
			result[arg.substr(2)] = "" # a flag
	return result


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		yaw -= event.relative.x * MOUSE_SENSITIVITY * menu.sensitivity
		pitch = clamp(pitch - event.relative.y * MOUSE_SENSITIVITY * menu.sensitivity, -MAX_PITCH, MAX_PITCH)
	elif event is InputEventMouseButton and event.pressed:
		match event.button_index:
			MOUSE_BUTTON_WHEEL_UP:
				distance = max(distance - 0.5, 2.0)
			MOUSE_BUTTON_WHEEL_DOWN:
				distance = min(distance + 0.5, 20.0)
			MOUSE_BUTTON_LEFT:
				if _joined and not menu.is_open() and not client.wants_cursor():
					Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	elif event.is_action_pressed("game_first_person"):
		# (By default the key left of 1, whatever the layout prints on it.)
		first_person = not first_person
	elif event.is_action_pressed("game_shoulder") and _replay == "":
		shoulder = 1 if shoulder == 0 else (-1 if shoulder > 0 else 0)
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
	# The viewer draws the player's own body for the view it is seen from.
	client.first_person = first_person and client.get_local_net_id() != 0 and not client.is_local_player_dead()
	presentation.update_screen_effects(delta)
	_update_hud()
	presentation.update_kind_huds()
	auto.finish()


# --- Joining and leaving --------------------------------------------------------------------------

## A server of the player's own: started, joined, and stopped when it is left (host.gd).
func _host() -> void:
	var started: Dictionary = Host.start(args)
	if started.has("error"):
		if auto.seconds > 0.0:
			print("autoplay refused: ", started["error"])
			get_tree().quit(3)
			return
		menu.show_main(started["error"])
		return
	_join("127.0.0.1", int(started["port"]))


func _join(host: String, port: int) -> void:
	_address = Menu.format_address(host, port)
	if peer == null:
		_leave(NO_LINK)
		return
	peer.host = host
	peer.port = port
	# The server learns names when a player joins.
	peer.player_name = args["name"] if args.has("name") else menu.player_name()
	_refused = ""
	_joined = false
	_joining_since = Time.get_ticks_msec() / 1000.0
	if auto.seconds > 0.0:
		menu.close()
	else:
		menu.show_connecting(_address)
	peer.connect_to_server()
	client.set_source(peer)


## Plays a recording (or a view file) instead of joining a server.
func _watch(path: String, is_view: bool) -> void:
	if not is_view and peer == null:
		_leave(NO_PEER % "play recordings (it can watch view files)")
		return
	_replay = path
	_replay_is_view = is_view
	_address = path.get_file()
	_refused = ""
	_joined = false
	_joining_since = Time.get_ticks_msec() / 1000.0
	menu.close()
	if is_view:
		client.open_view(path)
	else:
		peer.open_replay(ProjectSettings.globalize_path(path))
		client.set_source(peer)


## Nothing is drawn or played any more: the viewer drops its source, the sources end their threads.
func _stop() -> void:
	client.stop()
	if peer != null:
		peer.stop()


## The playback's keys. The source knows the commands (src/client/replay_source.h and
## src/present/view_file.h take the same ones).
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
	_stop()
	Host.stop()
	if auto.seconds > 0.0:
		# Unattended runs have nobody to read a menu.
		print("autoplay refused: ", message.replace("\n", " "))
		auto.seconds = 0.0
		get_tree().quit(3)
		return
	_menu_message = message
	_leaving = true
	_joining_since = -1.0
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	get_tree().reload_current_scene()


## The window's close button: a server this game started goes with it.
func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		Host.stop()


func _quit() -> void:
	_leaving = true
	_stop()
	Host.stop()
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
		if auto.seconds <= 0.0:
			if _replay == "":
				menu.remember_server(peer.host, peer.port, client.get_map_name())
			menu.close()
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	elif auto.seconds > 0.0 or _replay != "":
		return # unattended runs wait for their server; a recording has no connection to fail
	elif int(stats.get("connect_failures", 0)) > 0:
		_leave("The address \"%s\" could not be found. Check the spelling." % peer.host)
	elif Time.get_ticks_msec() / 1000.0 - _joining_since > JOIN_TIMEOUT:
		_leave("No answer from %s. Check the address and port, that the server is running, and that its port (UDP) is open." % _address)


func _on_schema_changed() -> void:
	if _load_items():
		keys.bind_actions()


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
	# (A mod's look published again is not that: the new item is loaded over the old, below.)
	var wanted := {}
	var wanted_mods := {}
	for path in paths:
		wanted[path.get_file().get_basename()] = true
		wanted_mods[path.get_base_dir().get_file()] = true
	for sha in _loaded_items:
		if not wanted.has(sha) and not wanted_mods.has(_loaded_items[sha]):
			_restart_and_join()
			return false

	var added := false
	var replaced := false
	for path in paths:
		var sha := path.get_file().get_basename()
		var mod := path.get_base_dir().get_file()
		if _loaded_items.has(sha):
			continue
		var problem: String = Boot.check_mod(path)
		if problem != "":
			_refuse("Workshop item %s was refused: %s" % [path.get_base_dir().get_file(), problem])
			return false
		if not ProjectSettings.load_resource_pack(path, true):
			_refuse("Workshop item %s could not be loaded" % path.get_base_dir().get_file())
			return false
		# The same mod's item from before: this one took its files' places. What was already
		# loaded from them is loaded again.
		for old in _loaded_items.keys():
			if _loaded_items[old] == mod:
				_loaded_items.erase(old)
				replaced = true
				_refresh_loaded(path)
		_loaded_items[sha] = mod
		added = true
		print("workshop item loaded: %s/%s" % [mod, path.get_file()])
	if added:
		# The player's own mods come last, so they can restyle what an item ships.
		for mod in Boot.player_mods:
			ProjectSettings.load_resource_pack(mod, true)
	# Also when the items were loaded by an earlier visit: this scene has not shown them yet.
	if added or (not _presented and not paths.is_empty()):
		presentation.reload()
	if replaced:
		# What is in the world is drawn from the scenes as they are now.
		client.rebuild_nodes()
		print("a look was published again: shown as it is now")
	_presented = true
	# The server's character (one of the items just loaded), or the built-in rig.
	var character_problem: String = client.use_character(client.get_character())
	if character_problem != "":
		_refuse("This server's character could not be used: %s" % character_problem)
		return false
	return true


## A pack was loaded over an older one of the same mod: Godot keeps what it loaded from the old
## files, so every resource of the pack that is in its cache is loaded again in place.
func _refresh_loaded(pack: String) -> void:
	var zip := ZIPReader.new()
	if zip.open(pack) != OK:
		return
	for file in zip.get_files():
		var path := "res://" + String(file).trim_suffix(".remap").trim_suffix(".import")
		if ResourceLoader.has_cached(path):
			ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_REPLACE_DEEP)
	zip.close()


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
		if not (arg.begins_with("--host=") or arg.begins_with("--port=") or arg.begins_with("--replay=") or arg.begins_with("--view=")):
			restart.append(arg)
	if _replay != "":
		restart.append("--%s=%s" % ["view" if _replay_is_view else "replay", _replay])
	else:
		restart.append("--host=%s" % peer.host)
		restart.append("--port=%d" % peer.port)
	print("restarting to join %s without the previous server's items" % _address)
	_stop()
	_joining_since = -1.0
	_leaving = true
	OS.set_restart_on_exit(true, restart)
	get_tree().quit()


# --- Input and the HUD ----------------------------------------------------------------------------
#
# The keys are keys.gd's; the unattended player's input is autoplay.gd's.

func _send_input(delta: float) -> void:
	var move := Vector2.ZERO
	var jump := false
	var sprint := false
	# A mod's screen that needs the mouse (a CbKey with cursor) takes it, and gives it back.
	var wants: bool = client.wants_cursor()
	if wants != _ui_cursor:
		_ui_cursor = wants
		if _joined and not menu.is_open():
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if wants else Input.MOUSE_MODE_CAPTURED
	var actions := 0
	var use := false
	var slot_keys := 0
	keys.bind_slot_keys()
	if auto.seconds > 0.0:
		var steered: Dictionary = auto.steer(delta)
		move = steered["move"]
		jump = steered["jump"]
		sprint = steered["sprint"]
		use = steered["use"]
		actions = steered["actions"]
	elif get_window().has_focus() and not menu.is_open() and not client.is_typing():
		move.x = float(Input.is_action_pressed("game_right")) - float(Input.is_action_pressed("game_left"))
		move.y = float(Input.is_action_pressed("game_forward")) - float(Input.is_action_pressed("game_back"))
		jump = Input.is_action_pressed("game_jump")
		sprint = Input.is_action_pressed("game_sprint")
		# Actions only count while the game has the mouse, so the click that captures it is not a shot.
		if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
			actions = keys.action_bits()
			# The use button is the engine's: what is in the hand is used (what that does is its mod's).
			use = Input.is_action_pressed("game_use")
		# The slots' keys select their slots. They go out held, like the buttons; the simulation
		# sees the presses. Also while a screen has the mouse (the inventory), not in a recording.
		if _replay == "" and (Input.mouse_mode == Input.MOUSE_MODE_CAPTURED or _ui_cursor):
			slot_keys = keys.slot_key_bits()
	client.set_input(move, yaw, pitch, jump, sprint, use, actions, view_camera.view(), slot_keys)


func _update_hud() -> void:
	if hud == null:
		return
	var stats: Dictionary = client.get_stats()
	var label := hud.get_node_or_null("%Stats") as Label
	if label:
		label.visible = show_debug and (_joined or auto.seconds > 0.0)
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
		banner.visible = state != "playing" and (_joined or auto.seconds > 0.0)
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
