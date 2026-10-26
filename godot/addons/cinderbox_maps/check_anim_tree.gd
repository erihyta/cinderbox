extends SceneTree
## Checks what more of Godot's AnimationTree bakes to (ROADMAP.md step 11), on the game's mannequin
## with a tree of its own (nothing of it is changed: the bake goes to a folder under user://):
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_anim_tree.gd
##
## A state that is a blend tree of an animation through a TimeScale node bakes to a "speed" line (its
## scale, or the graph_inputs expression for it); a stretched custom timeline to its speed; a
## transition's crossfade curve to its samples. Exit code 0 when everything held.

var _failures := 0


func _check(what: String, ok: bool, detail: String = "") -> void:
	print("  %-66s %s  %s" % [what, "ok  " if ok else "FAIL", detail])
	if not ok:
		_failures += 1


func _graph(folder: String) -> String:
	return FileAccess.get_file_as_string(folder + "graph.cfg")


## The line of the graph after the state's own, or "".
func _after_state(graph: String, state: String) -> String:
	var lines := graph.split("\n")
	for i in lines.size() - 1:
		if lines[i].begins_with("state\t" + state + "\t"):
			return lines[i + 1]
	return ""


func _line_starting(graph: String, start: String) -> String:
	for line in graph.split("\n"):
		if line.begins_with(start):
			return line
	return ""


func _initialize() -> void:
	var scene: PackedScene = load("res://characters/mannequin/character.tscn")
	var character: CbCharacter = scene.instantiate()
	root.add_child(character)
	var folder := "user://check_anim_tree/"
	var tree: AnimationTree = character.find_children("*", "AnimationTree", true, false)[0]
	var player: AnimationPlayer = tree.get_node(tree.anim_player)
	var names := player.get_animation_list()
	var first := String(names[0])
	var second := String(names[1])
	var length := player.get_animation(second).length

	# Idle: first, through a TimeScale node at 0.5. Swing: second, its custom timeline stretched to
	# twice its length. Fast: first through a TimeScale the graph_inputs drive.
	var machine := AnimationNodeStateMachine.new()
	var idle := AnimationNodeBlendTree.new()
	var clip := AnimationNodeAnimation.new()
	clip.animation = first
	idle.add_node("Clip", clip)
	idle.add_node("Slow", AnimationNodeTimeScale.new())
	idle.connect_node("Slow", 0, "Clip")
	idle.connect_node("output", 0, "Slow")
	machine.add_node("Idle", idle)
	var swing := AnimationNodeAnimation.new()
	swing.animation = second
	swing.use_custom_timeline = true
	swing.timeline_length = length * 2.0
	swing.stretch_time_scale = true
	machine.add_node("Swing", swing)
	var fast := AnimationNodeBlendTree.new()
	var clip2 := AnimationNodeAnimation.new()
	clip2.animation = first
	fast.add_node("Clip", clip2)
	fast.add_node("Scale", AnimationNodeTimeScale.new())
	fast.connect_node("Scale", 0, "Clip")
	fast.connect_node("output", 0, "Scale")
	machine.add_node("Fast", fast)
	var into := AnimationNodeStateMachineTransition.new()
	into.xfade_time = 0.2
	into.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	into.advance_expression = "jumped"
	var curve := Curve.new()
	curve.add_point(Vector2(0, 0))
	curve.add_point(Vector2(1, 1))
	into.xfade_curve = curve
	machine.add_transition("Idle", "Swing", into)
	var back := AnimationNodeStateMachineTransition.new()
	back.advance_expression = "speed > 1"
	back.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	machine.add_transition("Idle", "Fast", back)
	var start := AnimationNodeStateMachineTransition.new()
	machine.add_transition("Start", "Idle", start)
	tree.tree_root = machine
	tree.set("parameters/Idle/Slow/scale", 0.5)
	character.graph_inputs = {"Fast/Scale/scale": "speed * 0.5"}

	var baked: Dictionary = character.bake_to(folder)
	_check("the tree bakes", baked.ok, str(baked.get("error", "")))
	var graph := _graph(folder)
	_check("a TimeScale's scale is the state's speed", _after_state(graph, "Idle") == "speed\t0.5", _after_state(graph, "Idle"))
	_check("graph_inputs drive a TimeScale: an expression", _after_state(graph, "Fast") == "speed\tspeed * 0.5", _after_state(graph, "Fast"))
	_check("a stretched custom timeline twice as long: half speed", _after_state(graph, "Swing") == "speed\t0.5", _after_state(graph, "Swing"))
	var transition := _line_starting(graph, "transition\tIdle\tSwing\t")
	var samples := transition.split("\t")
	var points := samples[samples.size() - 1].split(",") if samples.size() == 9 else PackedStringArray()
	_check("a crossfade curve: 17 samples on its transition", points.size() == 17, transition)
	_check("... from 0 to 1 (an ease here: the default tangents)", points.size() == 17 and float(points[0]) == 0.0 and float(points[16]) == 1.0 and
		absf(float(points[8]) - 0.5) < 0.01, transition)
	var plain := _line_starting(graph, "transition\tIdle\tFast\t")
	_check("no curve: no samples", plain.split("\t").size() == 8, plain)

	# What the bake refuses: a blend tree of something else.
	var odd := AnimationNodeBlendTree.new()
	odd.add_node("Mix", AnimationNodeBlend2.new())
	odd.connect_node("output", 0, "Mix")
	machine.add_node("Odd", odd)
	baked = character.bake_to(folder)
	_check("a state's blend tree of a Blend2 is refused, and says why", not baked.ok and String(baked.error).contains("TimeScale"), str(baked.get("error", "")))
	machine.remove_node("Odd")

	# OneShot and Add2 (M231): a root blend tree of the machine, a OneShot of the second animation
	# through a TimeScale over it, and an added machine over both.
	var root_tree := AnimationNodeBlendTree.new()
	root_tree.add_node("Base", machine)
	var kick := AnimationNodeOneShot.new()
	kick.fadein_time = 0.1
	kick.fadeout_time = 0.2
	root_tree.add_node("Kick", kick)
	var kick_clip := AnimationNodeAnimation.new()
	kick_clip.animation = second
	root_tree.add_node("KickClip", kick_clip)
	root_tree.add_node("KickSpeed", AnimationNodeTimeScale.new())
	var lean_machine := AnimationNodeStateMachine.new()
	var lean_clip := AnimationNodeAnimation.new()
	lean_clip.animation = first
	lean_machine.add_node("Lean", lean_clip)
	lean_machine.add_transition("Start", "Lean", AnimationNodeStateMachineTransition.new())
	root_tree.add_node("LeanMachine", lean_machine)
	var lean := AnimationNodeAdd2.new()
	root_tree.add_node("Lean", lean)
	root_tree.connect_node("Kick", 0, "Base")
	root_tree.connect_node("KickSpeed", 0, "KickClip")
	root_tree.connect_node("Kick", 1, "KickSpeed")
	root_tree.connect_node("Lean", 0, "Kick")
	root_tree.connect_node("Lean", 1, "LeanMachine")
	root_tree.connect_node("output", 0, "Lean")
	tree.tree_root = root_tree
	tree.set("parameters/KickSpeed/scale", 2.0)
	character.graph_inputs = {"Base/Fast/Scale/scale": "speed * 0.5", "Kick/request": "jumped", "Lean/add_amount": "speed > 2"}
	baked = character.bake_to(folder)
	_check("a OneShot and an Add2 bake", baked.ok, str(baked.get("error", "")))
	graph = _graph(folder)
	var layer_lines := PackedStringArray()
	for line in graph.split("\n"):
		if line.begins_with("layer\t"):
			layer_lines.append(line.substr(6))
	_check("... as layers in the tree's order", layer_lines == PackedStringArray(["Base", "Kick", "LeanMachine"]), str(layer_lines))
	_check("the OneShot: its fades and its request", _line_starting(graph, "shot\t") == "shot\t0.1\t0.2\tjumped", _line_starting(graph, "shot\t"))
	_check("... its one state, at its TimeScale's speed", graph.contains("state\tShot\tclip\t%s\t0\nspeed\t2.0\nstart\tShot\n" % second) or
		graph.contains("state\tShot\tclip\t%s\t0\nspeed\t2\nstart\tShot\n" % second), graph.substr(graph.find("shot\t"), 120))
	_check("the Add2: an added layer weighed by its amount", graph.contains("layer\tLeanMachine\nweight\tspeed > 2\nadd\n"),
		graph.substr(graph.find("layer\tLeanMachine"), 60))
	_check("the machine below them keeps its own paths", _after_state(graph, "Fast") == "speed\tspeed * 0.5", _after_state(graph, "Fast"))
	character.graph_inputs = {"Lean/add_amount": "1"}
	baked = character.bake_to(folder)
	_check("a OneShot without a request is refused, and says why", not baked.ok and String(baked.error).contains("Kick/request"), str(baked.get("error", "")))

	# Nested state machines (M232), flattened: Combat holds Slash then Recover, which reaches its End.
	# Into it goes to its start; its ways out leave from each of its states, At End from where it
	# ends; its own transitions come after its parent's.
	tree.tree_root = machine
	var combat := AnimationNodeStateMachine.new()
	var slash := AnimationNodeAnimation.new()
	slash.animation = second
	combat.add_node("Slash", slash)
	var recover := AnimationNodeAnimation.new()
	recover.animation = first
	combat.add_node("Recover", recover)
	combat.add_transition("Start", "Slash", AnimationNodeStateMachineTransition.new())
	var slashed := AnimationNodeStateMachineTransition.new()
	slashed.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	slashed.switch_mode = AnimationNodeStateMachineTransition.SWITCH_MODE_AT_END
	combat.add_transition("Slash", "Recover", slashed)
	var done := AnimationNodeStateMachineTransition.new()
	done.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	done.advance_expression = "state_time > 0.5"
	combat.add_transition("Recover", "End", done)
	machine.add_node("Combat", combat)
	var attack := AnimationNodeStateMachineTransition.new()
	attack.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	attack.advance_expression = "aiming"
	machine.add_transition("Idle", "Combat", attack)
	var after := AnimationNodeStateMachineTransition.new()
	after.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	after.switch_mode = AnimationNodeStateMachineTransition.SWITCH_MODE_AT_END
	after.xfade_time = 0.3
	machine.add_transition("Combat", "Idle", after)
	var flee := AnimationNodeStateMachineTransition.new()
	flee.advance_mode = AnimationNodeStateMachineTransition.ADVANCE_MODE_AUTO
	flee.advance_expression = "speed > 3"
	machine.add_transition("Combat", "Fast", flee)
	character.graph_inputs = {"Fast/Scale/scale": "speed * 0.5"}
	baked = character.bake_to(folder)
	_check("a nested state machine bakes", baked.ok, str(baked.get("error", "")))
	graph = _graph(folder)
	_check("its states, under its name", graph.contains("state\tCombat/Slash\tclip\t%s\t0\n" % second) and
		graph.contains("state\tCombat/Recover\tclip\t%s\t0\n" % first), graph)
	_check("into it: to its start", _line_starting(graph, "transition\tIdle\tCombat/Slash\t") != "", _line_starting(graph, "transition\tIdle\tCombat"))
	_check("out of it: from each of its states", _line_starting(graph, "transition\tCombat/Slash\tFast\t") != "" and
		_line_starting(graph, "transition\tCombat/Recover\tFast\t") != "")
	var at_end := _line_starting(graph, "transition\tCombat/Recover\tIdle\t")
	_check("out at its end: from where it ends, on both conditions", at_end == "transition\tCombat/Recover\tIdle\t1\t0.3\timmediate\t1\tstate_time > 0.5", at_end)
	_check("... and not from where it does not end", _line_starting(graph, "transition\tCombat/Slash\tIdle\t") == "")
	_check("its own transitions after its parent's", graph.find("transition\tCombat/Slash\tCombat/Recover\t") > graph.find("transition\tCombat/Recover\tFast\t"),
		_line_starting(graph, "transition\tCombat/Slash\tCombat/Recover\t"))

	print("check_anim_tree: %s" % ("all held" if _failures == 0 else "%d failed" % _failures))
	quit(1 if _failures > 0 else 0)
