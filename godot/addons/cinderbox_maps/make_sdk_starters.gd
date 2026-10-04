extends SceneTree
## Builds the SDK's starter animation packs, on its placeholder character (run in the SDK project,
## after tools\sdk.ps1 -Setup has put the extension and the mannequin there):
##
##   godot --headless --path sdk --script <repo>/godot/addons/cinderbox_maps/make_sdk_starters.gd
##
##   starters/anim_base.tscn    a pack for "Base": the mannequin's own locomotion (idle, walk, jog,
##                              sprint by forward speed; jump, fall, land), to change into a carry,
##                              a limp, a crouch
##   starters/anim_upper.tscn   a pack for "Upper": one hold pose and a use on "MODNAME.used". It has
##                              no bone filter: it plays through the character's own Upper filter
##
## tools\sdk.ps1 -New <mod> copies them into the mod's project as anim_src/, named after the mod.
## Nothing is baked here: a pack bakes when its scene is saved in the editor.

const MODEL := "res://characters/mannequin/source/UAL1_Standard.glb"
const LOCOMOTION := [[-3.0, "Jog_Fwd", true], [-0.9, "Walk", true], [0.0, "Idle", false], [0.9, "Walk", false], [3.0, "Jog_Fwd", false], [5.0, "Sprint", false]]


func _initialize() -> void:
	var failed := not _save(_pack("MODNAME.walk", _base(), {"Locomotion/blend_position": "forward_speed"}), "res://starters/anim_base.tscn")
	failed = not _save(_pack("MODNAME.hold", _upper(), {}), "res://starters/anim_upper.tscn") or failed
	quit(1 if failed else 0)


func _pack(pack_name: String, root: AnimationRootNode, inputs: Dictionary) -> CbAnimPack:
	var pack := CbAnimPack.new()
	pack.name = "Pack"
	pack.character_name = pack_name
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


# Base: the root is the state machine itself, so the layer is "Base".
func _base() -> AnimationRootNode:
	var base := AnimationNodeStateMachine.new()
	var line := AnimationNodeBlendSpace1D.new()
	line.min_space = -4.0
	line.max_space = 6.0
	for point in LOCOMOTION:
		line.add_blend_point(_clip(point[1], point[2]), point[0], -1, ("Back" if point[2] else "") + point[1])
	base.add_node("Locomotion", line, Vector2(300, 100))
	base.add_node("JumpStart", _clip("Jump_Start"), Vector2(550, 0))
	base.add_node("Fall", _clip("Jump"), Vector2(800, 100))
	base.add_node("Land", _clip("Jump_Land"), Vector2(550, 220))
	_go(base, "Start", "Locomotion")
	_go(base, "Locomotion", "JumpStart", "jumped", 0.1)
	_go(base, "Locomotion", "Fall", "not grounded and airborne_time > 0.12", 0.2)
	_go(base, "JumpStart", "Land", "grounded and state_time > 0.1", 0.1)
	_go(base, "JumpStart", "Fall", "state_time > 0.3", 0.2)
	_go(base, "Fall", "Land", "grounded", 0.1)
	_go(base, "Land", "JumpStart", "jumped", 0.1)
	_go(base, "Land", "Locomotion", "state_time > 0.35 or speed > 1.5", 0.25)
	return base


# Upper: a blend tree with the one state machine on its output, so the layer is named as the node is.
func _upper() -> AnimationRootNode:
	var upper := AnimationNodeStateMachine.new()
	upper.add_node("Hold", _clip("Pistol_Idle"), Vector2(300, 100))
	upper.add_node("Use", _clip("Pistol_Shoot"), Vector2(550, 100))
	_go(upper, "Start", "Hold")
	_go(upper, "Hold", "Use", "MODNAME.used", 0.05)
	_go(upper, "Use", "Hold", "", 0.15, true)
	var root := AnimationNodeBlendTree.new()
	root.add_node("Upper", upper, Vector2(200, 100))
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
