extends SceneTree
## Checks predictions on a bare CbDirector: the viewer's press plays the cue the server will send,
## and the server's cue then plays only what had to wait for it.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_predictions.gd
##
## World (CbDirector)
## ├── player_0 (local)      {test.gun, test.ammo}
## ├── player_1
## └── Effects
##     ├── Predict             press "fire" -> "test.fired"   test.gun, test.ammo > 0   cooldown 0.2
##     ├── Sound               on test.fired, subject $at            SoundMark.hide()  plays on the press
##     ├── Kick                on test.fired, subject $at, is_local  screen_effect     plays on the press
##     ├── Tracer              on test.fired, placed at the cue's end   a scene        waits for the server
##     ├── Marker              on test.fired, placed at $other/Head     a scene        waits for the server
##     ├── Counted             on test.fired, event.value > 0   CountedMark.hide()     waits for the server
##     └── Told                on test.fired, wait_for_server   ToldMark.hide()        waits because it says so

var _failures := 0
var _frames := 0
var _world: CbDirector
var _p0: Node3D
var _p1: Node3D
var _effects: Node3D
var _predict: CbPrediction
var _sound: Node3D
var _counted: Node3D
var _told: Node3D
var _kicks := 0
var _signals: Array[String] = []
const SCENE := "user://check_prediction_mark.tscn"


func _check(what: String, ok: bool) -> void:
	print("  %-66s %s" % [what, "ok" if ok else "FAIL"])
	if not ok:
		_failures += 1


func _node3d(name: String, parent: Node) -> Node3D:
	var n := Node3D.new()
	n.name = name
	parent.add_child(n)
	return n


# Something a reaction can act on with a listed method: hide() it, and it shows that it played.
func _mark(name: String) -> Node3D:
	var m := Node3D.new()
	m.name = name
	_effects.add_child(m)
	return m


func _played(mark: Node3D) -> bool:
	var was := not mark.visible
	mark.show()
	return was


func _reaction(name: String) -> CbReaction:
	var r := CbReaction.new()
	r.name = name
	r.event = "test.fired"
	r.subject = NodePath("$at")
	_effects.add_child(r)
	return r


func _marks() -> int:
	var count := 0
	for child in _world.get_children() + _p1.get_node("Head").get_children():
		if child.scene_file_path == SCENE and not child.is_queued_for_deletion():
			count += 1
	return count


func _sounded() -> bool:
	return _played(_sound)


func _initialize() -> void:
	var mark := PackedScene.new()
	var root := Node3D.new()
	root.name = "Mark"
	mark.pack(root)
	root.free()
	ResourceSaver.save(mark, SCENE)

	_world = CbDirector.new()
	_world.name = "World"
	_world.auto_update = false
	get_root().add_child(_world)
	_world.screen_effect.connect(func(_s, _t, _c, _f): _kicks += 1)
	_world.predicted.connect(func(cue): _signals.append(cue))
	_p0 = _node3d("player_0", _world)
	_world.add_entity(_p0, "player", "", 10)
	_world.set_state(_p0, {"test.gun": true, "test.ammo": 2})
	_p1 = _node3d("player_1", _world)
	_world.add_entity(_p1, "player", "", 11)
	_node3d("Head", _p1)
	_world.set_local(_p0)
	_effects = _node3d("Effects", _world)

	_predict = CbPrediction.new()
	_predict.name = "Predict"
	_predict.action = "fire"
	_predict.cue = "test.fired"
	_predict.conditions = PackedStringArray(["test.gun", "test.ammo > 0"])
	_effects.add_child(_predict)

	_sound = _mark("SoundMark")
	_counted = _mark("CountedMark")
	_told = _mark("ToldMark")
	var sound := _reaction("Sound")
	sound.target = NodePath("../SoundMark")
	sound.method = "hide"
	var kick := _reaction("Kick")
	kick.conditions = PackedStringArray(["is_local"])
	kick.shake = 0.1
	var tracer := _reaction("Tracer")
	tracer.scene = SCENE
	tracer.scene_lifetime = 100.0
	tracer.place = CbReaction.PLACE_EVENT_END
	var marker := _reaction("Marker")
	marker.scene = SCENE
	marker.scene_lifetime = 100.0
	marker.place = CbReaction.PLACE_NODE
	marker.place_node = NodePath("$other/Head")
	var counted := _reaction("Counted")
	counted.conditions = PackedStringArray(["event.value > 0"])
	counted.target = NodePath("../CountedMark")
	counted.method = "hide"
	var told := _reaction("Told")
	told.wait_for_server = true
	told.target = NodePath("../ToldMark")
	told.method = "hide"


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames != 2:
		return false
	var server := {"value": 1, "point": Vector3(1, 1, 1), "end": Vector3(9, 1, 1)}

	# What a prediction changes until the server answers travels with it.
	_predict.changes = PackedStringArray(["test.ammo -= 1"])
	_predict.stance = "test_swing"
	_predict.stance_layer = "full"
	_check("nothing is pending before a press", _world.pending_predictions().is_empty())

	# The press plays what needs nothing from the server.
	var why: Dictionary = _world.explain_press("fire")
	_check("explain_press: says the cue it would predict", String(why.get("Effects/Predict", "")) == "predicts test.fired")
	_check("press: predicts one cue", _world.press("fire") == 1)
	_check("press: the predicted signal names it", _signals == ["test.fired"])
	var pending: Array = _world.pending_predictions()
	_check("press: it is pending, with its changes and stance", pending.size() == 1 and pending[0]["cue"] == "test.fired"
		and pending[0]["changes"] == PackedStringArray(["test.ammo -= 1"]) and pending[0]["stance"] == "test_swing"
		and pending[0]["stance_layer"] == "full" and float(pending[0]["age"]) < 0.5)
	_check("press: the sound plays at once", _sounded())
	_check("press: the local kick plays at once", _kicks == 1)
	_check("press: what needs the cue's end or other entity waits", _marks() == 0)
	_check("press: what reads event.value waits", not _played(_counted))
	_check("press: wait_for_server waits", not _played(_told))
	var waits: Dictionary = _world.explain("test.fired", _p0, null, {})
	_check("explain: an ordinary cue is not held back", String(waits.get("Effects/Sound", "")) == "acts")

	# The server's cue for the viewer is the echo: only what waited plays.
	_world.cue("test.fired", _p0, _p1, server)
	_check("echo: the sound does not play twice", not _sounded())
	_check("echo: nothing is pending any more", _world.pending_predictions().is_empty())
	_check("echo: the kick does not play twice", _kicks == 1)
	_check("echo: the tracer and the marker play now", _marks() == 2)
	_check("echo: the reaction on event.value plays now", _played(_counted))
	_check("echo: wait_for_server plays now", _played(_told))

	# The echo was used up: the next cue of that name is a cue like any other.
	_world.cue("test.fired", _p0, _p1, server)
	_check("no press: the server's cue plays in full", _sounded() and _kicks == 2 and _marks() == 4)

	# Another player's cue is never an echo, even right after a press.
	_world.press("fire")
	_sounded()
	_world.cue("test.fired", _p1, _p0, server)
	_check("another player's cue plays in full after a press", _sounded())
	_world.cue("test.fired", _p0, _p1, server)
	_check("and the viewer's own is still the echo", not _sounded())

	# Two presses, two echoes.
	_predict.cooldown = 0.0
	_world.press("fire")
	_world.press("fire")
	_sounded()
	_world.cue("test.fired", _p0, _p1, server)
	_world.cue("test.fired", _p0, _p1, server)
	_check("two presses: both echoes are quiet", not _sounded())
	_world.cue("test.fired", _p0, _p1, server)
	_check("two presses: the third cue plays", _sounded())

	# Conditions and the cooldown hold a press back.
	_world.set_state(_p0, {"test.gun": true, "test.ammo": 0})
	_check("conditions: no ammo, no prediction", _world.press("fire") == 0 and not _sounded())
	why = _world.explain_press("fire")
	_check("explain_press: names the condition", String(why.get("Effects/Predict", "")).contains("test.ammo > 0"))
	_world.cue("test.fired", _p0, _p1, server)
	_check("conditions: the server disagreed, its cue plays in full", _sounded())
	_world.set_state(_p0, {"test.gun": true, "test.ammo": 2})
	_check("another action predicts nothing", _world.press("reload") == 0)
	_check("no cooldown: a press right after another predicts", _world.press("fire") == 1)
	_predict.cooldown = 10.0
	_check("cooldown: the next one does not", _world.press("fire") == 0 and String(_world.explain_press("fire").get("Effects/Predict", "")).contains("cooling"))
	_world.cue("test.fired", _p0, _p1, server)
	_sounded()

	# Held: only a while_held prediction answers, once for every cooldown that passes.
	_predict.cooldown = 0.05
	_check("held: a press-only prediction does not repeat", _world.hold("fire") == 0)
	OS.delay_msec(60)
	_check("... and the press still predicts", _world.press("fire") == 1)
	_predict.while_held = true
	_check("while_held: nothing before the cooldown has passed", _world.hold("fire") == 0)
	var held := 0
	var until := Time.get_ticks_msec() + 520
	while Time.get_ticks_msec() < until:
		held += _world.hold("fire")
		OS.delay_msec(4)
	_check("while_held: one cue for every cooldown held (10 in half a second)", held >= 9 and held <= 10)
	_world.set_state(_p0, {"test.gun": true, "test.ammo": 0})
	OS.delay_msec(60)
	_check("while_held: the conditions still decide", _world.hold("fire") == 0)
	_world.set_state(_p0, {"test.gun": true, "test.ammo": 2})
	_predict.cooldown = 0.0
	_check("while_held without a cooldown predicts the press only", _world.hold("fire") == 0 and _world.press("fire") == 1)
	_predict.while_held = false

	# A prediction inside an item's own scene is that item's: it speaks for the copy the viewer
	# holds, while it is in use, and no condition names the item.
	OS.delay_msec(60)
	_world.cue("test.fired", _p0, _p1, server)
	_sounded()
	var items := []
	for spec in [["mine", _p0, 20], ["theirs", _p1, 21], ["lying", _world, 22]]:
		var item := _node3d("item_%s" % spec[0], spec[1])
		_world.add_entity(item, "item", "", spec[2])
		_world.set_state(item, {"in_use": spec[1] != _world})
		var inside := CbPrediction.new()
		inside.name = "Predict"
		inside.action = "swing"
		inside.cue = "test.fired"
		item.add_child(inside)
		items.append(item)
	_check("in an item: three copies in the world are one prediction, the viewer's", _world.press("swing") == 1)
	why = _world.explain_press("swing")
	_check("... the one someone else holds says so", String(why.get("player_1/item_theirs/Predict", "")).contains("does not hold"))
	_check("... and the one lying in the world", String(why.get("item_lying/Predict", "")).contains("does not hold"))
	_world.cue("test.fired", _p0, _p1, server)
	_world.set_state(items[0], {"in_use": false})
	_check("in an item that is put away: no prediction", _world.press("swing") == 0
		and String(_world.explain_press("swing").get("player_0/item_mine/Predict", "")).contains("put away"))
	_world.set_state(items[0], {"in_use": true})
	_check("... taken out again, it predicts", _world.press("swing") == 1)
	_world.cue("test.fired", _p0, _p1, server)
	_sounded()
	for item in items:
		item.free()

	# No local player: nothing to predict for.
	_predict.cooldown = 0.0
	_world.set_local(null)
	_check("no local player: no prediction", _world.press("fire") == 0)

	print("predictions: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
	return true
