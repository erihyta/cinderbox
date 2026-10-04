extends SceneTree
## Builds the melee mod's "carry" animation pack and bakes it into its client project:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/make_carry_pack.gd -- --out=<abs path>/server_mods/melee/client/anim/melee.carry
##
## The bat brings this pack: while a player holds one, the pack's "FullBody" layer plays instead of the
## player's own (mod_api.h, Declarations::ItemLayers): standing ready with it, a measured walk, the
## usual jog when faster.
##
## A pack is authored like a character's state machine: a model on a humanoid-profile skeleton (the
## mannequin here, CC0), its AnimationPlayer, and an AnimationTree. The tree's root is a state machine,
## so its one layer is named "FullBody", the layer it replaces. Characters on other skeletons get
## the clips fitted by the profile's bone names. It uses only the Standard (CC0) animations.

const SOURCE := "res://characters/mannequin/source/UAL1_Standard.glb"

func _initialize() -> void:
	var out := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out = arg.substr(6)
	if out == "":
		printerr("pass -- --out=<absolute folder>")
		quit(2)
		return

	var pack := CbAnimPack.new()
	pack.name = "MeleeCarry"
	pack.character_name = "melee.carry"
	var model: Node3D = (load(SOURCE) as PackedScene).instantiate()
	model.name = "Model"
	pack.add_child(model)
	var player := model.get_node("AnimationPlayer") as AnimationPlayer
	pack.skeleton_path = pack.get_path_to(model.get_node("Armature/Skeleton3D"))
	pack.animation_player_path = pack.get_path_to(player)
	# The ready pose is held as long as the bat is: a loop, though the pack ships it as a one-shot.
	player.get_animation("Sword_Idle").loop_mode = Animation.LOOP_LINEAR

	# Base: ready with the bat, walking by speed (backwards the same walk reversed), falling like anyone.
	var base := AnimationNodeStateMachine.new()
	var carry := AnimationNodeBlendSpace1D.new()
	carry.min_space = -4.0
	carry.max_space = 7.0
	for point in [[-2.0, "Walk_Formal", true], [0.0, "Sword_Idle", false], [2.0, "Walk_Formal", false], [5.0, "Jog_Fwd", false]]:
		var clip := AnimationNodeAnimation.new()
		clip.animation = point[1]
		if point[2]:
			clip.play_mode = AnimationNodeAnimation.PLAY_MODE_BACKWARD
		carry.add_blend_point(clip, point[0], -1, ("Back" if point[2] else "") + point[1])
	base.add_node("Carry", carry, Vector2(300, 100))
	var fall := AnimationNodeAnimation.new()
	fall.animation = "Jump"
	base.add_node("Fall", fall, Vector2(550, 100))
	_go(base, "Start", "Carry")
	_go(base, "Carry", "Fall", "not grounded and airborne_time > 0.12", 0.2)
	_go(base, "Fall", "Carry", "grounded", 0.15)

	var tree := AnimationTree.new()
	tree.name = "AnimationTree"
	tree.tree_root = base
	pack.add_child(tree)
	tree.root_node = NodePath("../Model")
	tree.anim_player = tree.get_path_to(player)
	pack.animation_tree_path = pack.get_path_to(tree)
	pack.graph_inputs = {"Carry/blend_position": "forward_speed"}

	var result: Dictionary = pack.bake_to(ProjectSettings.globalize_path(out) if out.begins_with("res://") else out)
	pack.free()
	if not result["ok"]:
		printerr("bake failed: ", result["error"])
		quit(1)
		return
	print("baked %d clips into %s" % [result["clips"], out])
	if result["warnings"] != "":
		print("warnings: ", result["warnings"])
	quit(0)


func _go(machine: AnimationNodeStateMachine, from: String, to: String, when := "", xfade := 0.0) -> void:
	var t := AnimationNodeStateMachineTransition.new()
	t.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	if when.contains(" "):
		t.advance_expression = when
	elif when != "":
		t.advance_condition = when
	t.xfade_time = xfade
	machine.add_transition(from, to, t)
