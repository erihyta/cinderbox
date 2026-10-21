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

	# Finds: a region drawn in the scene (an Area3D, a box ahead), or one written in the line.
	_scratch("motion Wave on press wave\n\tfind Wave players bodies\n\t\timpulse 9 along away\n\t\timpulse 4 along up\n" +
		"\t\tmove for 1 walk_speed 0\n\t\temit wave.hit\n" +
		"motion Poke on press poke\n\tfind Cone players max 1 along look\n\t\temit poke.hit\n" +
		"motion Near on press near\n\tif not grounded\n\t\tfind sphere 2 self\n\t\t\timpulse 3 on self along up\n\temit near.done\n")
	var finds := _set_with(SCRATCH, false)
	var region := Area3D.new()
	region.name = "Wave"
	var ball := CollisionShape3D.new()
	var sphere := SphereShape3D.new()
	sphere.radius = 4.0
	ball.shape = sphere
	ball.position = Vector3(0, 1, 0)
	region.add_child(ball)
	finds.add_child(region)
	var cone := CollisionShape3D.new()
	cone.name = "Cone"
	var box := BoxShape3D.new()
	box.size = Vector3(1, 2, 2.5)
	cone.shape = box
	cone.position = Vector3(0, 1, -1.75)  # ahead: Godot's forward is -Z
	finds.add_child(cone)
	var found: Dictionary = finds.bake()
	var found_text := String(found["text"])
	_check("finds bake", found_text.is_empty() == false, String(found["error"]))
	_check("a region: its sphere, then what happens to each", found_text.contains("motion\tWave\nwhen\tpress\twave\nfind\tall\t0\t16\tfacing\n" +
		"shape\t4\t0\t1\t0\nimpulse\tfound\t9\taway\tnone\nimpulse\tfound\t4\tup\tnone\nparam\twalk_speed\t0\t1\nemit\twave.hit\nendfind\n"), found_text)
	_check("a box ahead: turned to the simulation's forward (+Z), players, one, by the look",
		found_text.contains("find\tplayers\t0\t1\tlook\nshape\t0\t0.5\t0\t3\t-0.5\t0\t3\t0.5\t2\t3\t-0.5\t2\t3\t0.5\t0\t0.5\t"))
	_check("written in the line, the player itself, under an if",
		found_text.contains("guard\t( not grounded )\nfind\tall\t1\t16\tfacing\nshape\t2\t0\t1\t0\nimpulse\tself\t3\tup\tnone\nendfind\nguard\nemit\tnear.done\n"))
	finds.free()

	# A find's region under an item is not part of the item's body.
	var holder := CbItem.new()
	holder.kind = "test.wand"
	var body := CollisionShape3D.new()
	body.shape = BoxShape3D.new()
	holder.add_child(body)
	var zone := Area3D.new()
	var zone_shape := CollisionShape3D.new()
	zone_shape.shape = SphereShape3D.new()
	zone.add_child(zone_shape)
	holder.add_child(zone)
	var item_baked: Dictionary = holder.bake()
	var item_lines := String(item_baked["text"])
	_check("an Area3D under an item is not its body", item_lines.contains("box") and not item_lines.contains("sphere"), item_lines.substr(0, 400))
	holder.free()

	# Casts: a line thrown when the motion starts; what is under it happens while it holds.
	_scratch("motion Grab on press grab if not linked\n\tuntil pressed.grab\n\temit grab.thrown\n" +
		"\tcast sphere 0.5 range 10 speed 30 holds bodies players\n\t\temit grab.held\n\t\tlink length 2\n\t\timpulse 3 along up\n" +
		"motion Swipe on press swipe\n\tcast Blade range 3\n\t\timpulse 6 along look\n")
	var casts := _set_with(SCRATCH, false)
	var blade := CollisionShape3D.new()
	blade.name = "Blade"
	var flat := BoxShape3D.new()
	flat.size = Vector3(2, 0.1, 0.1)
	blade.shape = flat
	casts.add_child(blade)
	var cast_baked: Dictionary = casts.bake()
	var cast_text := String(cast_baked["text"])
	_check("casts bake", cast_text.is_empty() == false, String(cast_baked["error"]))
	_check("a ball thrown to fly: the probe, what it holds, its shape, its event, on what it holds",
		cast_text.contains("motion\tGrab\nwhen\tpress\tgrab\nif\t( not linked )\nuntil\t( pressed.grab )\nemit\tgrab.thrown\n" +
		"probe\t10\t30\nholds\tbodies\tplayers\nprobe_shape\t0.5\t0\t0\t0\nhold_emit\tgrab.held\nlink\thit\t2\t0\nimpulse\thit\t3\tup\tnone\n"), cast_text)
	_check("a blade drawn in the scene, around its own origin, at once",
		cast_text.contains("motion\tSwipe\nwhen\tpress\tswipe\nprobe\t3\t0\nprobe_shape\t0\t1\t-0.05\t0.05\t-1\t-0.05\t0.05\t"))
	casts.free()
	_refused("a cast in a later step", "motion A on press a\n\tafter 1\n\tcast ray range 5\n\t\timpulse 1\n", "line 3: a cast is thrown when the motion starts")
	_refused("a cast under an if", "motion A on press a\n\tif grounded\n\t\tcast ray range 5\n", "line 3: a cast is thrown when the motion starts")
	_refused("two casts", "motion A on press a\n\tcast ray range 5\n\tcast ray range 5\n", "line 3: a motion casts once")
	_refused("an impulse beside a cast", "motion A on press a\n\timpulse 3\n\tcast ray range 5\n", "line 2: with a cast")
	_refused("two events when it holds", "motion A on press a\n\tcast ray range 5\n\t\temit x\n\t\temit y\n", "line 4: a cast says one event")
	_refused("a cast with no range", "motion A on press a\n\tcast ray\n", "line 2: cast SHAPE range R")

	# Numbers that are expressions, and a curve drawn in a CbCurve beside the sheet.
	_scratch("motion Wave on press wave\n\tfind sphere 6\n\t\timpulse 9 * curve(Falloff, reach) along away\n" +
		"\tforce 2 * dash.power along up\n")
	var curves := _set_with(SCRATCH, false)
	var falloff := CbCurve.new()
	falloff.name = "Falloff"
	var line := Curve.new()
	line.add_point(Vector2(0, 1), 0, -1, Curve.TANGENT_LINEAR, Curve.TANGENT_LINEAR)
	line.add_point(Vector2(1, 0), -1, 0, Curve.TANGENT_LINEAR, Curve.TANGENT_LINEAR)
	falloff.curve = line
	curves.add_child(falloff)
	var curve_baked: Dictionary = curves.bake()
	var curve_text := String(curve_baked["text"])
	_check("curves bake", curve_text.is_empty() == false, String(curve_baked["error"]))
	var curve_line := ""
	for each in curve_text.split("\n"):
		if each.begins_with("curve\tFalloff\t"):
			curve_line = each
	var values := curve_line.split("\t")
	_check("the curve: its domain, then 64 values from 1 to 0", values.size() == 68 and values[2] == "0" and values[3] == "1" and
		values[4] == "1" and values[67] == "0" and absf(float(values[35]) - 0.507937) < 0.001, curve_line.substr(0, 120))
	_check("written right after the motion's name (before what reads it)", curve_text.contains("motion\tWave\ncurve\tFalloff\t"))
	_check("an expression where an impulse's speed is, and a force's strength",
		curve_text.contains("impulse\tfound\t9 * curve(Falloff, reach)\taway\tnone\n") and curve_text.contains("force\tself\tup\taccel\t2 * dash.power\t0\t0\t0\n"), curve_text)
	curves.free()
	_refused("a curve no CbCurve has", "motion A on press a\n\timpulse curve(Nope, 1) along up\n", "line 1: curve(Nope, ...): no CbCurve named Nope")

	# Markers and paths drawn in the scene: a muzzle tilted 30 degrees up, an arc 4 m ahead.
	_scratch("motion Shoot on press shoot\n\tlaunch new test.ball speed (2 * power.x) along Muzzle from Muzzle\n" +
		"motion Sidestep on press sidestep\n\timpulse 10 along look turn (90, 0)\n" +
		"motion Leap on press leap\n\tlasts 1\n\tmove gravity 0\n\tfollow Arc over 1\n")
	var drawn := _set_with(SCRATCH, false)
	var muzzle := Marker3D.new()
	muzzle.name = "Muzzle"
	muzzle.position = Vector3(0.3, 1.5, -0.5)
	muzzle.rotation_degrees = Vector3(30, 0, 0)
	drawn.add_child(muzzle)
	var arc := Path3D.new()
	arc.name = "Arc"
	var points := Curve3D.new()
	points.add_point(Vector3(0, 0, 0))
	points.add_point(Vector3(0, 1, -2))
	points.add_point(Vector3(0, 0, -4))
	arc.curve = points
	drawn.add_child(arc)
	var drawn_baked: Dictionary = drawn.bake()
	var drawn_text := String(drawn_baked["text"])
	_check("markers and paths bake", drawn_text.is_empty() == false, String(drawn_baked["error"]))
	var shoot := ""
	for each in drawn_text.split("\n"):
		if each.begins_with("launch\t"):
			shoot = each
	var shot := shoot.split("\t")
	_check("a launch along the marker: its forward turned to +Z, 30 degrees up; its speed an expression",
		shot.size() == 10 and shot[2] == "(2 * power.x)" and shot[3] == "marker" and shot[7] == "0" and absf(float(shot[8]) - 0.5) < 0.001 and
		absf(float(shot[9]) - 0.866) < 0.001, shoot)
	_check("from the marker's place, turned to +Z", drawn_text.contains("\nfrom\t-0.3\t1.5\t0.5\n"))
	_check("a turn after its effect", drawn_text.contains("impulse\tself\t10\tlook\tnone\nturn\t90\t0\n"))
	var arc_line := ""
	for each in drawn_text.split("\n"):
		if each.begins_with("path\tArc\t"):
			arc_line = each
	var arc_words := arc_line.split("\t")
	_check("a path: 32 points from where it starts, its end 4 m ahead (+Z)", arc_words.size() == 98 and arc_words[2] == "0" and
		arc_words[4] == "0" and absf(float(arc_words[97]) - 4.0) < 0.01 and absf(float(arc_words[96])) < 0.01, arc_line.substr(0, 120))
	_check("written up front, and followed", drawn_text.contains("motion\tLeap\npath\tArc\t") and drawn_text.contains("follow\tArc\t1\n"))
	drawn.free()
	_refused("along a marker that is not there", "motion A on press a\n\timpulse 3 along Nope\n", "line 2: along Nope: no Node3D")
	_refused("a path that is not there", "motion A on press a\n\tlasts 1\n\tfollow Nope over 1\n", "line 1: follow Nope: no Path3D")
	_refused("a turn of one number", "motion A on press a\n\timpulse 3 along look turn (90)\n", "line 2: turn (yaw, pitch)")

	# An item's own motion: on its expiry, where it lies; walls broken, itself removed.
	_scratch("motion Blast on expired\n\towner item\n\temit bomb.boom\n\tbreak radius bomb.radius speed 12\n\tfind sphere 4 bodies\n" +
		"\t\timpulse 10 along away\n\tremove self\n")
	var bomb := CbItem.new()
	bomb.name = "Bomb"
	bomb.kind = "test.bomb"
	var bomb_rules := CbRules.new()
	bomb_rules.name = "Rules"
	bomb_rules.file = SCRATCH
	bomb.add_child(bomb_rules)
	var bomb_baked: Dictionary = bomb.bake_motions()
	var bomb_text := String(bomb_baked["text"])
	_check("an item's own motion: on its expiry, owner item, no one's hand asked",
		bomb_text.contains("motion\tBlast\nwhen\tevent\ttest.bomb.expired\nowner\titem\nemit\tbomb.boom\nbreak\tbomb.radius\t12\t0\n" +
		"find\tbodies\t0\t16\tfacing\nshape\t4\t0\t1\t0\nimpulse\tfound\t10\taway\tnone\nendfind\nremove\n"), bomb_text + String(bomb_baked["error"]))
	bomb.free()
	_refused("owner item in a motion set", "motion A on expired\n\towner item\n\temit x\n", "line 1: owner item is for an item's sheet")
	_refused("a break with no radius", "motion A on press a\n\tbreak speed 3\n", "line 2: break radius R")

	# A repeat (every), and an impulse of momentum.
	_scratch("motion Flame on press flame\n\tlasts 1\n\temit flame.on\n\tevery 0.1\n\tfind sphere 3 players\n\t\timpulse 0.6 along away\n" +
		"\timpulse 400 along up kind momentum\n")
	var flame := _set_with(SCRATCH, false)
	var flame_baked: Dictionary = flame.bake()
	var flame_text := String(flame_baked["text"])
	_check("every: what follows repeats; an impulse of momentum", flame_text.contains("emit\tflame.on\nevery\t0.1\nfind\tplayers\t0\t16\tfacing\n" +
		"shape\t3\t0\t1\t0\nimpulse\tfound\t0.6\taway\tnone\nendfind\nimpulse_momentum\tself\t400\tup\tnone\n"), flame_text + String(flame_baked["error"]))
	flame.free()
	_refused("every under an if", "motion A on press a\n\tif grounded\n\t\tevery 1\n", "line 3: every S")
	_refused("an impulse of a kind there is not", "motion A on press a\n\timpulse 3 along up kind mass\n", "line 2: kind velocity or momentum")

	# What refuses the bake, with its file and line.
	_refused("a find of a node that is not there", "motion A on press a\n\tfind Nope\n\t\temit x\n", "line 2: find: no node Nope")
	_refused("a force inside a find", "motion A on press a\n\tfind sphere 2\n\t\tforce 3\n", "line 3: \"force\" inside a find")
	_refused("a move inside a find with no seconds", "motion A on press a\n\tfind sphere 2\n\t\tmove walk_speed 0\n", "line 3: a move inside a find")
	_refused("away outside a find", "motion A on press a\n\timpulse 3 along away\n", "what a find found")
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
