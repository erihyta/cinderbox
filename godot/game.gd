extends Node3D
## Base game presentation: input, camera, HUD binding and the VFX director.
##
## Everything visual it uses is loaded by path, so mods can replace it:
##   res://ui/hud.tscn          HUD layout. Optional unique nodes: %Stats, %Banner, %Help.
##   res://vfx/<event>.tscn     one-shot effects: prop_spawn, prop_destroy, jump, land.
##   res://prefabs/*.tscn       entity visuals (loaded by CinderboxClient).
##
## Command line (after `--`): --host=H --port=P --rollback=N --animations=DIR
##                            --autoplay=SECONDS --screenshot=FILE --mods=DIR

const MOUSE_SENSITIVITY := 0.003
const VFX_LIFETIME := 3.0

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

var _vfx_cache := {}


func _ready() -> void:
	args = _parse_args()
	client.host = args.get("host", "127.0.0.1")
	client.port = int(args.get("port", "7777"))
	if args.has("rollback"):
		client.rollback_min = int(args["rollback"])
		client.rollback_max = int(args["rollback"])
	if args.has("animations"):
		client.animation_dir = args["animations"]
	autoplay = float(args.get("autoplay", "0"))
	screenshot = args.get("screenshot", "")
	auto_rng.seed = Time.get_ticks_usec()

	client.visual_spawned.connect(_on_visual_spawned)
	client.visual_destroying.connect(_on_visual_destroying)
	client.player_jumped.connect(func(_id, pos, _local): _spawn_vfx("jump", pos))
	client.player_landed.connect(func(_id, pos, _local): _spawn_vfx("land", pos))
	client.connection_state_changed.connect(func(state): print("connection: ", state))

	var hud_scene: PackedScene = load("res://ui/hud.tscn")
	if hud_scene:
		hud = hud_scene.instantiate()
		add_child(hud)

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
	_update_camera()
	_update_hud()
	_autoplay_finish()


func _send_input(delta: float) -> void:
	var move := Vector2.ZERO
	var jump := false
	var sprint := false
	var spawn := false
	if autoplay > 0.0:
		# Scripted player for unattended runs.
		if auto_rng.randf() < delta * 1.5:
			auto_move = Vector2(auto_rng.randf_range(-1, 1), auto_rng.randf_range(0.2, 1))
		move = auto_move
		sprint = true
		jump = auto_rng.randf() < delta * 1.0
		spawn = auto_rng.randf() < delta * 2.0
		yaw += delta * 0.6
	elif get_window().has_focus():
		move.x = float(Input.is_physical_key_pressed(KEY_D)) - float(Input.is_physical_key_pressed(KEY_A))
		move.y = float(Input.is_physical_key_pressed(KEY_W)) - float(Input.is_physical_key_pressed(KEY_S))
		jump = Input.is_physical_key_pressed(KEY_SPACE)
		sprint = Input.is_physical_key_pressed(KEY_SHIFT)
		spawn = Input.is_physical_key_pressed(KEY_F)
	client.set_input(move, yaw, jump, sprint, spawn)


func _update_camera() -> void:
	var target := Vector3(0, 1, 0)
	if client.has_local_player():
		target = client.get_local_player_position() + Vector3(0, 0.4, 0)
	camera.rotation = Vector3(pitch, yaw, 0)
	camera.global_position = target + camera.global_transform.basis.z * distance


func _update_hud() -> void:
	if hud == null:
		return
	var stats: Dictionary = client.get_stats()
	var label := hud.get_node_or_null("%Stats") as Label
	if label:
		label.visible = show_debug
		label.text = "%d FPS   %s\ntick %d   confirmed %d   window %d\nrtt %d ms   rollbacks %d (last %d)   stalled %.1f s\nchecksums ok %d   desyncs %d\nentities %d   animation: %s" % [
			Engine.get_frames_per_second(), stats.get("state", ""),
			stats.get("tick", 0), stats.get("confirmed_tick", 0), stats.get("rollback_window", 0),
			stats.get("rtt_ms", 0), stats.get("rollbacks", 0), stats.get("last_rollback_depth", 0), stats.get("stalled_seconds", 0.0),
			stats.get("checksums_verified", 0), stats.get("desyncs", 0),
			stats.get("entities", 0), stats.get("animation", "")]
	var banner := hud.get_node_or_null("%Banner") as Label
	if banner:
		var state: String = stats.get("state", "")
		banner.visible = state != "playing"
		if state == "rejected":
			banner.text = "Rejected: %s" % stats.get("reject_reason", "")
		elif state == "reconnecting" or (state == "joining" and stats.get("welcomes", 0) > 0):
			banner.text = "Connection lost - time is paused, reconnecting..."
		else:
			banner.text = "Connecting..."


func _autoplay_finish() -> void:
	if autoplay <= 0.0:
		return
	if playing_since < 0.0:
		if client.get_connection_state() == "playing":
			playing_since = Time.get_ticks_msec() / 1000.0
		return
	if Time.get_ticks_msec() / 1000.0 - playing_since < autoplay:
		return
	autoplay = 0.0
	if screenshot != "":
		await RenderingServer.frame_post_draw
		get_viewport().get_texture().get_image().save_png(screenshot)
	var stats: Dictionary = client.get_stats()
	print("autoplay done: %s, checksums ok %d, desyncs %d, fingerprint %s, fp ok %s" % [
		stats.get("state"), stats.get("checksums_verified"), stats.get("desyncs"), stats.get("fingerprint"), stats.get("fp_environment_ok")])
	client.disconnect_from_server()
	get_tree().quit(0 if stats.get("desyncs", 1) == 0 and stats.get("state") == "playing" else 2)


# --- VFX director -------------------------------------------------------------------------------

func _on_visual_spawned(_visual_id: int, net_id: int, kind: String, node: Node3D, position: Vector3, with_effect: bool) -> void:
	if kind == "prop" and node.has_meta("tint_by_net_id"):
		_tint(node, net_id)
	if with_effect and kind == "prop":
		_spawn_vfx("prop_spawn", position)


func _on_visual_destroying(_visual_id: int, _net_id: int, kind: String, position: Vector3) -> void:
	if kind == "prop":
		_spawn_vfx("prop_destroy", position)


func _spawn_vfx(effect: String, position: Vector3) -> void:
	if not _vfx_cache.has(effect):
		var path := "res://vfx/%s.tscn" % effect
		_vfx_cache[effect] = load(path) if ResourceLoader.exists(path) else null
	var scene: PackedScene = _vfx_cache[effect]
	if scene == null:
		return
	var node := scene.instantiate() as Node3D
	if node == null:
		return
	add_child(node)
	node.global_position = position
	for particles in node.find_children("*", "GPUParticles3D", true, false) + ([node] if node is GPUParticles3D else []):
		particles.restart()
	get_tree().create_timer(VFX_LIFETIME).timeout.connect(node.queue_free)


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
