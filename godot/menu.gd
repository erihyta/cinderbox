extends Node
## The game's menus: joining a server, the in-game (Esc) menu, settings.
##
## The layout is res://ui/menu.tscn, a scene without scripts that a mod can restyle; this script
## finds its nodes by unique name (listed at the top of that scene) and skips any that are missing.
## game.gd decides when each panel shows and does the joining and leaving.
##
## What it remembers lives in user://player.cfg (or --config=FILE):
##   [player]   name
##   [settings] sensitivity (0.2-3), volume (0-100), fullscreen
##   [keys]     an action's name = the key the player chose for it ("cb_dash" = "MouseRight")
##   [servers]  recent: the servers joined last, newest first

signal join_requested(host: String, port: int)
## A server of the player's own: start one, join it (host.gd).
signal host_requested
signal cancel_requested
signal resume_requested
signal leave_requested
signal quit_requested

const DEFAULT_PORT := 7777
const RECENT_LIMIT := 6
const PANELS := ["Main", "Connecting", "Pause", "Settings", "Keys"]

## Mouse sensitivity as a multiplier; game.gd reads it.
var sensitivity := 1.0

var _scene: Node
var _config_path := "user://player.cfg"
var _settings_from := "" # the panel Back returns to

## Keys. Every action the player can rebind is an InputMap action that somebody gave a key: the
## game its own (game_*), the server's mods theirs (cb_<action>: game.gd binds them on joining), a
## HUD's CbKey its own (cb_<name>, when its screen first runs). `_keys` is what the player chose
## instead, by the action's name; it is kept for actions that are not there now (another server's).
var _keys := {}		   # action -> key ("V", "MouseRight")
var _key_info := {}	   # action -> { "key": the default, "label": String, "group": String }
var _capturing := ""   # the action whose next key press is its new key
var _apply_in := 0.0


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--config="):
			_config_path = arg.substr(9)
	var packed := load("res://ui/menu.tscn") as PackedScene
	if packed == null:
		push_warning("res://ui/menu.tscn is missing: no menus")
		return
	# (Through the guard, like every scene a mod can replace.)
	_scene = CbDirector.instantiate(packed)
	if _scene == null:
		push_warning("res://ui/menu.tscn was refused: no menus")
		return
	add_child(_scene)
	_press("Join", _on_join)
	_press("Host", _on_host)
	_press("Quit", func(): quit_requested.emit())
	_press("PauseQuit", func(): quit_requested.emit())
	_press("Cancel", func(): cancel_requested.emit())
	_press("Resume", func(): resume_requested.emit())
	_press("Leave", func(): leave_requested.emit())
	_press("OpenSettings", func(): _open_settings("Main"))
	_press("PauseSettings", func(): _open_settings("Pause"))
	_press("SettingsBack", func(): _show(_settings_from))
	_press("OpenKeys", _open_keys)
	_press("KeysBack", func(): _capturing = ""; _show("Settings"))
	_press("KeysReset", _reset_keys)
	var saved: Variant = _config().get_value("keys", "bound", {})
	_keys = saved if saved is Dictionary else {}
	var address := _node("Address") as LineEdit
	if address:
		address.text_submitted.connect(func(_text): _on_join())
	var name_edit := _node("Name") as LineEdit
	if name_edit:
		name_edit.text_submitted.connect(func(_text): _on_join())
	_load_settings()
	_show("")


# --- What game.gd asks for ------------------------------------------------------------------------

## The join panel, with `message` above it (why the last attempt ended, or "").
func show_main(message := "") -> void:
	var label := _node("Message") as Label
	if label:
		label.text = message
		label.visible = message != ""
	var name_edit := _node("Name") as LineEdit
	if name_edit:
		name_edit.text = player_name()
	var recent := recent_servers()
	var address := _node("Address") as LineEdit
	if address:
		if address.text == "" and not recent.is_empty():
			address.text = String(recent[0].get("address", ""))
		address.grab_focus.call_deferred()
	_fill_recent(recent)
	_show("Main")


func show_connecting(address: String) -> void:
	var label := _node("ConnectingText") as Label
	if label:
		label.text = "Connecting to %s..." % address
	_show("Connecting")


func show_pause(text: String) -> void:
	var label := _node("PauseText") as Label
	if label:
		label.text = text
	_show("Pause")


func close() -> void:
	_show("")


func is_open() -> bool:
	return _scene != null and _scene.visible


## True while the in-game menu (or its settings) is up, as opposed to the join panels.
func is_paused() -> bool:
	return is_open() and (_visible("Pause") or ((_visible("Settings") or _visible("Keys")) and _settings_from == "Pause"))


# --- Addresses ------------------------------------------------------------------------------------

## "host" or "host:port" -> { host, port } or { error }.
static func parse_address(text: String) -> Dictionary:
	var clean := text.strip_edges()
	if clean == "":
		return {"error": "Type a server address, like 203.0.113.5 or play.example.org:7777."}
	if clean.contains(" ") or clean.contains("/"):
		return {"error": "\"%s\" is not an address. Use a host name or IP, with :port if it is not %d." % [clean, DEFAULT_PORT]}
	var host := clean
	var port := DEFAULT_PORT
	var colons := clean.count(":")
	if colons > 1:
		return {"error": "IPv6 addresses are not supported yet. Use the server's IPv4 address or host name."}
	if colons == 1:
		host = clean.get_slice(":", 0)
		var port_text := clean.get_slice(":", 1)
		if not port_text.is_valid_int() or int(port_text) < 1 or int(port_text) > 65535:
			return {"error": "\"%s\" is not a port. Ports are numbers from 1 to 65535." % port_text}
		port = int(port_text)
	if host == "":
		return {"error": "The address has no host before the colon."}
	return {"host": host, "port": port}


static func format_address(host: String, port: int) -> String:
	return host if port == DEFAULT_PORT else "%s:%d" % [host, port]


func _on_host() -> void:
	var name_edit := _node("Name") as LineEdit
	if name_edit:
		set_player_name(name_edit.text)
	host_requested.emit()


func _on_join() -> void:
	var name_edit := _node("Name") as LineEdit
	if name_edit:
		set_player_name(name_edit.text)
	var address := _node("Address") as LineEdit
	var parsed := parse_address(address.text if address else "")
	if parsed.has("error"):
		show_main(parsed["error"])
		return
	join_requested.emit(parsed["host"], parsed["port"])


# --- What is remembered ---------------------------------------------------------------------------

func _config() -> ConfigFile:
	var config := ConfigFile.new()
	config.load(_config_path)
	return config


func player_name() -> String:
	return String(_config().get_value("player", "name", ""))


func set_player_name(text: String) -> void:
	var config := _config()
	config.set_value("player", "name", text.strip_edges())
	config.save(_config_path)


## [{ address, map }], newest first.
func recent_servers() -> Array:
	var list = _config().get_value("servers", "recent", [])
	var result := []
	if list is Array:
		for entry in list:
			if entry is Dictionary and entry.has("address"):
				result.append(entry)
	return result


## Called once a join has succeeded: the server goes to the top of the list.
func remember_server(host: String, port: int, map_name: String) -> void:
	var address := format_address(host, port)
	var list := recent_servers().filter(func(entry): return entry["address"] != address)
	list.push_front({"address": address, "map": map_name})
	var config := _config()
	config.set_value("servers", "recent", list.slice(0, RECENT_LIMIT))
	config.save(_config_path)


func _fill_recent(recent: Array) -> void:
	var box := _node("Recent")
	if box == null:
		return
	for child in box.get_children():
		box.remove_child(child)
		child.queue_free()
	if recent.is_empty():
		var none := Label.new()
		none.text = "None yet. Servers you join are listed here."
		none.modulate = Color(1, 1, 1, 0.5)
		box.add_child(none)
		return
	for entry in recent:
		var button := Button.new()
		var map_name := String(entry.get("map", ""))
		button.text = String(entry["address"]) + ("" if map_name == "" else "     %s" % map_name)
		button.alignment = HORIZONTAL_ALIGNMENT_LEFT
		button.pressed.connect(_on_recent.bind(String(entry["address"])))
		box.add_child(button)


func _on_recent(address: String) -> void:
	var edit := _node("Address") as LineEdit
	if edit:
		edit.text = address
	_on_join()


# --- Settings -------------------------------------------------------------------------------------

func _load_settings() -> void:
	var config := _config()
	sensitivity = clampf(float(config.get_value("settings", "sensitivity", 1.0)), 0.2, 3.0)
	var volume := clampf(float(config.get_value("settings", "volume", 100.0)), 0.0, 100.0)
	var fullscreen := bool(config.get_value("settings", "fullscreen", false))
	_apply_volume(volume)
	if fullscreen:
		_apply_fullscreen(true)
	var slider := _node("Sensitivity") as Range
	if slider:
		slider.value = sensitivity
		slider.value_changed.connect(_on_sensitivity)
	var volume_slider := _node("Volume") as Range
	if volume_slider:
		volume_slider.value = volume
		volume_slider.value_changed.connect(_on_volume)
	var check := _node("Fullscreen") as BaseButton
	if check:
		check.button_pressed = fullscreen
		check.toggled.connect(_on_fullscreen)
	_set_text("SensitivityValue", "%.2f" % sensitivity)
	_set_text("VolumeValue", "%d%%" % int(volume))


func _save_setting(key: String, value: Variant) -> void:
	var config := _config()
	config.set_value("settings", key, value)
	config.save(_config_path)


func _on_sensitivity(value: float) -> void:
	sensitivity = value
	_set_text("SensitivityValue", "%.2f" % value)
	_save_setting("sensitivity", value)


func _on_volume(value: float) -> void:
	_apply_volume(value)
	_set_text("VolumeValue", "%d%%" % int(value))
	_save_setting("volume", value)


func _on_fullscreen(on: bool) -> void:
	_apply_fullscreen(on)
	_save_setting("fullscreen", on)


func _apply_volume(percent: float) -> void:
	AudioServer.set_bus_mute(0, percent <= 0.0)
	AudioServer.set_bus_volume_db(0, linear_to_db(maxf(percent, 1.0) / 100.0))


func _apply_fullscreen(on: bool) -> void:
	DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_FULLSCREEN if on else DisplayServer.WINDOW_MODE_WINDOWED)


func _open_settings(from: String) -> void:
	_settings_from = from
	_show("Settings")


# --- Keys -----------------------------------------------------------------------------------------

## Gives `action` (an InputMap action's whole name) its key: the player's own if one was chosen,
## else `default_key` ("W", "Shift", "MouseLeft"). `group` is the heading it is listed under.
func bind_key(action: String, default_key: String, label: String, group: String) -> void:
	_key_info[action] = {"key": default_key, "label": label, "group": group}
	_set_action_key(action, String(_keys.get(action, default_key)))


## Forgets a group's actions (the last server's, when another is joined). What the player chose
## for them is kept.
func forget_keys(group: String) -> void:
	for action in _key_info.keys():
		if _key_info[action]["group"] == group:
			if InputMap.has_action(action):
				InputMap.erase_action(action)
			_key_info.erase(action)


## What `action` is bound to now, as a key cap says it ("V", "Mouse Right"); "" when unbound.
func key_text(action: String) -> String:
	if not InputMap.has_action(action):
		return ""
	for event in InputMap.action_get_events(action):
		return key_of_event(event).replace("Mouse", "Mouse ").replace("QuoteLeft", "`")
	return ""


func is_capturing() -> bool:
	return _capturing != ""


static func event_for_key(key: String) -> InputEvent:
	var buttons := {"MouseLeft": MOUSE_BUTTON_LEFT, "MouseRight": MOUSE_BUTTON_RIGHT, "MouseMiddle": MOUSE_BUTTON_MIDDLE,
		"MouseBack": MOUSE_BUTTON_XBUTTON1, "MouseForward": MOUSE_BUTTON_XBUTTON2}
	if buttons.has(key):
		var mouse := InputEventMouseButton.new()
		mouse.button_index = buttons[key]
		return mouse
	var code := OS.find_keycode_from_string(key)
	if code == KEY_NONE:
		return null
	var ev := InputEventKey.new()
	ev.physical_keycode = code
	return ev


## The name a key or a mouse button is saved and suggested by; "" for what cannot be bound.
static func key_of_event(event: InputEvent) -> String:
	if event is InputEventMouseButton:
		return String({MOUSE_BUTTON_LEFT: "MouseLeft", MOUSE_BUTTON_RIGHT: "MouseRight", MOUSE_BUTTON_MIDDLE: "MouseMiddle",
			MOUSE_BUTTON_XBUTTON1: "MouseBack", MOUSE_BUTTON_XBUTTON2: "MouseForward"}.get(event.button_index, ""))
	if event is InputEventKey:
		var code: int = event.physical_keycode if event.physical_keycode != KEY_NONE else event.keycode
		return OS.get_keycode_string(code)
	return ""


func _set_action_key(action: String, key: String) -> void:
	var event := event_for_key(key)
	if event == null:
		push_warning("\"%s\" is not a key Godot knows (the key of %s)" % [key, action])
	if not InputMap.has_action(action):
		InputMap.add_action(action)
	InputMap.action_erase_events(action)
	if event != null:
		InputMap.action_add_event(action, event)


## Actions that appeared by themselves (a HUD's CbKey gives its own a key when its screen first
## runs) are listed too, and get the player's key if one was chosen for them.
func _adopt_keys() -> void:
	for name in InputMap.get_actions():
		var action := String(name)
		if not action.begins_with("cb_") or _key_info.has(action):
			continue
		var events := InputMap.action_get_events(action)
		var key := key_of_event(events[0]) if events.size() > 0 else ""
		_key_info[action] = {"key": key, "label": action.substr(3).replace("_", " ").capitalize(), "group": "Screens"}
		if _keys.has(action):
			_set_action_key(action, String(_keys[action]))


func _process(delta: float) -> void:
	# (A screen's key exists from the first frame its node runs: looked for now and then.)
	_apply_in -= delta
	if _apply_in <= 0.0:
		_apply_in = 0.5
		_adopt_keys()


func _open_keys() -> void:
	_adopt_keys()
	_fill_keys()
	_show("Keys")


func _fill_keys() -> void:
	var rows := _node("KeyRows")
	if rows == null:
		return
	for child in rows.get_children():
		rows.remove_child(child)
		child.queue_free()
	var groups := []
	for action in _key_info:
		if not groups.has(_key_info[action]["group"]):
			groups.append(_key_info[action]["group"])
	for group in groups:
		var heading := Label.new()
		heading.text = group
		heading.add_theme_color_override("font_color", Color(0.75, 0.78, 0.82))
		rows.add_child(heading)
		for action in _key_info:
			if _key_info[action]["group"] != group:
				continue
			var row := HBoxContainer.new()
			var label := Label.new()
			label.text = String(_key_info[action]["label"])
			label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			row.add_child(label)
			var button := Button.new()
			button.custom_minimum_size = Vector2(170, 0)
			button.name = "Key_" + action
			button.text = "press a key..." if _capturing == action else key_text(action)
			if _keys.has(action) and String(_keys[action]) != String(_key_info[action]["key"]):
				button.tooltip_text = "Yours. The default is %s." % String(_key_info[action]["key"])
			# Two actions on one key both happen: said, not forbidden (a mod may mean it).
			var shared := []
			for other in _key_info:
				if other != action and key_text(other) != "" and key_text(other) == key_text(action):
					shared.append(String(_key_info[other]["label"]))
			if not shared.is_empty() and _capturing != action:
				button.text += "  *"
				button.tooltip_text = (button.tooltip_text + " " if button.tooltip_text != "" else "") + "Also the key of: %s." % ", ".join(shared)
			button.pressed.connect(func(): _capturing = action; _fill_keys())
			row.add_child(button)
			rows.add_child(row)
	if groups.is_empty():
		var none := Label.new()
		none.text = "Nothing to bind yet."
		rows.add_child(none)


## While a key is being chosen, the next key or mouse button is it.
func _input(event: InputEvent) -> void:
	if _capturing == "" or not _visible("Keys"):
		return
	var pressed: bool = (event is InputEventKey and event.pressed and not event.echo) or (event is InputEventMouseButton and event.pressed)
	if not pressed:
		return
	get_viewport().set_input_as_handled()
	if event is InputEventKey and event.physical_keycode == KEY_ESCAPE:
		_capturing = ""
		_fill_keys()
		return
	var key := key_of_event(event)
	if key == "":
		return # a wheel: not a key
	var action := _capturing
	_capturing = ""
	if _key_info.has(action) and key == String(_key_info[action]["key"]):
		_keys.erase(action)
	else:
		_keys[action] = key
	_set_action_key(action, key)
	_save_keys()
	_fill_keys()


func _reset_keys() -> void:
	_capturing = ""
	for action in _key_info:
		if _keys.has(action):
			_keys.erase(action)
			_set_action_key(action, String(_key_info[action]["key"]))
	_save_keys()
	_fill_keys()


func _save_keys() -> void:
	var config := _config()
	config.set_value("keys", "bound", _keys)
	config.save(_config_path)


# --- Nodes ----------------------------------------------------------------------------------------

func _node(unique: String) -> Node:
	return _scene.get_node_or_null("%" + unique) if _scene else null


func _press(unique: String, action: Callable) -> void:
	var button := _node(unique) as BaseButton
	if button:
		button.pressed.connect(action)


func _set_text(unique: String, text: String) -> void:
	var label := _node(unique) as Label
	if label:
		label.text = text


func _visible(panel: String) -> bool:
	var node := _node(panel) as CanvasItem
	return node != null and node.visible


## One panel, or none ("" hides the whole menu).
func _show(panel: String) -> void:
	if _scene == null:
		return
	_scene.visible = panel != ""
	for name in PANELS:
		var node := _node(name) as CanvasItem
		if node:
			node.visible = name == panel
