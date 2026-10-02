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
##     └── Counted             on test.fired, event.value > 0   CountedMark.hide()     waits for the server

var _failures := 0
var _frames := 0
var _world: CbDirector
var _p0: Node3D
var _p1: Node3D
var _effects: Node3D
var _predict: CbPrediction
var _sound: Node3D
var _counted: Node3D
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


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames != 2:
		return false
	var server := {"value": 1, "point": Vector3(1, 1, 1), "end": Vector3(9, 1, 1)}

	# The press plays what needs nothing from the server.
	var why: Dictionary = _world.explain_press("fire")
	_check("explain_press: says the cue it would predict", String(why.get("Effects/Predict", "")) == "predicts test.fired")
	_check("press: predicts one cue", _world.press("fire") == 1)
	_check("press: the predicted signal names it", _signals == ["test.fired"])
	_check("press: the sound plays at once", _sounded())
	_check("press: the local kick plays at once", _kicks == 1)
	_check("press: what needs the cue's end or other entity waits", _marks() == 0)
	_check("press: what reads event.value waits", not _played(_counted))
	var waits: Dictionary = _world.explain("test.fired", _p0, null, {})
	_check("explain: an ordinary cue is not held back", String(waits.get("Effects/Sound", "")) == "acts")

	# The server's cue for the viewer is the echo: only what waited plays.
	_world.cue("test.fired", _p0, _p1, server)
	_check("echo: the sound does not play twice", not _sounded())
	_check("echo: the kick does not play twice", _kicks == 1)
	_check("echo: the tracer and the marker play now", _marks() == 2)
	_check("echo: the reaction on event.value plays now", _played(_counted))

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

	# No local player: nothing to predict for.
	_predict.cooldown = 0.0
	_world.set_local(null)
	_check("no local player: no prediction", _world.press("fire") == 0)

	print("predictions: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
	return true
