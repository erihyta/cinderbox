extends SceneTree
## Checks what a rules sheet's motion blocks bake to (docs/rules-language.md):
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_motion_blocks.gd
##
## A motion block in a sheet under a CbMotionSet, or under a CbItem, bakes into the same motions
## file as the CbMotion nodes, in its version 3: steps (then when, after, a timeout), guards (if,
## else: as conditions written into the file), ends, a move's seconds, an edge (when). The reactions
## of the same sheet are still made into nodes; the motion blocks are not. A block that cannot run
## refuses the bake and says its file and line. Exit code 0 when everything held.

const SHEET := "res://addons/cinderbox_maps/check_motion_blocks.rules"
const SCRATCH := "res://addons/cinderbox_maps/check_motion_blocks_scratch.rules"

var _failures := 0


func _initialize() -> void:
	root.add_child(_set_with(SHEET, true))


func _check(what: String, ok: bool, detail: String = "") -> void:
	print("  %-66s %s  %s" % [what, "ok  " if ok else "FAIL", detail])
	if not ok:
		_failures += 1


func _set_with(file: String, with_node: bool) -> CbMotionSet:
	var set := CbMotionSet.new()
	set.name = "Moves"
	set.set_name = "test.moves"
	if with_node:
		var dash := CbMotion.new()
		dash.name = "Dash"
		dash.action = "dash"
		set.add_child(dash)
	var rules := CbRules.new()
	rules.name = "Rules"
	rules.file = file
	set.add_child(rules)
	return set


func _scratch(text: String) -> void:
	var f := FileAccess.open(SCRATCH, FileAccess.WRITE)
	f.store_string(text)
	f.close()


func _refused(what: String, text: String, says: String, with_node: bool = false) -> void:
	_scratch(text)
	var set := _set_with(SCRATCH, with_node)
	var baked: Dictionary = set.bake()
	var error := String(baked["error"])
	_check(what, String(baked["text"]).is_empty() and error.contains(says), error)
	set.free()


var _done := false


# (In the first frame: a node added to the tree is ready then, and its sheet read.)
func _process(_delta: float) -> bool:
	if _done:
		return true
	_done = true
	_run()
	return true


func _run() -> void:
	print("check_motion_blocks: a sheet's motion blocks bake into the motions file")
	var set: CbMotionSet = root.get_node("Moves")
	var baked: Dictionary = set.bake()
	var text := String(baked["text"])
	_check("the set bakes", text.is_empty() == false, String(baked["error"]))
	_check("as a file of version 3", text.begins_with("cinderbox_motions\t3\n"))
	_check("the node's motion and the sheet's three", int(baked["motions"]) == 4, str(baked["motions"]))
	var slam := "motion\tSlam\nwhen\tpress\tslam\nif\t( not grounded )\ncooldown\t2\nduration\t3\nemit\tslam.started\n" + \
		"impulse\tself\t25\tworld\tvertical\t0\t-1\t0\nparam\tgravity\t30\n" + \
		"step\t0\t2\tgrounded\nemit\tslam.landed\n" + \
		"guard\t( slam.big > 0 )\nemit\tslam.big_landed\nparam\twalk_speed\t0\t0.5\n" + \
		"guard\tnot ( slam.big > 0 )\nemit\tslam.small_landed\n" + \
		"guard\tnot ( slam.big > 0 ) and ( slam.loud )\nemit\tslam.loud\n" + \
		"guard\nchange\tslam.count\t+=\t1\ntimeout\nemit\tslam.missed\n"
	_check("a dive and a landing: a step, ifs as guards, a timeout", text.contains(slam), text)
	_check("an edge: once, as it turns true", text.contains("motion\tLand\nwhen\tedge\tgrounded\nif\t( not motion.Slam )\nemit\tlanded\n"))
	var later := "motion\tLater\nwhen\tpress\tlater\nuses\t2\t3\nemit\tlater.a\nstep\t0.5\t0\nemit\tlater.b\n" + \
		"guard\t( later.stop )\nend\nguard\nparam\tfriction\t0\t1\n"
	_check("a wait by time, an end under an if, a move's seconds", text.contains(later))
	var rules := set.get_node("Rules") as CbRules
	_check("the sheet's reaction is still a node; its motions are not", rules.get_reaction_count() == 1 and rules.get_problems().is_empty(),
		"%d %s" % [rules.get_reaction_count(), rules.get_problems()])
	set.queue_free()

	# Under an item: its use, and who may run the others (whoever has it out, or carries it).
	_scratch("motion Smash on use\n\temit hammer.smash\n\tthen when grounded\n\temit hammer.landed\n" +
		"motion Glow when speed > 5\n\titem carried\n\temit hammer.fast\n" +
		"motion Spin on press spin\n\temit hammer.spin\n")
	var item := CbItem.new()
	item.name = "Hammer"
	item.kind = "test.hammer"
	var item_rules := CbRules.new()
	item_rules.name = "Rules"
	item_rules.file = SCRATCH
	item.add_child(item_rules)
	var motions: Dictionary = item.bake_motions()
	var item_text := String(motions["text"])
	_check("an item's sheet bakes with it", item_text.begins_with("cinderbox_motions\t3\n"), String(motions["error"]))
	_check("on use: the item's use", item_text.contains("motion\tSmash\nwhen\tevent\ttest.hammer.used\nemit\thammer.smash\nstep\t0\t0\tgrounded\n"))
	_check("item carried: whoever has it", item_text.contains("motion\tGlow\nwhen\tedge\tspeed > 5\nif\t( carried.test.hammer )\n"))
	_check("by default: whoever has it out", item_text.contains("motion\tSpin\nwhen\tpress\tspin\nif\t( test.hammer )\n"))
	item.free()

	# What refuses the bake, with its file and line.
	_refused("a timeout's lasting statement", "motion A on press a\n\tthen when grounded within 1\n\ttimeout\n\tmove friction 0\n", "happen once")
	_refused("a line not indented under its motion", "motion A on press a\nemit x\n", "line 2: a motion's lines are indented")
	_refused("an else with no if", "motion A on press a\n\telse\n", "line 2: an else is under an if")
	_refused("a setting under an if", "motion A on press a\n\tif x\n\t\tcooldown 1\n", "line 3: \"cooldown\" is the motion's")
	_refused("a setting after a step", "motion A on press a\n\tthen when grounded\n\tcooldown 1\n", "line 3: \"cooldown\" is the motion's: before")
	_refused("a while with steps", "motion A while flying\n\tafter 1\n\temit x\n", "a while motion has no steps")
	_refused("on use in a motion set", "motion A on use\n\temit x\n", "line 1: on use is for an item's sheet")
	_refused("a name a node has", "motion Dash on press a\n\temit x\n", "named Dash already", true)
	_refused("a line no motion has", "motion A on press a\n\tfly 3\n", "line 2: \"fly\" is not a line of a motion")
	_refused("a timeout of nothing", "motion A on press a\n\temit x\n\ttimeout\n", "line 3: a timeout comes right after")
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SCRATCH))

	print("check_motion_blocks: %s" % ("all held" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
