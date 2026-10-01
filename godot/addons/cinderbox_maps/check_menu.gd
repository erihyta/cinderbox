extends SceneTree
## Drives the game's menus the way a player would, against a running server:
##
##   cb_server --port 7790
##   godot --path godot --script res://addons/cinderbox_maps/check_menu.gd -- --test-port=7790 --config=<abs file> [--shots=<abs folder>]
##
## It types a bad address, a host that does not exist and a port nobody listens on (each must come
## back to the menu and say why), then joins the server, checks the camera stays above the floor,
## opens the Esc menu and the settings, leaves, and joins again from the recent list. --config keeps
## the test's name, settings and recent servers out of the player's own file. Exit code 0 when
## everything held.
##
## With a second server that runs other mods (cb_server --port 7792 --mods inventory,props) and
## --other-port=7792 --result=<abs file>, it then joins that one from the menu. The first server's
## workshop items cannot be unloaded, so the game must start again and join by itself; the restarted
## run finds itself here again (by its --host), checks that nothing of the first server is loaded,
## and writes "ok" or what failed into the result file.

var _failures := 0
var _port := 0
var _shots := ""
var _other_port := 0
var _result := ""
var _restarted := false


func _check(what: String, ok: bool) -> void:
	print("  %-66s %s" % [what, "ok" if ok else "FAIL"])
	if not ok:
		_failures += 1


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--test-port="):
			_port = int(arg.substr(12))
		elif arg.begins_with("--shots="):
			_shots = arg.substr(8)
		elif arg.begins_with("--other-port="):
			_other_port = int(arg.substr(13))
		elif arg.begins_with("--result="):
			_result = arg.substr(9)
		elif arg.begins_with("--host="):
			_restarted = true
	if _restarted:
		_after_restart.call_deferred()
		return
	if _port == 0:
		printerr("pass -- --test-port=<port of a running cb_server>")
		quit(2)
		return
	_run.call_deferred()


## Headless runs have no mouse to capture.
func _mouse_is(mode: Input.MouseMode) -> bool:
	return DisplayServer.get_name() == "headless" or Input.mouse_mode == mode


func _game() -> Node:
	return current_scene


func _menu() -> Node:
	return _game().menu


func _n(unique: String) -> Node:
	return _menu()._scene.get_node("%" + unique)


func _shown(panel: String) -> bool:
	return _menu().is_open() and (_n(panel) as CanvasItem).visible


func _wait(seconds: float) -> void:
	await create_timer(seconds).timeout


## Waits until `test` holds (the scene may be reloaded meanwhile). False after `limit` seconds.
func _until(test: Callable, limit: float) -> bool:
	var start := Time.get_ticks_msec()
	while Time.get_ticks_msec() - start < limit * 1000.0:
		await process_frame
		if current_scene != null and current_scene.get("menu") != null and test.call():
			return true
	return false


func _shot(name: String) -> void:
	if _shots == "":
		return
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(_shots.path_join(name + ".png"))


func _type_and_join(address: String) -> void:
	(_n("Address") as LineEdit).text = address
	(_n("Join") as Button).pressed.emit()


func _message() -> String:
	var label := _n("Message") as Label
	return label.text if label.visible else ""


func _run() -> void:
	change_scene_to_file("res://game.tscn")
	_check("the game starts in the menu", await _until(func(): return _shown("Main"), 5.0))
	_check("  nothing is connecting yet", not _game().client.is_running())
	_check("  no message", _message() == "")
	(_n("Name") as LineEdit).text = "Menu Tester"
	await _shot("menu_1_start")

	print("addresses")
	var Menu = load("res://menu.gd")
	_check("  host alone gets the default port", Menu.parse_address(" example.org ") == {"host": "example.org", "port": 7777})
	_check("  host:port", Menu.parse_address("10.0.0.2:9000") == {"host": "10.0.0.2", "port": 9000})
	_check("  empty, spaces, bad port, port out of range are errors",
		Menu.parse_address("").has("error") and Menu.parse_address("my server").has("error")
		and Menu.parse_address("host:abc").has("error") and Menu.parse_address("host:70000").has("error")
		and Menu.parse_address(":7777").has("error"))
	_check("  the default port is left out when shown", Menu.format_address("h", 7777) == "h" and Menu.format_address("h", 9) == "h:9")

	print("an address that is not one")
	_type_and_join("not an address")
	await process_frame
	_check("  stays in the menu and says so", _shown("Main") and _message().contains("not an address"))
	_check("  nothing started", not _game().client.is_running())

	print("a host that does not exist")
	_type_and_join("no-such-host.invalid")
	await process_frame
	_check("  shows the connecting panel", _shown("Connecting"))
	_check("  comes back to the menu", await _until(func(): return _shown("Main") and _message() != "", 15.0))
	print("    \"%s\"" % _message())
	_check("  says the address could not be found", _message().contains("could not be found"))
	_check("  the name typed before is kept", (_n("Name") as LineEdit).text == "Menu Tester")
	await _shot("menu_2_unknown_host")

	print("a port nobody listens on")
	_type_and_join("127.0.0.1:%d" % (_port + 1))
	_check("  comes back to the menu", await _until(func(): return _shown("Main") and _message() != "", 20.0))
	print("    \"%s\"" % _message())
	_check("  says nobody answered", _message().contains("No answer"))
	_check("  no recent server was recorded", _menu().recent_servers().is_empty())

	print("cancel")
	_type_and_join("127.0.0.1:%d" % (_port + 1))
	await _wait(0.5)
	_check("  connecting", _shown("Connecting") and _game().client.is_running())
	(_n("Cancel") as Button).pressed.emit()
	_check("  back in the menu without a message", await _until(func(): return _shown("Main"), 5.0) and _message() == "")

	print("the server")
	_type_and_join("127.0.0.1:%d" % _port)
	_check("  joins", await _until(func(): return _game()._joined, 20.0))
	_check("  the menu is closed and the mouse is the game's", not _menu().is_open() and _mouse_is(Input.MOUSE_MODE_CAPTURED))
	_check("  the name reached the server", await _until(func(): return _game().client.get_player_name(_game().client.get_local_net_id()) == "Menu Tester", 5.0))
	var recent: Array = _menu().recent_servers()
	_check("  it is the recent server, with its map", recent.size() == 1 and recent[0]["address"] == "127.0.0.1:%d" % _port and recent[0]["map"] != "")
	await _wait(1.5)
	var huds: int = _game()._item_huds.size()
	await _shot("menu_3_playing")

	print("camera")
	var game := _game()
	game.pitch = 0.4 # looking up: the camera would be under the floor
	game.distance = 8.0
	await _wait(0.5)
	var cam: Camera3D = game.camera
	print("    looking up: camera height %.2f m, distance %.2f of %.1f" % [cam.global_position.y, game._camera_distance, game.distance])
	_check("  the floor keeps the camera above it", cam.global_position.y > 0.15 and game._camera_distance < game.distance - 1.0)
	await _shot("menu_4_camera_floor")
	game.pitch = -0.35
	await _wait(1.0)
	_check("  with the way clear it is back at full distance", absf(game._camera_distance - game.distance) < 0.01)
	game.distance = 6.0

	print("the Esc menu")
	game._toggle_pause()
	await process_frame
	_check("  opens, with the mouse free", _shown("Pause") and Input.mouse_mode == Input.MOUSE_MODE_VISIBLE and _menu().is_paused())
	_check("  says where you are", (_n("PauseText") as Label).text.contains(str(_port)))
	_check("  the game goes on behind it", game.client.get_source_state() == "playing")
	await _shot("menu_5_pause")
	(_n("PauseSettings") as Button).pressed.emit()
	await process_frame
	_check("  settings open", _shown("Settings"))
	(_n("Sensitivity") as Range).value = 2.0
	(_n("Volume") as Range).value = 40.0
	await process_frame
	_check("  sensitivity applies", is_equal_approx(_menu().sensitivity, 2.0))
	_check("  volume applies", absf(AudioServer.get_bus_volume_db(0) - linear_to_db(0.4)) < 0.01)
	await _shot("menu_6_settings")
	(_n("SettingsBack") as Button).pressed.emit()
	await process_frame
	_check("  back returns to the Esc menu", _shown("Pause"))
	(_n("Resume") as Button).pressed.emit()
	await process_frame
	_check("  resume closes it", not _menu().is_open() and _mouse_is(Input.MOUSE_MODE_CAPTURED))

	print("leaving")
	game._toggle_pause()
	(_n("Leave") as Button).pressed.emit()
	_check("  back in the menu", await _until(func(): return _shown("Main") and not _game()._joined, 5.0))
	_check("  disconnected, no message", not _game().client.is_running() and _message() == "")
	_check("  settings were kept", is_equal_approx(_menu().sensitivity, 2.0) and (_n("Volume") as Range).value == 40.0)
	var buttons := _n("Recent").get_children()
	_check("  the server is in the recent list", buttons.size() == 1 and buttons[0] is Button and (buttons[0] as Button).text.contains(str(_port)))
	_check("  and in the address box", (_n("Address") as LineEdit).text == "127.0.0.1:%d" % _port)
	await _shot("menu_7_recent")

	print("joining again from the recent list")
	(buttons[0] as Button).pressed.emit()
	_check("  joins", await _until(func(): return _game()._joined, 20.0))
	await _wait(1.0)
	var stats: Dictionary = _game().client.get_stats()
	_check("  playing, no desyncs", stats["state"] == "playing" and stats["desyncs"] == 0)
	_check("  the items' HUDs are there again (%d)" % huds, _game()._item_huds.size() == huds)

	if _other_port != 0 and _failures == 0:
		print("a server with other mods: the game starts again to join it")
		_check("  this server's items are loaded", not _game()._loaded_items.is_empty())
		_game()._toggle_pause()
		(_n("Leave") as Button).pressed.emit()
		await _until(func(): return _shown("Main"), 5.0)
		_type_and_join("127.0.0.1:%d" % _other_port)
		# The process ends here when it works; the restarted one writes the result.
		await _wait(15.0)
		_check("  restarted", false)
	_game()._stop()

	print("menu check: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)


## The run the game started by itself, straight into the second server.
func _after_restart() -> void:
	change_scene_to_file("res://game.tscn")
	var problems: PackedStringArray = []
	if not await _until(func(): return _game()._joined, 20.0):
		problems.append("did not join")
	else:
		await _wait(1.0)
		if not _game()._loaded_items.is_empty() or not _game()._item_huds.is_empty():
			problems.append("items of the first server are loaded")
		if _menu().is_open():
			problems.append("the menu is open")
		if _game()._link.port != _other_port:
			problems.append("joined port %d" % _game()._link.port)
		var recent: Array = _menu().recent_servers()
		if recent.size() != 2 or recent[0]["address"] != "127.0.0.1:%d" % _other_port:
			problems.append("recent servers are %s" % str(recent))
		_game()._stop()
	var text := "ok" if problems.is_empty() else ", ".join(problems)
	print("after the restart: ", text)
	if _result != "":
		var file := FileAccess.open(_result, FileAccess.WRITE)
		file.store_string(text)
		file.close()
	quit(0 if problems.is_empty() else 1)
