extends SceneTree
## The Keys page of the settings, driven the way a player would, with no server:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_keys.gd -- --config=<abs file>
##
## It gives a few actions their keys as the game does, opens the page, changes one by pressing a
## key and another by pressing a mouse button, leaves one alone with Esc, starts the menu again (the
## choices must be remembered), and resets. --config keeps it out of the player's own file (the
## file is emptied first). Exit code 0 when everything held.

const Menu := preload("res://menu.gd")

var _failures := 0
var _config := ""


func _check(what: String, ok: bool) -> void:
	print("  %-70s %s" % [what, "ok" if ok else "FAIL"])
	if not ok:
		_failures += 1


func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--config="):
			_config = arg.substr(9)
	if _config == "":
		printerr("pass -- --config=<a file of the test's own>")
		quit(2)
		return
	if FileAccess.file_exists(_config):
		DirAccess.remove_absolute(_config)
	_run.call_deferred()


func _menu() -> Node:
	var menu := Menu.new()
	menu.name = "MenuDriver"
	root.add_child(menu)
	# As game.gd does: its own keys, then a server's, and a key a screen made by itself.
	menu.bind_key("game_jump", "Space", "Jump", "Game")
	menu.bind_key("game_use", "MouseLeft", "Use what is in the hand", "Game")
	menu.bind_key("cb_dash", "V", "Dash", "This server")
	return menu


func _key_of(action: String) -> String:
	var events := InputMap.action_get_events(action)
	return Menu.key_of_event(events[0]) if events.size() > 0 else ""


func _press_key(code: Key) -> void:
	var event := InputEventKey.new()
	event.physical_keycode = code
	event.pressed = true
	Input.parse_input_event(event)
	await process_frame
	await process_frame


func _press_mouse(button: MouseButton) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = button
	event.pressed = true
	Input.parse_input_event(event)
	await process_frame
	await process_frame


func _click(menu: Node, action: String) -> bool:
	var button := menu.find_child("Key_" + action, true, false) as Button
	if button == null:
		return false
	button.pressed.emit()
	await process_frame
	return true


func _run() -> void:
	var menu := _menu()
	await process_frame
	# A screen's own key, as a CbKey makes it when it first runs.
	InputMap.add_action("cb_inventory")
	var own := InputEventKey.new()
	own.physical_keycode = KEY_I
	InputMap.action_add_event("cb_inventory", own)

	print("the defaults")
	_check("jump is Space, use is the left button, dash is V", _key_of("game_jump") == "Space" and _key_of("game_use") == "MouseLeft" and _key_of("cb_dash") == "V")

	print("the page")
	menu._open_keys()
	await process_frame
	var rows := menu.find_child("KeyRows", true, false)
	_check("the page lists the four actions under three headings", rows != null and rows.get_child_count() == 7)
	_check("a screen's own key is listed with the key it has", menu.find_child("Key_cb_inventory", true, false) != null and menu.key_text("cb_inventory") == "I")

	print("choosing")
	_check("dash can be clicked", await _click(menu, "cb_dash"))
	_check("the game waits for a key", menu.is_capturing())
	await _press_key(KEY_G)
	_check("dash is G", _key_of("cb_dash") == "G" and not menu.is_capturing())
	await _click(menu, "game_jump")
	await _press_mouse(MOUSE_BUTTON_RIGHT)
	_check("jump is the right mouse button", _key_of("game_jump") == "MouseRight")
	await _click(menu, "game_use")
	await _press_key(KEY_ESCAPE)
	_check("Esc leaves use as it was", _key_of("game_use") == "MouseLeft" and not menu.is_capturing())
	await _click(menu, "cb_inventory")
	await _press_key(KEY_B)
	_check("the screen's key is B", _key_of("cb_inventory") == "B")
	await _click(menu, "game_use")
	await _press_mouse(MOUSE_BUTTON_WHEEL_UP)
	_check("a wheel is not a key: still waiting", menu.is_capturing() and _key_of("game_use") == "MouseLeft")
	await _press_key(KEY_ESCAPE)

	print("remembered")
	root.remove_child(menu)
	menu.free()
	for action in ["game_jump", "game_use", "cb_dash", "cb_inventory"]:
		InputMap.erase_action(action)
	menu = _menu()
	await process_frame
	_check("a new start: dash is G, jump the right button, use unchanged", _key_of("cb_dash") == "G" and _key_of("game_jump") == "MouseRight" and _key_of("game_use") == "MouseLeft")
	# The screen's key comes back when its screen runs again with its default.
	InputMap.add_action("cb_inventory")
	own = InputEventKey.new()
	own.physical_keycode = KEY_I
	InputMap.action_add_event("cb_inventory", own)
	menu._open_keys()
	await process_frame
	_check("the screen's key is B again", _key_of("cb_inventory") == "B")
	# Another server: its actions go, what was chosen for them stays for the next time.
	menu.forget_keys("This server")
	_check("leaving a server takes its actions away", not InputMap.has_action("cb_dash"))
	menu.bind_key("cb_dash", "V", "Dash", "This server")
	_check("and joining again brings the player's key back", _key_of("cb_dash") == "G")

	print("reset")
	menu._open_keys()
	await process_frame
	menu._reset_keys()
	_check("everything is its default again", _key_of("cb_dash") == "V" and _key_of("game_jump") == "Space" and _key_of("cb_inventory") == "I")
	var saved := ConfigFile.new()
	saved.load(_config)
	_check("and nothing is kept", (saved.get_value("keys", "bound", {}) as Dictionary).is_empty())

	print("keys: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(1 if _failures > 0 else 0)
