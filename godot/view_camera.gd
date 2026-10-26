extends RefCounted
## The camera: behind the player (or over a shoulder), pulled in where the map is in the way, or out
## of the character's eyes in first person; and the view model, what the player holds that floats in
## the first-person view, drawn by a camera of its own.
##
## Where the player looks (yaw, pitch, distance, first person, the shoulder) is the game's: input,
## the unattended run and this all use it.

## The camera keeps this far off the map's surfaces.
const CAMERA_RADIUS := 0.25

var game: Node
var camera: Camera3D
var camera_distance := 6.0 # after the map got in the way

## The view model: what the player holds that floats in the first-person view, and the arms on it,
## are drawn by a camera of their own over the world (the viewer puts them on a layer only this
## camera sees). It stands where the game's camera does, with the field of view the held item asks
## for, and has its own depth: what is held never passes into a wall.
var _view_model: SubViewport
var _view_model_camera: Camera3D
var _view_model_rect: TextureRect


func _init(owner: Node, game_camera: Camera3D) -> void:
	game = owner
	camera = game_camera


## Which camera the player looks through, as the server is told (ViewMode): 0 behind, 1 first
## person, 2 / 3 over the right / left shoulder. The server starts the line of sight where this
## camera's does, so what is under the crosshair is what a shot is aimed at.
func view() -> int:
	if game.first_person and game.client.has_first_person():
		return 1
	return 2 if game.shoulder > 0 else (3 if game.shoulder < 0 else 0)


## First person: from the character's eye height above its feet, on its mover: steps, landings and
## the bowing body do not move the camera. Of your own body the arms and what is below the hips
## are drawn (CinderboxClient.first_person). False while there is no living body to look out of.
func _update_first_person() -> bool:
	var client = game.client
	var me: int = client.get_local_net_id()
	if not game.sees_first_person() or client.get_entity_node(me) == null:
		return false
	camera.rotation = Vector3(game.pitch, game.yaw, 0)
	camera.global_position = client.get_view_position(1, camera.global_transform.basis)
	return true


## Placed just before the frame is drawn (RenderingServer.frame_pre_draw).
func update() -> void:
	if game._leaving or not game.is_inside_tree():
		return
	var shake: float = game.presentation.shake
	camera.near = 0.03 if game.sees_first_person() else 0.05
	if _update_first_person():
		if shake > 0.0:
			camera.global_position += Vector3(
				game.auto.rng.randf_range(-shake, shake),
				game.auto.rng.randf_range(-shake, shake),
				game.auto.rng.randf_range(-shake, shake)) * 0.3
		_camera_moved()
		# The view model (the arms, and what they hold) goes to the camera where it is now: it is
		# stitched to it. What floats in the view (an item with a view camera) goes in front of it.
		game.client.place_view_items(camera.global_transform, game.get_process_delta_time())
		game.auto.probe_sample()
		_show_view_model(game.client.get_view_fov())
		return
	_show_view_model(0.0)
	# The point above the player the camera orbits (moved to a shoulder, if it is), or its ragdoll
	# while dead. The server's line of sight passes through the same point.
	camera.rotation = Vector3(game.pitch, game.yaw, 0)
	var target: Vector3 = game.client.get_view_position(view(), camera.global_transform.basis)
	# The map pulls the camera in at once, and it eases back out when the way is clear.
	var back: Vector3 = camera.global_transform.basis.z
	# (As far back as the body is big: a crate is seen from closer, a giant from further.)
	var free: float = game.client.get_camera_distance(target, back, game.distance * game.client.get_view_scale(), CAMERA_RADIUS)
	camera_distance = free if free < camera_distance else minf(free, camera_distance + 12.0 * game.get_process_delta_time())
	camera.global_position = target + back * camera_distance
	if shake > 0.0:
		camera.global_position += Vector3(
			game.auto.rng.randf_range(-shake, shake),
			game.auto.rng.randf_range(-shake, shake),
			game.auto.rng.randf_range(-shake, shake))
	_camera_moved()


func make_view_model() -> void:
	var layer: int = game.client.get_view_layer()
	camera.cull_mask &= ~layer
	_view_model = SubViewport.new()
	_view_model.name = "ViewModel"
	_view_model.transparent_bg = true
	_view_model.msaa_3d = game.get_viewport().msaa_3d
	_view_model.render_target_update_mode = SubViewport.UPDATE_DISABLED
	_view_model_camera = Camera3D.new()
	_view_model_camera.cull_mask = layer
	_view_model_camera.near = 0.01
	_view_model_camera.far = 20.0
	# The world's light without the world's picture: the sky lights what is held and is not drawn.
	var world := (game.get_node("WorldEnvironment") as WorldEnvironment).environment
	var env := Environment.new()
	env.background_mode = Environment.BG_CLEAR_COLOR
	env.sky = world.sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.reflected_light_source = Environment.REFLECTION_SOURCE_SKY
	env.tonemap_mode = world.tonemap_mode
	_view_model_camera.environment = env
	_view_model.add_child(_view_model_camera)
	game.add_child(_view_model)
	# Under every HUD: over the world and nothing else.
	var canvas := CanvasLayer.new()
	canvas.layer = -1
	_view_model_rect = TextureRect.new()
	_view_model_rect.texture = _view_model.get_texture()
	_view_model_rect.set_anchors_preset(Control.PRESET_FULL_RECT)
	_view_model_rect.stretch_mode = TextureRect.STRETCH_SCALE
	_view_model_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_view_model_rect.visible = false
	canvas.add_child(_view_model_rect)
	game.add_child(canvas)


## `fov` 0: nothing floats in the view, and nothing is drawn for it.
func _show_view_model(fov: float) -> void:
	var on := fov > 0.0
	if _view_model_rect.visible != on:
		_view_model_rect.visible = on
		_view_model.render_target_update_mode = SubViewport.UPDATE_ALWAYS if on else SubViewport.UPDATE_DISABLED
	if not on:
		return
	var size: Vector2i = game.get_viewport().get_visible_rect().size
	if _view_model.size != size:
		_view_model.size = size
	_view_model_camera.fov = fov
	_view_model_camera.global_transform = camera.global_transform
	_view_model_camera.force_update_transform()


## The camera is placed just before the frame is drawn (so it has this frame's newest pose), which
## is after Godot has passed this frame's moved nodes on to the renderer: left at that, the frame
## would be drawn from where the camera was a frame ago, while the player's body is drawn where it
## is now. In first person that makes the arms shake against the view with every step, by the
## distance walked in a frame. So the camera's move is passed on at once.
func _camera_moved() -> void:
	camera.force_update_transform()
