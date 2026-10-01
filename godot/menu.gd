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
##   [servers]  recent: the servers joined last, newest first

signal join_requested(host: String, port: int)
signal cancel_requested
signal resume_requested
signal leave_requested
signal quit_requested

const DEFAULT_PORT := 7777
const RECENT_LIMIT := 6
const PANELS := ["Main", "Connecting", "Pause", "Settings"]

## Mouse sensitivity as a multiplier; game.gd reads it.
var sensitivity := 1.0

var _scene: Node
var _config_path := "user://player.cfg"
var _settings_from := "" # the panel Back returns to


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--config="):
			_config_path = arg.substr(9)
	var packed := load("res://ui/menu.tscn") as PackedScene
	if packed == null:
		push_warning("res://ui/menu.tscn is missing: no menus")
		return
	_scene = packed.instantiate()
	add_child(_scene)
	_press("Join", _on_join)
	_press("Quit", func(): quit_requested.emit())
	_press("PauseQuit", func(): quit_requested.emit())
	_press("Cancel", func(): cancel_requested.emit())
	_press("Resume", func(): resume_requested.emit())
	_press("Leave", func(): leave_requested.emit())
	_press("OpenSettings", func(): _open_settings("Main"))
	_press("PauseSettings", func(): _open_settings("Pause"))
	_press("SettingsBack", func(): _show(_settings_from))
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
	return is_open() and (_visible("Pause") or (_visible("Settings") and _settings_from == "Pause"))


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
