extends SceneTree
## Builds the SDK's starter animation pack on its placeholder model (run in the SDK project, after
## tools\sdk.ps1 -Setup):
##
##   godot --headless --path sdk --script <repo>/godot/addons/cinderbox_maps/make_sdk_starters.gd
##
## starters/animation_pack.tscn holds the default AnimationTree in full, as a character has it:
##
##   FullBody         the whole body: Move (idle, walk, jog, sprint by forward speed), JumpStart,
##                    InAir, Land
##   UpperBody        over it, on the spine, arms and head: Hold
##   UpperBodyBlend   the Blend2 that lays UpperBody over FullBody, with its bone filter
##
## and replaces only "UpperBody" (the root's Replaces list): the full body is there to see the
## upper body over it in the editor, and is not baked. Add "FullBody" to the list to replace the
## locomotion as well. It uses only the placeholder's clips.
##
## tools\sdk.ps1 -New <mod> copies it into the mod's project (animation_packs/), named after the
## mod. Nothing is baked here: a pack bakes when its scene is saved in the editor, and when the mod
## is published.

const MODEL := "res://placeholder/mannequin.glb"
# Along forward speed (m/s): which clip, played backward or not, and the point's name in the editor.
const MOVE := [
	[-3.0, "Jog_Fwd", true, "Jog backward"],
	[-0.9, "Walk", true, "Walk backward"],
	[0.0, "Idle", false, "Idle"],
	[0.9, "Walk", false, "Walk"],
	[3.0, "Jog_Fwd", false, "Jog"],
	[5.0, "Sprint", false, "Sprint"],
]
const ABOUT := "An animation pack: state machines that replace a character's own while the server mod says so " \
	+ "(ItemLayers: while an item is held; or SwapLayer).\n\n" \
	+ "The AnimationTree is the default one in full: FullBody (the whole body's locomotion), UpperBody over it " \
	+ "(spine, arms, head), and UpperBodyBlend, which lays one over the other and has the bone filter.\n\n" \
	+ "Replaces lists the layers this pack ships. It is UpperBody: FullBody is here to see your upper body over a " \
	+ "walking character, and is not baked. Add FullBody to replace the locomotion too.\n\n" \
	+ "UpperBody's Hold plays the placeholder's Idle: put your own clip there. For a use (a shot, a swing), add a state " \
	+ "and a transition whose condition is the server mod's event (MODNAME.used), and one back with Switch Mode At End.\n\n" \
	+ "Transitions use the game's names: forward_speed, speed, grounded, jumped, airborne_time, state_time, a stance, an event. " \
	+ "Move's position comes from Graph Inputs on this node.\n\n" \
	+ "Saving this scene bakes it into res://anim/MODNAME.animations/."


func _initialize() -> void:
	var pack := CbAnimPack.new()
	pack.name = "AnimationPack"
	pack.character_name = "MODNAME.animations"
	pack.editor_description = ABOUT
	pack.replaces = PackedStringArray(["UpperBody"])
	var model: Node3D = (load(MODEL) as PackedScene).instantiate()
	model.name = "Model"
	pack.add_child(model)
	model.owner = pack
	var player := model.get_node("AnimationPlayer") as AnimationPlayer
	var skeleton := model.get_node("Armature/Skeleton3D") as Skeleton3D
	pack.skeleton_path = pack.get_path_to(skeleton)
	pack.animation_player_path = pack.get_path_to(player)

	# The upper body: the spine and everything above it, as the clips' tracks name those bones.
	var track := String(player.get_animation("Idle").track_get_path(0))
	var to_skeleton := track.substr(0, track.find(":"))
	var blend := AnimationNodeBlend2.new()
	blend.filter_enabled = true
	var spine := skeleton.find_bone("Spine")
	for bone in range(skeleton.get_bone_count()):
		var b := bone
		while b >= 0 and b != spine:
			b = skeleton.get_bone_parent(b)
		if b == spine:
			blend.set_filter_path(NodePath(to_skeleton + ":" + skeleton.get_bone_name(bone)), true)

	var root := AnimationNodeBlendTree.new()
	root.add_node("FullBody", _full_body(), Vector2(0, 0))
	root.add_node("UpperBody", _upper_body(), Vector2(0, 200))
	root.add_node("UpperBodyBlend", blend, Vector2(250, 100))
	root.connect_node("UpperBodyBlend", 0, "FullBody")
	root.connect_node("UpperBodyBlend", 1, "UpperBody")
	root.connect_node("output", 0, "UpperBodyBlend")

	var tree := AnimationTree.new()
	tree.name = "AnimationTree"
	tree.tree_root = root
	pack.add_child(tree)
	tree.owner = pack
	tree.root_node = NodePath("../Model")
	tree.anim_player = tree.get_path_to(player)
	tree.active = false
	# In the editor the upper body shows in full; in the game it plays while the pack is swapped in.
	tree.set("parameters/UpperBodyBlend/blend_amount", 1.0)
	pack.animation_tree_path = pack.get_path_to(tree)
	pack.graph_inputs = {"FullBody/Move/blend_position": "forward_speed"}

	var packed := PackedScene.new()
	var ok := packed.pack(pack) == OK and ResourceSaver.save(packed, "res://starters/animation_pack.tscn") == OK
	print("res://starters/animation_pack.tscn: %s" % ("saved" if ok else "NOT saved"))
	pack.free()
	quit(0 if ok else 1)


func _full_body() -> AnimationNodeStateMachine:
	var machine := AnimationNodeStateMachine.new()
	var move := AnimationNodeBlendSpace1D.new()
	move.min_space = -4.0
	move.max_space = 6.0
	move.value_label = "forward speed"
	for point in MOVE:
		move.add_blend_point(_clip(point[1], point[2]), point[0], -1, point[3])
	machine.add_node("Move", move, Vector2(300, 100))
	machine.add_node("JumpStart", _clip("Jump_Start"), Vector2(550, 0))
	machine.add_node("InAir", _clip("Jump"), Vector2(800, 100))
	machine.add_node("Land", _clip("Jump_Land"), Vector2(550, 220))
	_go(machine, "Start", "Move")
	_go(machine, "Move", "JumpStart", "jumped", 0.1)
	_go(machine, "Move", "InAir", "not grounded and airborne_time > 0.12", 0.2)
	_go(machine, "JumpStart", "Land", "grounded and state_time > 0.1", 0.1)
	_go(machine, "JumpStart", "InAir", "state_time > 0.3", 0.2)
	_go(machine, "InAir", "Land", "grounded", 0.1)
	_go(machine, "Land", "JumpStart", "jumped", 0.1)
	_go(machine, "Land", "Move", "state_time > 0.35 or speed > 1.5", 0.25)
	return machine


func _upper_body() -> AnimationNodeStateMachine:
	var machine := AnimationNodeStateMachine.new()
	machine.add_node("Hold", _clip("Idle"), Vector2(300, 100))
	_go(machine, "Start", "Hold")
	return machine


func _clip(animation: String, backward := false) -> AnimationNodeAnimation:
	var node := AnimationNodeAnimation.new()
	node.animation = animation
	if backward:
		node.play_mode = AnimationNodeAnimation.PLAY_MODE_BACKWARD
	return node


func _go(machine: AnimationNodeStateMachine, from: String, to: String, when := "", xfade := 0.0, at_end := false) -> void:
	var t := AnimationNodeStateMachineTransition.new()
	t.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	if when.contains(" "):
		t.advance_expression = when
	elif when != "":
		t.advance_condition = when
	t.xfade_time = xfade
	if at_end:
		t.switch_mode = AnimationNodeStateMachineTransition.SWITCH_MODE_AT_END
	machine.add_transition(from, to, t)
