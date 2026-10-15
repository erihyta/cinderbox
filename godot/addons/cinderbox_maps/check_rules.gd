extends SceneTree
## Checks the rules sheet (CbRules, cue_rules.h): a scene's reactions as lines of text.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_rules.gd
##
## World (CbDirector)
## ├── player_0              entity
## │   └── RightHand
## │       └── Item          entity (item)   {melee.hot}
## │           ├── Barrel, Timer
## │           └── Rules     CbRules  check_rules_good.rules: while melee.hot hide Barrel;
## │                                  on melee.hit with $other:combat.health < 50, Timer.hide() and a shake
## └── player_1              entity   {combat.health}
## And a prediction (predict use as melee.swing), and a sheet with mistakes (check_rules_bad.rules): each is said with its line, the rest still works.

var _failures := 0
var _frames := 0
var _world: CbDirector
var _p0: Node3D
var _p1: Node3D
var _item: Node3D
var _barrel: Node3D
var _timer: Node3D
var _rules: CbRules
var _bad: CbRules
var _shakes := 0


func _check(what: String, ok: bool) -> void:
	print("  %-62s %s" % [what, "ok" if ok else "FAIL"])
	if not ok:
		_failures += 1


func _node3d(name: String, parent: Node) -> Node3D:
	var n := Node3D.new()
	n.name = name
	parent.add_child(n)
	return n


func _initialize() -> void:
	_world = CbDirector.new()
	_world.name = "World"
	_world.auto_update = false
	get_root().add_child(_world)
	_world.screen_effect.connect(func(_s, _t, _c, _f): _shakes += 1)
	_p0 = _node3d("player_0", _world)
	_world.add_entity(_p0, "player", "", 10)
	_item = _node3d("Item", _node3d("RightHand", _p0))
	_world.add_entity(_item, "item", "test.bat", 12)
	_world.set_state(_item, {"melee.hot": false})
	_p1 = _node3d("player_1", _world)
	_world.add_entity(_p1, "player", "", 11)
	_world.set_state(_p1, {"combat.health": 100})
	_world.set_local(_p0)
	_barrel = _node3d("Barrel", _item)
	_timer = _node3d("Timer", _item)
	_rules = CbRules.new()
	_rules.name = "Rules"
	_rules.file = "res://addons/cinderbox_maps/check_rules_good.rules"
	_item.add_child(_rules)
	_rules.owner = _item
	_bad = CbRules.new()
	_bad.name = "Bad"
	_item.add_child(_bad)
	_bad.owner = _item
	_bad.file = "res://addons/cinderbox_maps/check_rules_bad.rules"


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames == 2:
		_check("five actions and a prediction: six nodes", _rules.get_reaction_count() == 6)
		var two := {}
		for child in _rules.get_children(true):
			if child is CbReaction and child.event == "test.two":
				two[String(child.target)] = child
		_check("a setting on the line is that action's: at point, delay 0.5", two.has("../../Barrel")
			and two["../../Barrel"].place == CbReaction.PLACE_EVENT_POINT and is_equal_approx(two["../../Barrel"].delay, 0.5))
		_check("... the rule's for the others: at end, no delay", two.has("../../Timer")
			and two["../../Timer"].place == CbReaction.PLACE_EVENT_END and two["../../Timer"].delay == 0.0)
		var predicted: CbPrediction = null
		for child in _rules.get_children(true):
			if child is CbPrediction:
				predicted = child
		_check("the prediction, as written", predicted != null and predicted.action == "use" and predicted.cue == "melee.swing"
			and is_equal_approx(predicted.cooldown, 0.5) and predicted.changes == PackedStringArray(["melee.swings += 1"])
			and predicted.stance == "melee_swing" and predicted.stance_layer == "full" and predicted.conditions == PackedStringArray(["!combat.dead"]))
		_check("none of them is saved with the scene (internal)", _rules.get_child_count() == 0)
		_check("the good sheet says nothing", _rules.get_problems().is_empty())
		_world.set_state(_item, {"melee.hot": true})
		_world.update()
		_check("while melee.hot: the Barrel is hidden", not _barrel.visible)
		_world.set_state(_item, {"melee.hot": false})
		_world.update()
		_check("... and shown again when it is not", _barrel.visible)
		_world.set_state(_p1, {"combat.health": 60})
		_world.cue("melee.hit", _p0, _p1, {"value": 10})
		_check("on melee.hit with health 60: nothing", _timer.visible and _shakes == 0)
		_world.set_state(_p1, {"combat.health": 40})
		_world.cue("melee.hit", _p0, _p1, {"value": 10})
		_check("with health 40: the Timer hidden and a shake", not _timer.visible and _shakes == 1)
		var problems: PackedStringArray = _bad.get_problems()
		print("    the bad sheet: ", " | ".join(problems))
		var lines := []
		for p in problems:
			lines.append(int(p.get_slice(":", 0).get_slice(" ", 1)))
		lines.sort()
		_check("its mistakes, each at its line (3, 4, 6, 9, 12)", lines == [3, 4, 6, 9, 12])
		_check("the rule without one still works", _bad.get_reaction_count() == 1)
		_timer.visible = true
		_world.cue("test.fine", _item, null, {})
		_check("... and acts", not _timer.visible)
		quit(0 if _failures == 0 else 1)
	return false
