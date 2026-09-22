extends SceneTree
## Builds the example AnimationTree player prefab:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/make_animtree_example.gd -- --out=<path>
##
## A scene like this is normally made by hand in the editor. It is generated here so the example
## stays in step with the animation tuning in the simulation (clip lengths and blend speeds are
## simulation constants) and so a fresh clone can rebuild it. Open it in Godot and edit it freely.
##
## The character is deliberately plain boxes: it demonstrates the binding, not the art.

const WALK_CYCLE := 1.0 # cb::anim_tuning::kWalkCycleSeconds
const RUN_CYCLE := 0.7  # cb::anim_tuning::kRunCycleSeconds
const WALK_SPEED := 3.0 # cb::anim_tuning::kWalkSpeed
const RUN_SPEED := 6.5  # cb::anim_tuning::kRunSpeed

const HIP_HEIGHT := 0.9

var _root: Node3D


func _initialize() -> void:
	var out_path := "res://prefabs/player.tscn"
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out_path = arg.substr(6)

	_root = Node3D.new()
	_root.name = "Player"

	var hips := _add_node3d(_root, "Hips", Vector3(0, HIP_HEIGHT, 0))
	_add_box(hips, "Torso", Vector3(0, 0.3, 0), Vector3(0.45, 0.6, 0.25), Color(0.83, 0.52, 0.45))
	_add_box(hips, "Head", Vector3(0, 0.75, 0), Vector3(0.26, 0.26, 0.26), Color(0.93, 0.72, 0.62))
	for side in [["ArmL", 0.32], ["ArmR", -0.32]]:
		var arm := _add_node3d(hips, side[0], Vector3(side[1], 0.55, 0))
		_add_box(arm, "Mesh", Vector3(0, -0.25, 0), Vector3(0.14, 0.5, 0.14), Color(0.83, 0.52, 0.45))
	for side in [["LegL", 0.13], ["LegR", -0.13]]:
		var leg := _add_node3d(hips, side[0], Vector3(side[1], 0, 0))
		_add_box(leg, "Mesh", Vector3(0, -0.42, 0), Vector3(0.17, 0.85, 0.19), Color(0.35, 0.38, 0.52))

	var library := AnimationLibrary.new()
	library.add_animation("idle", _make_idle())
	library.add_animation("walk", _make_stride(WALK_CYCLE, 0.45, 0.35, 0.04, 0.0))
	library.add_animation("run", _make_stride(RUN_CYCLE, 0.85, 0.75, 0.09, 0.22))
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
	quit(0)


func _add_node3d(parent: Node, name: String, position: Vector3) -> Node3D:
	var node := Node3D.new()
	node.name = name
	node.position = position
	parent.add_child(node)
	node.owner = _root
	return node


func _add_box(parent: Node, name: String, position: Vector3, size: Vector3, color: Color) -> MeshInstance3D:
	var mesh := BoxMesh.new()
	mesh.size = size
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = 0.75
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


func _swing(x: float) -> Vector3:
	return Vector3(x, 0, 0)


## Walk and run are the same shape at different amplitudes, which is why one phase drives both.
func _make_stride(cycle: float, leg: float, arm: float, bob: float, lean: float) -> Animation:
	var anim := Animation.new()
	anim.length = cycle
	anim.loop_mode = Animation.LOOP_LINEAR
	var half := cycle * 0.5
	var times := [0.0, half, cycle]
	_track(anim, "Hips/LegL:rotation", times, [_swing(leg), _swing(-leg), _swing(leg)])
	_track(anim, "Hips/LegR:rotation", times, [_swing(-leg), _swing(leg), _swing(-leg)])
	_track(anim, "Hips/ArmL:rotation", times, [_swing(-arm), _swing(arm), _swing(-arm)])
	_track(anim, "Hips/ArmR:rotation", times, [_swing(arm), _swing(-arm), _swing(arm)])
	# Two bobs per cycle: one per step.
	_track(anim, "Hips:position", [0.0, cycle * 0.25, half, cycle * 0.75, cycle],
		[Vector3(0, HIP_HEIGHT, 0), Vector3(0, HIP_HEIGHT - bob, 0), Vector3(0, HIP_HEIGHT, 0),
		Vector3(0, HIP_HEIGHT - bob, 0), Vector3(0, HIP_HEIGHT, 0)])
	_track(anim, "Hips:rotation", [0.0], [Vector3(lean, 0, 0)])
	return anim


func _make_idle() -> Animation:
	var anim := Animation.new()
	anim.length = 2.4
	anim.loop_mode = Animation.LOOP_LINEAR
	_track(anim, "Hips:position", [0.0, 1.2, 2.4],
		[Vector3(0, HIP_HEIGHT, 0), Vector3(0, HIP_HEIGHT - 0.015, 0), Vector3(0, HIP_HEIGHT, 0)])
	_track(anim, "Hips:rotation", [0.0], [Vector3.ZERO])
	_track(anim, "Hips/LegL:rotation", [0.0], [Vector3.ZERO])
	_track(anim, "Hips/LegR:rotation", [0.0], [Vector3.ZERO])
	_track(anim, "Hips/ArmL:rotation", [0.0, 1.2, 2.4], [_swing(0.05), _swing(-0.03), _swing(0.05)])
	_track(anim, "Hips/ArmR:rotation", [0.0, 1.2, 2.4], [_swing(-0.03), _swing(0.05), _swing(-0.03)])
	return anim


func _make_jump() -> Animation:
	# Matches kJumpStartSeconds so the state ends when the simulation leaves the mode.
	var anim := Animation.new()
	anim.length = 0.25
	_track(anim, "Hips/ArmL:rotation", [0.0, 0.25], [_swing(0.0), _swing(-2.3)])
	_track(anim, "Hips/ArmR:rotation", [0.0, 0.25], [_swing(0.0), _swing(-2.3)])
	_track(anim, "Hips/LegL:rotation", [0.0, 0.25], [_swing(0.0), _swing(0.7)])
	_track(anim, "Hips/LegR:rotation", [0.0, 0.25], [_swing(0.0), _swing(0.5)])
	_track(anim, "Hips:position", [0.0, 0.25], [Vector3(0, HIP_HEIGHT - 0.1, 0), Vector3(0, HIP_HEIGHT, 0)])
	_track(anim, "Hips:rotation", [0.0], [Vector3.ZERO])
	return anim


func _make_fall() -> Animation:
	var anim := Animation.new()
	anim.length = 0.8
	anim.loop_mode = Animation.LOOP_LINEAR
	_track(anim, "Hips/ArmL:rotation", [0.0, 0.4, 0.8], [_swing(-1.9), _swing(-2.1), _swing(-1.9)])
	_track(anim, "Hips/ArmR:rotation", [0.0, 0.4, 0.8], [_swing(-2.1), _swing(-1.9), _swing(-2.1)])
	_track(anim, "Hips/LegL:rotation", [0.0, 0.4, 0.8], [_swing(0.25), _swing(0.4), _swing(0.25)])
	_track(anim, "Hips/LegR:rotation", [0.0, 0.4, 0.8], [_swing(-0.3), _swing(-0.15), _swing(-0.3)])
	_track(anim, "Hips:position", [0.0], [Vector3(0, HIP_HEIGHT, 0)])
	_track(anim, "Hips:rotation", [0.0], [Vector3(0.12, 0, 0)])
	return anim


func _make_land() -> Animation:
	# Matches kLandSeconds.
	var anim := Animation.new()
	anim.length = 0.3
	_track(anim, "Hips:position", [0.0, 0.1, 0.3],
		[Vector3(0, HIP_HEIGHT - 0.02, 0), Vector3(0, HIP_HEIGHT - 0.18, 0), Vector3(0, HIP_HEIGHT, 0)])
	_track(anim, "Hips:rotation", [0.0, 0.1, 0.3], [Vector3(0.1, 0, 0), Vector3(0.28, 0, 0), Vector3.ZERO])
	_track(anim, "Hips/LegL:rotation", [0.0, 0.1, 0.3], [_swing(0.3), _swing(0.55), _swing(0.0)])
	_track(anim, "Hips/LegR:rotation", [0.0, 0.1, 0.3], [_swing(-0.25), _swing(-0.5), _swing(0.0)])
	_track(anim, "Hips/ArmL:rotation", [0.0, 0.1, 0.3], [_swing(-1.2), _swing(-0.6), _swing(0.0)])
	_track(anim, "Hips/ArmR:rotation", [0.0, 0.1, 0.3], [_swing(-1.2), _swing(-0.6), _swing(0.0)])
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
