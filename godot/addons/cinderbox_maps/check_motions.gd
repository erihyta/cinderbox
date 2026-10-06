extends SceneTree
## Checks what the motion nodes bake to, on sets built here:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_motions.gd
##
## A CbMotionSet bakes its CbMotion children, in tree order, to the text the server reads from the
## mod's item (motions/<set_name>.cfg) and every simulation runs. A motion's own children are what
## it does: a CbProbe (a line that has to find something), then CbImpulse, CbForce and CbLink. A
## motion that cannot run (no action, a parameter that is none, a condition that does not parse, an
## effect on what a probe found without a probe) refuses the bake and says which node and why.
## Exit code 0 when everything held.

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


func _impulse(parent: Node, speed: float, frame: int = CbMotionEffect.FRAME_MOVE, replace: int = CbImpulse.REPLACE_NOTHING) -> CbImpulse:
	var impulse := CbImpulse.new()
	impulse.name = "Impulse%d" % parent.get_child_count()
	impulse.speed = speed
	impulse.frame = frame
	impulse.replace = replace
	parent.add_child(impulse)
	return impulse


func _initialize() -> void:
	var dash := _motion("Dash", "dash")
	dash.conditions = PackedStringArray(["dash.charges > 0", "not aiming"])
	dash.cooldown = 0.4
	dash.duration = 0.18
	# Given out of order: the file has the simulation's order.
	dash.parameters = {"gravity": 4.0, "friction": 0.0}
	dash.changes = PackedStringArray(["dash.charges -= 1", "dash.heat = 2.5"])
	dash.emits = "dash.started"
	_impulse(dash, 11.0, CbMotionEffect.FRAME_MOVE, CbImpulse.REPLACE_HORIZONTAL)
	var jump := _motion("DoubleJump", "jump")
	jump.conditions = PackedStringArray(["not grounded"])
	jump.uses = 1
	_impulse(jump, 6.5, CbMotionEffect.FRAME_UP, CbImpulse.REPLACE_VERTICAL)
	var blink := _motion("Blink", "blink")
	blink.uses = 2
	blink.refill = CbMotion.REFILL_AFTER_SECONDS
	blink.refill_seconds = 1.5
	# A world direction is made one long; two effects are two lines, in the order of the tree.
	_impulse(blink, 5.0, CbMotionEffect.FRAME_WORLD).direction = Vector3(0, 0, 2)
	_impulse(blink, 2.0, CbMotionEffect.FRAME_UP)
	var root := _make([dash, jump, blink])
	var baked: Dictionary = root.bake()
	var want := "\n".join([
		"cinderbox_motions\t2",
		"# Baked from the CbMotion nodes of a CbMotionSet. Edit the scene and bake again.",
		"motion\tDash",
		"when\tpress\tdash",
		"if\t( dash.charges > 0 ) and ( not aiming )",
		"cooldown\t0.4",
		"duration\t0.18",
		"param\tfriction\t0",
		"param\tgravity\t4",
		"impulse\tself\t11\tmove\thorizontal",
		"change\tdash.charges\t-=\t1",
		"change\tdash.heat\t=\t2.5",
		"emit\tdash.started",
		"motion\tDoubleJump",
		"when\tpress\tjump",
		"if\t( not grounded )",
		"uses\t1\tground",
		"impulse\tself\t6.5\tup\tvertical",
		"motion\tBlink",
		"when\tpress\tblink",
		"uses\t2\t1.5",
		"impulse\tself\t5\tworld\tnone\t0\t0\t1",
		"impulse\tself\t2\tup\tnone",
		""])
	_check("three motions bake, in tree order", baked.text == want and baked.motions == 3, str(baked.get("error", "")))
	if baked.text != want:
		print(baked.text)

	# While and On a cue: no press; what a While does not use (uses, duration, until) is left out.
	# A While pushes with a force: so much a second, for as long as it is on.
	var fly := CbMotion.new()
	fly.name = "Thrust"
	fly.when = CbMotion.WHEN_WHILE
	fly.conditions = PackedStringArray(["held.jump", "flight.fuel > 0"])
	fly.cooldown = 0.5
	fly.uses = 3
	fly.duration = 2.0
	fly.until = PackedStringArray(["grounded"])
	fly.parameters = {"move_frame": 1.0, "air_friction": 3.0}
	fly.changes = PackedStringArray(["flight.fuel -= 30"])
	fly.emits = "flight.thrust"
	var lift := CbForce.new()
	lift.name = "Lift"
	lift.frame = CbMotionEffect.FRAME_UP
	lift.strength = 26.0
	fly.add_child(lift)
	var stun := CbMotion.new()
	stun.name = "Stun"
	stun.when = CbMotion.WHEN_EVENT
	stun.event = "stun.hit"
	stun.duration = 0.5
	stun.parameters = {"walk_speed": 0.0}
	var held_root := _make([fly, stun])
	var held: Dictionary = held_root.bake()
	var held_want := "\n".join([
		"cinderbox_motions\t2",
		"# Baked from the CbMotion nodes of a CbMotionSet. Edit the scene and bake again.",
		"motion\tThrust",
		"when\twhile",
		"if\t( held.jump ) and ( flight.fuel > 0 )",
		"cooldown\t0.5",
		"param\tair_friction\t3",
		"param\tmove_frame\t1",
		"force\tself\tup\taccel\t26\t0\t0\t0",
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

	# A grappling hook: a probe, a force toward what it found that pushes back, and a rope. The
	# probe is written first wherever it is in the tree; what lets go is any of the conditions.
	var hook := _motion("Hook", "grapple")
	hook.parameters = {"airborne": 1.0}
	hook.until = PackedStringArray(["not held.grapple", "combat.dead"])
	hook.emits = "grapple.fired"
	var pull := CbForce.new()
	pull.name = "Pull"
	pull.frame = CbMotionEffect.FRAME_TO
	pull.kind = CbForce.KIND_FORCE
	pull.strength = 1920.0
	pull.ramp_in = 0.15
	pull.react = true
	hook.add_child(pull)
	var rope := CbLink.new()
	rope.name = "Rope"
	rope.reel = 4.0
	hook.add_child(rope)
	var line := CbProbe.new()
	line.name = "Line"
	line.range = 40.0
	line.travel = 60.0
	hook.add_child(line)
	# Two players bound: a force on the entity a field names, toward a speed, and a rope of 6 m.
	var bind := _motion("Bind", "bind")
	bind.duration = 2.0
	var drag := CbForce.new()
	drag.name = "Drag"
	drag.target = CbMotionEffect.TARGET_FIELD
	drag.target_field = "bind.partner"
	drag.frame = CbMotionEffect.FRAME_LOOK
	drag.kind = CbForce.KIND_VELOCITY
	drag.strength = 30.0
	drag.speed = 8.0
	bind.add_child(drag)
	var leash := CbLink.new()
	leash.name = "Leash"
	leash.target = CbMotionEffect.TARGET_FIELD
	leash.target_field = "bind.partner"
	leash.length = 6.0
	bind.add_child(leash)
	var link_root := _make([hook, bind])
	var links: Dictionary = link_root.bake()
	var link_want := "\n".join([
		"cinderbox_motions\t2",
		"# Baked from the CbMotion nodes of a CbMotionSet. Edit the scene and bake again.",
		"motion\tHook",
		"when\tpress\tgrapple",
		"until\t( not held.grapple ) or ( combat.dead )",
		"param\tairborne\t1",
		"probe\t40\t60",
		"force\tself\tto\tforce\t1920\t0\t0.15\t1",
		"link\thit\t0\t4",
		"emit\tgrapple.fired",
		"motion\tBind",
		"when\tpress\tbind",
		"duration\t2",
		"force\t@bind.partner\tlook\tvelocity\t30\t8\t0\t0",
		"link\t@bind.partner\t6\t0",
		""])
	_check("a probe, forces and links bake", links.text == link_want, str(links.get("error", "")))
	if links.text != link_want:
		print(links.text)
	rope.target = CbMotionEffect.TARGET_SELF
	_check("a rope to the player itself refuses the bake", link_root.bake().text == "" and String(link_root.bake().error).contains("Rope"), link_root.bake().error)
	rope.target = CbMotionEffect.TARGET_HIT
	leash.length = 0.0
	_check("a rope without a probe needs its length", link_root.bake().text == "", link_root.bake().error)
	leash.length = 6.0
	drag.target_field = ""
	_check("a field target without its field refuses the bake", link_root.bake().text == "" and String(link_root.bake().error).contains("Drag"), link_root.bake().error)
	drag.target_field = "bind.partner"
	hook.remove_child(line)
	_check("an effect on what a probe found needs a probe", link_root.bake().text == "" and String(link_root.bake().error).contains("probe"), link_root.bake().error)
	hook.add_child(line)
	hook.when = CbMotion.WHEN_WHILE
	hook.conditions = PackedStringArray(["grounded"])
	_check("a While with a probe refuses the bake", link_root.bake().text == "" and String(link_root.bake().error).contains("Hook"), link_root.bake().error)
	line.free()
	link_root.free()

	# An effect that is not under a motion.
	var stray_root := _make([_motion("Dash", "dash")])
	var stray := CbImpulse.new()
	stray.name = "Stray"
	stray.speed = 5.0
	stray_root.add_child(stray)
	_check("an effect outside a motion refuses the bake", stray_root.bake().text == "" and String(stray_root.bake().error).contains("Stray"), stray_root.bake().error)
	stray_root.free()

	# What refuses the bake, and says the node.
	for case in [
		["no action", func(m: CbMotion): m.action = "", "Dash"],
		["an action of two words", func(m: CbMotion): m.action = "dash now", "Dash"],
		["a parameter that is none", func(m: CbMotion): m.parameters = {"fly_speed": 3.0}, "fly_speed"],
		["a parameter outside its range", func(m: CbMotion): m.parameters = {"gravity": -1.0}, "Dash"],
		["a condition that does not parse", func(m: CbMotion): m.conditions = PackedStringArray(["grounded and"]), "Dash"],
		["a change without an operator", func(m: CbMotion): m.changes = PackedStringArray(["dash.charges 1"]), "dash.charges 1"],
		["an impulse past 200 m/s", func(m: CbMotion): (m.get_child(0) as CbImpulse).speed = 500.0, "Dash"],
		["an event of two words", func(m: CbMotion): m.emits = "dash started", "Dash"],
	]:
		var bad := _motion("Dash", "dash")
		_impulse(bad, 5.0)
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
		_impulse(m, 1.0)
		many.append(m)
	var big := _make(many)
	_check("more than 16 motions refuse the bake", big.bake().text == "" and String(big.bake().error).contains("16"), big.bake().error)
	big.free()

	print("motions: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
