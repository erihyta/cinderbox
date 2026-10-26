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

	print("check_anim_tree: %s" % ("all held" if _failures == 0 else "%d failed" % _failures))
	quit(1 if _failures > 0 else 0)
