extends SceneTree
## Builds the example AnimationTree player prefab:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/make_animtree_example.gd -- --out=<path>
##
## A scene like this is normally made by hand in the editor. It is generated here so the example
## stays in step with the animation tuning in the simulation (clip lengths and blend speeds are
## simulation constants) and so a fresh clone can rebuild it. Open it in Godot and edit it freely.
##
## The body is the ozz pose (drawn as bone boxes by a CinderboxSkeleton): the same pose the server
## poses hitboxes with, so nothing Godot animates may move it. The AnimationTree animates what is
## cosmetic only, here a jetpack whose flames follow the same state machine (idle flicker, walk and
## run by speed, a burst on jump). That is the rule for players: Godot animation adds, ozz places.

const WALK_CYCLE := 1.0 # cb::anim_tuning::kWalkCycleSeconds
const RUN_CYCLE := 0.7  # cb::anim_tuning::kRunCycleSeconds
const WALK_SPEED := 3.0 # cb::anim_tuning::kWalkSpeed
const RUN_SPEED := 6.5  # cb::anim_tuning::kRunSpeed

const FLAMES := ["Jetpack/FlameL", "Jetpack/FlameR"]

var _root: Node3D


func _initialize() -> void:
	var out_path := "res://prefabs/player.tscn"
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out_path = arg.substr(6)

	_root = Node3D.new()
	_root.name = "Player"

	# The body: the ozz pose, placed and aimed by the simulation.
	var body := CinderboxSkeleton.new()
	body.name = "Body"
	_root.add_child(body)
	body.owner = _root

	# The cosmetic part the tree animates: a jetpack on the back (the character faces +Z).
	var pack := _add_node3d(_root, "Jetpack", Vector3(0, 1.22, -0.2))
	_add_box(pack, "Case", Vector3.ZERO, Vector3(0.3, 0.36, 0.14), Color(0.35, 0.38, 0.45), false)
	for side in [["FlameL", 0.08], ["FlameR", -0.08]]:
		var flame := _add_node3d(pack, side[0], Vector3(side[1], -0.18, 0))
		_add_box(flame, "Mesh", Vector3(0, -0.1, 0), Vector3(0.07, 0.2, 0.07), Color(1.0, 0.55, 0.15), true)

	var library := AnimationLibrary.new()
	library.add_animation("idle", _make_idle())
	library.add_animation("walk", _make_stride(WALK_CYCLE, 0.5))
	library.add_animation("run", _make_stride(RUN_CYCLE, 1.0))
	library.add_animation("jump", _make_jump())
	library.add_animation("fall", _make_fall())
	library.add_animation("land", _make_land())

	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	player.root_node = NodePath("..")
	player.add_animation_library("", library)
	_root.add_child(player)
	player.owner = _root

	var tree := AnimationTree.new()
	tree.name = "AnimationTree"
	tree.anim_player = NodePath("../AnimationPlayer")
	tree.tree_root = _make_state_machine()
	tree.active = true
	_root.add_child(tree)
	tree.owner = _root

	# The node that ties it to the simulation: the tree's state and blend come from the game.
	var animator := CinderboxAnimator.new()
	animator.name = "Animator"
	animator.animation_tree_path = NodePath("../AnimationTree")
	animator.speed_parameter = "parameters/locomotion/blend_position"
	_root.add_child(animator)
	animator.owner = _root

	var packed := PackedScene.new()
	if packed.pack(_root) != OK:
		printerr("cannot pack the prefab")
		quit(2)
		return
	var err := ResourceSaver.save(packed, out_path)
	if err != OK:
		printerr("cannot save ", out_path, ": ", err)
		quit(2)
		return
	print("wrote ", out_path)
	_root.free()
	quit(0)


func _add_node3d(parent: Node, name: String, position: Vector3) -> Node3D:
	var node := Node3D.new()
	node.name = name
	node.position = position
	parent.add_child(node)
	node.owner = _root
	return node


func _add_box(parent: Node, name: String, position: Vector3, size: Vector3, color: Color, glow: bool) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = 0.75
	if glow:
		material.emission_enabled = true
		material.emission = color
		material.emission_energy_multiplier = 2.0
	mesh.material = material
	var node := MeshInstance3D.new()
	node.name = name
	node.position = position
	node.mesh = mesh
	parent.add_child(node)
	node.owner = _root
	return node


func _track(anim: Animation, path: String, times: Array, values: Array) -> void:
	var index := anim.add_track(Animation.TYPE_VALUE)
	anim.track_set_path(index, NodePath(path))
	anim.track_set_interpolation_type(index, Animation.INTERPOLATION_LINEAR)
	for i in times.size():
		anim.track_insert_key(index, times[i], values[i])


## Every clip only sets the flames' length (their Y scale).
func _flames(anim: Animation, times: Array, lengths: Array) -> void:
	var values := []
	for length in lengths:
		values.append(Vector3(1, length, 1))
	for flame in FLAMES:
		_track(anim, flame + ":scale", times, values)


func _loop(length: float) -> Animation:
	var anim := Animation.new()
	anim.length = length
	anim.loop_mode = Animation.LOOP_LINEAR
	return anim


## Walk and run flicker at their cycle, harder the faster.
func _make_stride(cycle: float, strength: float) -> Animation:
	var anim := _loop(cycle)
	_flames(anim, [0.0, cycle * 0.5, cycle], [strength, strength * 1.3, strength])
	return anim


func _make_idle() -> Animation:
	var anim := _loop(2.4)
	_flames(anim, [0.0, 1.2, 2.4], [0.15, 0.25, 0.15])
	return anim


func _make_jump() -> Animation:
	# Matches kJumpStartSeconds so the state ends when the simulation leaves the mode.
	var anim := Animation.new()
	anim.length = 0.25
	_flames(anim, [0.0, 0.25], [0.5, 2.4])
	return anim


func _make_fall() -> Animation:
	var anim := _loop(0.8)
	_flames(anim, [0.0, 0.4, 0.8], [1.3, 1.6, 1.3])
	return anim


func _make_land() -> Animation:
	# Matches kLandSeconds.
	var anim := Animation.new()
	anim.length = 0.3
	_flames(anim, [0.0, 0.1, 0.3], [1.6, 0.4, 0.2])
	return anim


func _clip(name: String) -> AnimationNodeAnimation:
	var node := AnimationNodeAnimation.new()
	node.animation = name
	return node


func _make_state_machine() -> AnimationNodeStateMachine:
	# Locomotion is a 1D blend over the simulation's ground speed, in metres per second, so idle,
	# walk and run sit exactly where the simulation's blend points are.
	var locomotion := AnimationNodeBlendSpace1D.new()
	locomotion.min_space = 0.0
	locomotion.max_space = RUN_SPEED + 0.5
	locomotion.add_blend_point(_clip("idle"), 0.0)
	locomotion.add_blend_point(_clip("walk"), WALK_SPEED)
	locomotion.add_blend_point(_clip("run"), RUN_SPEED)

	var machine := AnimationNodeStateMachine.new()
	machine.add_node("locomotion", locomotion, Vector2(340, 100))
	machine.add_node("jump", _clip("jump"), Vector2(560, 40))
	machine.add_node("fall", _clip("fall"), Vector2(560, 160))
	machine.add_node("land", _clip("land"), Vector2(340, 240))

	var states := ["locomotion", "jump", "fall", "land"]
	machine.add_transition("Start", "locomotion", _transition(0.0))
	for from in states:
		for to in states:
			if from != to:
				machine.add_transition(from, to, _transition(0.12))
	return machine


func _transition(xfade: float) -> AnimationNodeStateMachineTransition:
	var transition := AnimationNodeStateMachineTransition.new()
	# The simulation decides when to switch, so transitions never wait for a clip to end.
	transition.switch_mode = AnimationNodeStateMachineTransition.SWITCH_MODE_IMMEDIATE
	transition.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_DISABLED
	transition.xfade_time = xfade
	return transition
