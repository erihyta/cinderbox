extends Node3D
## Base game presentation: input, camera, HUD binding and the VFX director.
##
## Everything visual it uses is loaded by path, so mods can replace it:
##   res://ui/hud.tscn          HUD layout. Optional unique nodes: %Stats, %Banner, %Help, %Name.
##   res://ui/hud_*.tscn        HUDs that come with workshop items, laid over the game's.
##   res://vfx/bindings*.tres   effect bindings (CbEffectTable): scenes, sounds and screen effects,
##                              plus state bindings (held items, aimed arms).
##   res://vfx/<event>.tscn     fallback one-shot effects: prop_spawn, prop_destroy, jump, land.
##   res://prefabs/*.tscn       entity visuals (loaded by CinderboxClient).
##
## The game rules live in the server's mods. This script only knows the engine's own controls
## (move, sprint, jump, camera); everything else is an action the server declared, bound to the key
## it suggested, and every mod event plays whatever the bindings say.
##
## Joining: the server announces the workshop items its mods need. They must all be in the local
## workshop (workshop.gd), exactly as announced, or the game leaves and says what is missing. Loaded
## items come before the player's own mods, which are loaded again after them.
##
## Command line (after `--`): --host=H --port=P --name=NAME --rollback=N --animations=DIR
##                            --autoplay=SECONDS --screenshot=FILE --screenshot-every=SECONDS
##                            --mods=DIR --workshop=DIR
## With --screenshot-every, autoplay also saves FILE_1.png, FILE_2.png, ... along the way.

const MOUSE_SENSITIVITY := 0.003
const VFX_LIFETIME := 3.0
const ACTION_PREFIX := "cb_"
const Boot := preload("res://boot.gd")
const Workshop := preload("res://workshop.gd")
const SETTINGS := "user://player.cfg"

@onready var client: CinderboxClient = $Client
@onready var camera: Camera3D = $Camera

var yaw := PI # facing +Z like the server's spawn orientation
var pitch := -0.35
var distance := 6.0
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
var _loaded_items := {} # sha256 -> true, loaded this session
var _item_huds: Array[Node] = []
var _refused := "" # why this server cannot be joined, shown on the banner

var _vfx_cache := {}
var _sound_cache := {}
var _effects: Array = []
var _cooldowns := {}
var _actions: Array = [] # [{ name, bit, key }] from the server's mods

var _shake := 0.0
var _shake_decay := 1.0
var _flash := 0.0
var _flash_decay := 1.0
var _flash_color := Color.WHITE
var _flash_rect: ColorRect


func _ready() -> void:
	args = _parse_args()
	client.host = args.get("host", "127.0.0.1")
	client.port = int(args.get("port", "7777"))
	if args.has("rollback"):
		client.rollback_min = int(args["rollback"])
		client.rollback_max = int(args["rollback"])
	if args.has("animations"):
		client.animation_dir = args["animations"]
	client.player_name = _player_name()
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
	client.visual_destroying.connect(_on_visual_destroying)
	client.player_jumped.connect(func(_id, pos, is_local):
		_play_effects(CbEffect.EVENT_JUMPED, "jump", {"kind": "player", "position": pos, "is_local": is_local}))
	client.player_landed.connect(func(_id, pos, is_local):
		_play_effects(CbEffect.EVENT_LANDED, "land", {"kind": "player", "position": pos, "is_local": is_local}))
	client.footstep.connect(func(_id, pos, is_local):
		_play_effects(CbEffect.EVENT_FOOTSTEP, "", {"kind": "player", "position": pos, "is_local": is_local}))
	client.impact.connect(func(_id, pos, strength, kind, template_name):
		_play_effects(CbEffect.EVENT_IMPACT, "", {"kind": kind, "template": template_name, "position": pos,
			"strength": strength}))
	client.mod_event.connect(_on_mod_event)
	client.action_pressed.connect(_on_action_pressed)
	client.schema_changed.connect(_on_schema_changed)
	client.connection_state_changed.connect(func(state): print("connection: ", state))

	_load_effects()
	_make_flash_overlay()

	var hud_scene: PackedScene = load("res://ui/hud.tscn")
	if hud_scene:
		hud = hud_scene.instantiate()
		add_child(hud)
		var name_edit := hud.get_node_or_null("%Name") as LineEdit
		if name_edit:
			name_edit.text = client.player_name
			name_edit.text_submitted.connect(_on_name_submitted)

	if autoplay <= 0.0:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	client.connect_to_server()


func _parse_args() -> Dictionary:
	var result := {}
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--") and arg.contains("="):
			var eq := arg.find("=")
			result[arg.substr(2, eq - 2)] = arg.substr(eq + 1)
	return result


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		yaw -= event.relative.x * MOUSE_SENSITIVITY
		pitch = clamp(pitch - event.relative.y * MOUSE_SENSITIVITY, -1.3, 0.4)
	elif event is InputEventMouseButton and event.pressed:
		match event.button_index:
			MOUSE_BUTTON_WHEEL_UP:
				distance = max(distance - 0.5, 2.0)
			MOUSE_BUTTON_WHEEL_DOWN:
				distance = min(distance + 0.5, 20.0)
			MOUSE_BUTTON_LEFT:
				Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	elif event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_ESCAPE:
				Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED else Input.MOUSE_MODE_CAPTURED
			KEY_F1:
				show_debug = not show_debug


func _process(delta: float) -> void:
	_send_input(delta)
	_update_screen_effects(delta)
	_update_camera()
	_update_hud()
	_autoplay_finish()


# --- Names and workshop items ---------------------------------------------------------------------

func _player_name() -> String:
	if args.has("name"):
		return args["name"]
	var config := ConfigFile.new()
	if config.load(SETTINGS) == OK:
		return String(config.get_value("player", "name", ""))
	return ""


func _on_name_submitted(text: String) -> void:
	var config := ConfigFile.new()
	config.load(SETTINGS)
	config.set_value("player", "name", text.strip_edges())
	config.save(SETTINGS)
	# The server learns names when a player joins, so a new name means joining again.
	client.player_name = text.strip_edges()
	client.disconnect_from_server()
	client.connect_to_server()
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED


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
		_reload_presentation()
	return true


func _refuse(reason: String) -> void:
	_refused = reason
	push_warning(reason.replace("\n", " "))
	client.disconnect_from_server()


## Bindings and item HUDs, loaded again now that items may have added or replaced some.
func _reload_presentation() -> void:
	_effects.clear()
	_cooldowns.clear()
	client.clear_state_bindings()
	_load_effects()
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
		# Scripted player for unattended runs: props first, then the pistol.
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
		else:
			actions |= _action_bit("slot_2")
			if auto_rng.randf() < delta * 3.0:
				actions |= _action_bit("fire")
		# Hold Tab at the end, so screenshots show the scoreboard too.
		if elapsed > autoplay * 0.8 and not Input.is_action_pressed("scoreboard"):
			Input.action_press("scoreboard")
	elif get_window().has_focus():
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
	camera.global_position = target + camera.global_transform.basis.z * distance
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
	var text := "WASD move   Shift sprint   Space jump"
	for action in _actions:
		var key: String = String(action["key"]).replace("Mouse", "Mouse ")
		text += "   %s %s" % [key, String(action["name"]).replace("_", " ")]
	help.text = text + "   Tab scores   Mouse orbit   Wheel zoom   Esc name & cursor   F1 stats"


func _update_hud() -> void:
	if hud == null:
		return
	var stats: Dictionary = client.get_stats()
	var label := hud.get_node_or_null("%Stats") as Label
	if label:
		label.visible = show_debug
		label.text = "%d FPS   %s\ntick %d   confirmed %d   window %d\nrtt %d ms   rollbacks %d (last %d)   stalled %.1f s\nchecksums ok %d   desyncs %d\nentities %d   animation: %s\nmods: %s" % [
			Engine.get_frames_per_second(), stats.get("state", ""),
			stats.get("tick", 0), stats.get("confirmed_tick", 0), stats.get("rollback_window", 0),
			stats.get("rtt_ms", 0), stats.get("rollbacks", 0), stats.get("last_rollback_depth", 0), stats.get("stalled_seconds", 0.0),
			stats.get("checksums_verified", 0), stats.get("desyncs", 0),
			stats.get("entities", 0), stats.get("animation", ""), ", ".join(client.get_mod_names())]
	var name_edit := hud.get_node_or_null("%Name") as LineEdit
	if name_edit:
		name_edit.visible = Input.mouse_mode != Input.MOUSE_MODE_CAPTURED and autoplay <= 0.0
	var banner := hud.get_node_or_null("%Banner") as Label
	if banner:
		var state: String = stats.get("state", "")
		banner.visible = state != "playing"
		if _refused != "" and state == "stopped":
			banner.text = _refused
		elif state == "rejected":
			banner.text = "Rejected: %s" % stats.get("reject_reason", "")
		elif state == "reconnecting" or (state == "joining" and stats.get("welcomes", 0) > 0):
			banner.text = "Connection lost - time is paused, reconnecting..."
		else:
			banner.text = "Connecting..."


func _autoplay_finish() -> void:
	if autoplay <= 0.0:
		return
	if _refused != "":
		print("autoplay refused: ", _refused.replace("\n", " "))
		autoplay = 0.0
		get_tree().quit(3)
		return
	if playing_since < 0.0:
		if client.get_connection_state() == "playing":
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
	client.disconnect_from_server()
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


# --- VFX director -------------------------------------------------------------------------------
#
# What plays when is data: every res://vfx/bindings*.tres is loaded, so a mod adds effects by
# adding a file of its own instead of replacing the game's. A binding picks an event and may narrow
# it to one map template, one kind of entity, the local player, or board conditions. When nothing
# matches, the old convention still applies: res://vfx/<event>.tscn.
#
# Mod events name two entities: a (who it is about) and b (the other one). A binding picks which
# of them is its subject, and kind, template, who, conditions and bone are all checked on it.

func _load_effects() -> void:
	var names := []
	for file in DirAccess.get_files_at("res://vfx"):
		# Exported games list resources under their remapped names.
		var clean: String = file.trim_suffix(".remap")
		if clean.begins_with("bindings") and (clean.ends_with(".tres") or clean.ends_with(".res")):
			names.append(clean)
	names.sort()
	var states := 0
	for file in names:
		var table = ResourceLoader.load("res://vfx/%s" % file, "", ResourceLoader.CACHE_MODE_REPLACE)
		if table is CbEffectTable:
			for effect in table.effects:
				if effect is CbEffect:
					_effects.append(effect)
			for state in table.states:
				if state is CbStateBinding:
					client.add_state_binding(state)
					states += 1
		else:
			push_warning("vfx/%s is not a CbEffectTable" % file)
	print("effect bindings: %d, state bindings: %d, from %d file(s)" % [_effects.size(), states, names.size()])


func _on_mod_event(name: String, a: int, b: int, value: int, position: Vector3, vector: Vector3) -> void:
	_event_counts[name] = _event_counts.get(name, 0) + 1
	_play_effects(CbEffect.EVENT_MOD, "", {"name": name, "subjects": [a, b], "value": value, "position": position,
		"end": vector})


func _on_action_pressed(name: String) -> void:
	var me: int = client.get_local_net_id()
	if me == 0:
		return
	_play_effects(CbEffect.EVENT_ACTION, "", {"name": name, "subjects": [me, 0], "value": 0,
		"position": client.get_camera_target(), "end": Vector3.ZERO})


# The kind, template and locality of the entity a binding is about.
func _subject(effect: CbEffect, ctx: Dictionary) -> Dictionary:
	if not ctx.has("subjects"):
		return ctx
	var id: int = ctx["subjects"][effect.subject]
	return {"id": id, "kind": client.get_kind(id), "template": client.get_entity_template_name(id),
		"is_local": id != 0 and id == client.get_local_net_id()}


func _play_effects(event: int, fallback: String, ctx: Dictionary) -> void:
	var played := false
	var now := Time.get_ticks_msec() / 1000.0
	for effect in _effects:
		if effect.event != event:
			continue
		if (event == CbEffect.EVENT_MOD or event == CbEffect.EVENT_ACTION) and effect.name != ctx.get("name", ""):
			continue
		var subject := _subject(effect, ctx)
		if effect.template_name != "" and effect.template_name != subject.get("template", ""):
			continue
		if effect.kind != "" and effect.kind != "any" and effect.kind != subject.get("kind", ""):
			continue
		if effect.who == CbEffect.WHO_LOCAL and not subject.get("is_local", false):
			continue
		if effect.who == CbEffect.WHO_REMOTE and subject.get("is_local", false):
			continue
		if effect.min_strength > 0.0 and ctx.get("strength", 0.0) < effect.min_strength:
			continue
		var value: int = ctx.get("value", 0)
		if effect.value_filter == CbEffect.VALUE_POSITIVE and value <= 0:
			continue
		if effect.value_filter == CbEffect.VALUE_ZERO and value != 0:
			continue
		if not effect.conditions.is_empty() and not client.check_conditions(subject.get("id", 0), effect.conditions):
			continue
		if effect.cooldown > 0.0:
			# Keeps a busy event (twenty props at once) from stacking twenty sounds.
			var last: float = _cooldowns.get(effect, -1e9)
			if now - last < effect.cooldown:
				continue
			_cooldowns[effect] = now

		var position: Vector3 = ctx.get("position", Vector3.ZERO)
		if effect.at_end:
			position = ctx.get("end", position)
		if effect.bone != "" and subject.get("id", 0) != 0:
			position = client.get_bone_position(subject["id"], effect.bone)
		var follow: Node3D = ctx.get("node", null)
		if follow == null and effect.follow and subject.get("id", 0) != 0:
			follow = client.get_entity_node(subject["id"])
		if effect.beam:
			_play_beam(effect.scene, position + effect.offset, ctx.get("end", position), effect.lifetime)
		else:
			_play_scene(effect.scene, position + effect.offset, effect.lifetime, follow if effect.follow else null)
		_play_sound(effect, position + effect.offset)
		if effect.shake > 0.0:
			_shake = max(_shake, effect.shake)
			_shake_decay = effect.shake / max(effect.shake_time, 0.05)
		if effect.flash_color.a > 0.0:
			_flash_color = effect.flash_color
			_flash = effect.flash_color.a
			_flash_decay = effect.flash_color.a / max(effect.flash_time, 0.02)
		played = true
	if not played and fallback != "":
		_play_scene("res://vfx/%s.tscn" % fallback, ctx.get("position", Vector3.ZERO), VFX_LIFETIME, null)


func _play_sound(effect: CbEffect, position: Vector3) -> void:
	if effect.sound == "":
		return
	if not _sound_cache.has(effect.sound):
		_sound_cache[effect.sound] = load(effect.sound) if ResourceLoader.exists(effect.sound) else null
		if _sound_cache[effect.sound] == null:
			push_warning("missing sound %s" % effect.sound)
	var stream: AudioStream = _sound_cache[effect.sound]
	if stream == null:
		return
	var player := AudioStreamPlayer3D.new()
	player.stream = stream
	player.volume_db = effect.volume_db
	player.pitch_scale = max(0.01, effect.pitch_scale + auto_rng.randf_range(-effect.pitch_jitter, effect.pitch_jitter))
	if effect.bus != "":
		player.bus = effect.bus
	if effect.max_distance > 0.0:
		player.max_distance = effect.max_distance
	add_child(player)
	player.global_position = position
	player.finished.connect(player.queue_free)
	player.play()


func _update_screen_effects(delta: float) -> void:
	_shake = max(0.0, _shake - _shake_decay * delta)
	if _flash > 0.0:
		_flash = max(0.0, _flash - _flash_decay * delta)
		_flash_rect.color = Color(_flash_color.r, _flash_color.g, _flash_color.b, _flash)
		_flash_rect.visible = _flash > 0.0


func _instance(path: String) -> Node3D:
	if path == "":
		return null
	if not _vfx_cache.has(path):
		_vfx_cache[path] = load(path) if ResourceLoader.exists(path) else null
		if _vfx_cache[path] == null:
			push_warning("missing effect scene %s" % path)
	var scene: PackedScene = _vfx_cache[path]
	if scene == null:
		return null
	return scene.instantiate() as Node3D


func _start(node: Node3D, lifetime: float) -> void:
	for particles in node.find_children("*", "GPUParticles3D", true, false) + ([node] if node is GPUParticles3D else []):
		particles.restart()
	get_tree().create_timer(lifetime).timeout.connect(func():
		if is_instance_valid(node):
			node.queue_free())


func _play_scene(path: String, position: Vector3, lifetime: float, parent: Node3D) -> void:
	var node := _instance(path)
	if node == null:
		return
	# Following an entity means living under it, so it dies with it too.
	if parent != null and is_instance_valid(parent):
		parent.add_child(node)
		node.position = Vector3.ZERO
	else:
		add_child(node)
		node.global_position = position
	_start(node, lifetime)


# A one-metre scene stretched along its -Z from `from` to `to`, like a tracer.
func _play_beam(path: String, from: Vector3, to: Vector3, lifetime: float) -> void:
	var length := from.distance_to(to)
	if length < 0.01:
		return
	var node := _instance(path)
	if node == null:
		return
	add_child(node)
	node.global_position = from
	node.look_at(to, Vector3.UP if abs((to - from).normalized().y) < 0.99 else Vector3.RIGHT)
	node.scale = Vector3(1, 1, length)
	_start(node, lifetime)


func _on_visual_spawned(_visual_id: int, net_id: int, kind: String, node: Node3D, position: Vector3, with_effect: bool,
		template_name: String) -> void:
	if kind == "prop" and node != null and node.has_meta("tint_by_net_id"):
		_tint(node, net_id)
	if not with_effect:
		return
	_play_effects(CbEffect.EVENT_SPAWNED, "prop_spawn" if kind == "prop" else "",
		{"kind": kind, "template": template_name, "position": position, "node": node})


func _on_visual_destroying(_visual_id: int, _net_id: int, kind: String, position: Vector3, template_name: String) -> void:
	_play_effects(CbEffect.EVENT_DESTROYING, "prop_destroy" if kind == "prop" else "",
		{"kind": kind, "template": template_name, "position": position})


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
