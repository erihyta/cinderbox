extends SceneTree
## Checks what CbMotion nodes bake to, on sets built here:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_motions.gd
##
## A CbMotionSet bakes its CbMotion children, in tree order, to the text the server reads from the
## mod's item (motions/<set_name>.cfg) and every simulation runs. A motion that cannot run (no
## action, a parameter that is none, a condition that does not parse) refuses the bake and says
## which node and why. Exit code 0 when everything held.

var _failures := 0


func _check(what: String, ok: bool, detail: String = "") -> void:
	print("  %-66s %s  %s" % [what, "ok  " if ok else "FAIL", detail])
	if not ok:
		_failures += 1


func _make(motions: Array) -> CbMotionSet:
	var root := CbMotionSet.new()
	root.name = "Moves"
	root.set_name = "test.moves"
	for motion in motions:
		root.add_child(motion)
	return root


func _motion(name: String, action: String) -> CbMotion:
	var motion := CbMotion.new()
	motion.name = name
	motion.action = action
	return motion


func _initialize() -> void:
	var dash := _motion("Dash", "dash")
	dash.conditions = PackedStringArray(["dash.charges > 0", "not aiming"])
	dash.cooldown = 0.4
	dash.impulse = 11.0
	dash.replace = CbMotion.REPLACE_HORIZONTAL
	dash.duration = 0.18
	# Given out of order: the file has the simulation's order.
	dash.parameters = {"gravity": 4.0, "friction": 0.0}
	dash.changes = PackedStringArray(["dash.charges -= 1", "dash.heat = 2.5"])
	dash.emits = "dash.started"
	var jump := _motion("DoubleJump", "jump")
	jump.conditions = PackedStringArray(["not grounded"])
	jump.uses = 1
	jump.impulse = 6.5
	jump.impulse_frame = CbMotion.FRAME_UP
	jump.replace = CbMotion.REPLACE_VERTICAL
	var blink := _motion("Blink", "blink")
	blink.uses = 2
	blink.refill = CbMotion.REFILL_AFTER_SECONDS
	blink.refill_seconds = 1.5
	blink.impulse = 5.0
	blink.impulse_frame = CbMotion.FRAME_WORLD
	blink.impulse_direction = Vector3(0, 0, 2)
	var root := _make([dash, jump, blink])
	var baked: Dictionary = root.bake()
	var want := "\n".join([
		"cinderbox_motions\t1",
		"# Baked from the CbMotion nodes of a CbMotionSet. Edit the scene and bake again.",
		"motion\tDash",
		"when\tpress\tdash",
		"if\t( dash.charges > 0 ) and ( not aiming )",
		"cooldown\t0.4",
		"duration\t0.18",
		"impulse\t11\tmove\thorizontal",
		"param\tfriction\t0",
		"param\tgravity\t4",
		"change\tdash.charges\t-=\t1",
		"change\tdash.heat\t=\t2.5",
		"emit\tdash.started",
		"motion\tDoubleJump",
		"when\tpress\tjump",
		"if\t( not grounded )",
		"uses\t1\tground",
		"impulse\t6.5\tup\tvertical",
		"motion\tBlink",
		"when\tpress\tblink",
		"uses\t2\t1.5",
		"impulse\t5\tworld\tnone\t0\t0\t1",
		""])
	_check("three motions bake, in tree order", baked.text == want and baked.motions == 3, str(baked.get("error", "")))
	if baked.text != want:
		print(baked.text)

	# While and On a cue: no press; what a While does not use (uses, duration, replace) is left out.
	var fly := CbMotion.new()
	fly.name = "Thrust"
	fly.when = CbMotion.WHEN_WHILE
	fly.conditions = PackedStringArray(["held.jump", "flight.fuel > 0"])
	fly.cooldown = 0.5
	fly.uses = 3
	fly.duration = 2.0
	fly.impulse = 26.0
	fly.impulse_frame = CbMotion.FRAME_UP
	fly.replace = CbMotion.REPLACE_ALL
	fly.parameters = {"move_frame": 1.0, "air_friction": 3.0}
	fly.changes = PackedStringArray(["flight.fuel -= 30"])
	fly.emits = "flight.thrust"
	var stun := CbMotion.new()
	stun.name = "Stun"
	stun.when = CbMotion.WHEN_EVENT
	stun.event = "stun.hit"
	stun.duration = 0.5
	stun.parameters = {"walk_speed": 0.0}
	var held_root := _make([fly, stun])
	var held: Dictionary = held_root.bake()
	var held_want := "\n".join([
		"cinderbox_motions\t1",
		"# Baked from the CbMotion nodes of a CbMotionSet. Edit the scene and bake again.",
		"motion\tThrust",
		"when\twhile",
		"if\t( held.jump ) and ( flight.fuel > 0 )",
		"cooldown\t0.5",
		"impulse\t26\tup\tnone",
		"param\tair_friction\t3",
		"param\tmove_frame\t1",
		"change\tflight.fuel\t-=\t30",
		"emit\tflight.thrust",
		"motion\tStun",
		"when\tevent\tstun.hit",
		"duration\t0.5",
		"param\twalk_speed\t0",
		""])
	_check("a While and an On a cue bake", held.text == held_want, str(held.get("error", "")))
	if held.text != held_want:
		print(held.text)
	stun.event = ""
	_check("an On a cue without its event refuses the bake", held_root.bake().text == "" and String(held_root.bake().error).contains("Stun"), held_root.bake().error)
	held_root.free()

	# What refuses the bake, and says the node.
	for case in [
		["no action", func(m: CbMotion): m.action = "", "Dash"],
		["an action of two words", func(m: CbMotion): m.action = "dash now", "Dash"],
		["a parameter that is none", func(m: CbMotion): m.parameters = {"fly_speed": 3.0}, "fly_speed"],
		["a parameter outside its range", func(m: CbMotion): m.parameters = {"gravity": -1.0}, "Dash"],
		["a condition that does not parse", func(m: CbMotion): m.conditions = PackedStringArray(["grounded and"]), "Dash"],
		["a change without an operator", func(m: CbMotion): m.changes = PackedStringArray(["dash.charges 1"]), "dash.charges 1"],
		["an impulse past 200 m/s", func(m: CbMotion): m.impulse = 500.0, "Dash"],
		["an event of two words", func(m: CbMotion): m.emits = "dash started", "Dash"],
	]:
		var bad := _motion("Dash", "dash")
		bad.impulse = 5.0
		case[1].call(bad)
		var bad_root := _make([bad])
		var result: Dictionary = bad_root.bake()
		_check("%s refuses the bake" % case[0], result.text == "" and String(result.error).contains(case[2]), result.error)
		bad_root.free()

	root.set_name = "two words"
	_check("a set name of two words refuses the bake", root.bake().text == "", root.bake().error)
	root.set_name = ""
	_check("a set without a name refuses the bake", root.bake().text == "")
	root.free()

	var many := []
	for i in 17:
		var m := _motion("M%d" % i, "dash")
		m.impulse = 1.0
		many.append(m)
	var big := _make(many)
	_check("more than 16 motions refuse the bake", big.bake().text == "" and String(big.bake().error).contains("16"), big.bake().error)
	big.free()

	print("motions: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
