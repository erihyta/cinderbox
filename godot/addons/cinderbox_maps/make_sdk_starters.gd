extends SceneTree
## Builds the SDK's two starter animation packs on its placeholder model (run in the SDK project,
## after tools\sdk.ps1 -Setup):
##
##   godot --headless --path sdk --script <repo>/godot/addons/cinderbox_maps/make_sdk_starters.gd
##
##   starters/animation_pack_locomotion.tscn   replaces a character's "Base" layer: the placeholder's
##                                             own locomotion (Move by forward speed, JumpStart,
##                                             InAir, Land), to change into a carry, a limp, a crouch
##   starters/animation_pack_upper_body.tscn   replaces a character's "Upper" layer: one state, Hold
##
## They use only the placeholder's clips. tools\sdk.ps1 -New <mod> copies them into the mod's
## project (animation_packs/), named after the mod. Nothing is baked here: a pack bakes when its
## scene is saved in the editor, and when the mod is published.

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


func _initialize() -> void:
	var locomotion := _pack("LocomotionPack", "MODNAME.locomotion", _locomotion(), {"Move/blend_position": "forward_speed"},
		"An animation pack for the whole body.\n\n"
		+ "Its state machine replaces a character's \"Base\" layer while the server mod says so "
		+ "(ItemLayers: while an item is held; or SwapLayer). The tree's root is the state machine itself: that is what makes it \"Base\".\n\n"
		+ "Move blends the clips by forward speed (Graph Inputs on this node: Move/blend_position = forward_speed). "
		+ "Transitions use the game's names: grounded, jumped, airborne_time, state_time, speed.\n\n"
		+ "Saving this scene bakes it into res://anim/MODNAME.locomotion/.")
	var upper := _pack("UpperBodyPack", "MODNAME.upper_body", _upper_body(), {},
		"An animation pack for the upper body.\n\n"
		+ "Its state machine replaces a character's \"Upper\" layer while the server mod says so "
		+ "(ItemLayers: while an item is held; or SwapLayer). In the tree, the state machine node is named Upper: that name is what it replaces. "
		+ "It has no bone filter: it moves the bones the character's own Upper layer moves (spine, arms, head).\n\n"
		+ "Hold plays the placeholder's Idle: put your own clip there. For a use (a shot, a swing), add a state and a transition "
		+ "whose condition is the server mod's event (MODNAME.used), and one back with Switch Mode At End.\n\n"
		+ "Saving this scene bakes it into res://anim/MODNAME.upper_body/.")
	var failed := not _save(locomotion, "res://starters/animation_pack_locomotion.tscn")
	failed = not _save(upper, "res://starters/animation_pack_upper_body.tscn") or failed
	quit(1 if failed else 0)


func _pack(node_name: String, pack_name: String, root: AnimationRootNode, inputs: Dictionary, about: String) -> CbAnimPack:
	var pack := CbAnimPack.new()
	pack.name = node_name
	pack.character_name = pack_name
	pack.editor_description = about
	var model: Node3D = (load(MODEL) as PackedScene).instantiate()
	model.name = "Model"
	pack.add_child(model)
	model.owner = pack
	var player := model.get_node("AnimationPlayer") as AnimationPlayer
	pack.skeleton_path = pack.get_path_to(model.get_node("Armature/Skeleton3D"))
	pack.animation_player_path = pack.get_path_to(player)
	var tree := AnimationTree.new()
	tree.name = "AnimationTree"
	tree.tree_root = root
	pack.add_child(tree)
	tree.owner = pack
	tree.root_node = NodePath("../Model")
	tree.anim_player = tree.get_path_to(player)
	tree.active = false
	pack.animation_tree_path = pack.get_path_to(tree)
	pack.graph_inputs = inputs
	return pack


func _save(pack: CbAnimPack, path: String) -> bool:
	var packed := PackedScene.new()
	var ok := packed.pack(pack) == OK and ResourceSaver.save(packed, path) == OK
	print("%s: %s" % [path, "saved" if ok else "NOT saved"])
	pack.free()
	return ok


func _locomotion() -> AnimationRootNode:
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


# A blend tree with the one state machine on its output: the layer is named as that node is.
func _upper_body() -> AnimationRootNode:
	var machine := AnimationNodeStateMachine.new()
	machine.add_node("Hold", _clip("Idle"), Vector2(300, 100))
	_go(machine, "Start", "Hold")
	var root := AnimationNodeBlendTree.new()
	root.add_node("Upper", machine, Vector2(200, 100))
	root.connect_node("output", 0, "Upper")
	return root


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
