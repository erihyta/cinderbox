extends SceneTree
## Checks the cue addon on its own: a CbDirector driven by hand, the way anything can drive it.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_reactions.gd
##
## World (CbDirector)
## ├── player_0              entity   {combat.dead}
## │   ├── RightHand
## │   │   └── Item           entity (item, test.bat)   {melee.hot}
## │   │       ├── Barrel, Timer, HurtTimer, AnimationPlayer
## │   │       ├── Glow        while melee.hot            Barrel emission = 4, animations glow / fade
## │   │       ├── Hit         on melee.hit, subject ^^   Timer.start(), animation flash
## │   │       ├── Hurt        on melee.hit, subject ^^, side B   HurtTimer.start()
## │   │       ├── Escape      on melee.hit, a path out of the world
## │   │       └── Bad         on melee.hit, queue_free
## │   └── Head
## ├── player_1              entity   {combat.health}
## │   └── Head
## └── Effects               a world scene
##     ├── Burst               on melee.hit, subject $other, $other:combat.health < 50   a scene at the cue point
##     ├── Mark                on melee.hit, subject $at   a scene at $other/Head
##     ├── Shake               on melee.hit, subject $at, is_local   screen_effect
##     └── Blind               while $local exists: hide $local/Head (follows set_local)

var _failures := 0
var _frames := 0
var _world: CbDirector
var _p0: Node3D
var _p1: Node3D
var _item: Node3D
var _material: StandardMaterial3D
var _timer: Timer
var _hurt_timer: Timer
var _outside: Timer
var _player: AnimationPlayer
var _effects: Node3D
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


func _timer_node(name: String, parent: Node) -> Timer:
	var t := Timer.new()
	t.name = name
	t.wait_time = 10.0
	parent.add_child(t)
	return t


func _reaction(name: String, parent: Node, when: int, event: String) -> CbReaction:
	var r := CbReaction.new()
	r.name = name
	r.when = when
	r.event = event
	parent.add_child(r)
	return r


func _spark_scene() -> String:
	var spark := PackedScene.new()
	var root := Node3D.new()
	root.name = "Spark"
	spark.pack(root)
	root.free()
	ResourceSaver.save(spark, "user://check_reaction_spark.tscn")
	return "user://check_reaction_spark.tscn"


func _initialize() -> void:
	_outside = _timer_node("Outside", get_root())
	_world = CbDirector.new()
	_world.name = "World"
	_world.auto_update = false
	get_root().add_child(_world)
	_world.screen_effect.connect(func(_s, _t, _c, _f): _shakes += 1)

	_p0 = _node3d("player_0", _world)
	_world.add_entity(_p0, "player", "")
	_world.set_state(_p0, {"combat.dead": false})
	_node3d("Head", _p0).position = Vector3(0, 1.7, 0)
	var hand := _node3d("RightHand", _p0)
	_item = _node3d("Item", hand)
	_world.add_entity(_item, "item", "test.bat")
	_world.set_state(_item, {"melee.hot": false})
	_p1 = _node3d("player_1", _world)
	_p1.position = Vector3(5, 0, 0)
	_world.add_entity(_p1, "player", "")
	_world.set_state(_p1, {"combat.health": 100})
	_node3d("Head", _p1).position = Vector3(0, 1.7, 0)
	_world.set_local(_p0)

	var barrel := MeshInstance3D.new()
	barrel.name = "Barrel"
	barrel.mesh = BoxMesh.new()
	_material = StandardMaterial3D.new()
	barrel.set_surface_override_material(0, _material)
	_item.add_child(barrel)
	_timer = _timer_node("Timer", _item)
	_hurt_timer = _timer_node("HurtTimer", _item)
	_player = AnimationPlayer.new()
	_player.name = "AnimationPlayer"
	var library := AnimationLibrary.new()
	for n in ["flash", "glow", "fade"]:
		var a := Animation.new()
		a.length = 1.0
		library.add_animation(n, a)
	_player.add_animation_library("", library)
	_item.add_child(_player)

	var glow := _reaction("Glow", _item, CbReaction.WHEN_WHILE, "")
	glow.conditions = PackedStringArray(["melee.hot"])
	glow.target = NodePath("../Barrel")
	glow.property = "surface_material_override/0:emission_energy_multiplier"
	glow.value = 4.0
	glow.animation_player = NodePath("../AnimationPlayer")
	glow.animation = "glow"
	glow.animation_off = "fade"
	var hit := _reaction("Hit", _item, CbReaction.WHEN_EVENT, "melee.hit")
	hit.subject = NodePath("^^")
	hit.target = NodePath("^/Timer")
	hit.method = "start"
	hit.animation_player = NodePath("../AnimationPlayer")
	hit.animation = "flash"
	var hurt := _reaction("Hurt", _item, CbReaction.WHEN_EVENT, "melee.hit")
	hurt.subject = NodePath("^^")
	hurt.event_side = CbReaction.SIDE_B
	hurt.target = NodePath("../HurtTimer")
	hurt.method = "start"
	var escape := _reaction("Escape", _item, CbReaction.WHEN_EVENT, "melee.hit")
	escape.subject = NodePath("^^")
	escape.target = NodePath("../../../../../Outside")
	escape.method = "start"
	var bad := _reaction("Bad", _item, CbReaction.WHEN_EVENT, "melee.hit")
	bad.subject = NodePath("^^")
	bad.target = NodePath("../Barrel")
	bad.method = "queue_free"

	var spark := _spark_scene()
	_effects = _node3d("Effects", _world)
	var burst := _reaction("Burst", _effects, CbReaction.WHEN_EVENT, "melee.hit")
	burst.subject = NodePath("$other")
	burst.conditions = PackedStringArray(["$other:combat.health < 50", "event.value > 0"])
	burst.scene = spark
	burst.scene_lifetime = 1.0
	burst.place = CbReaction.PLACE_EVENT_POINT
	var mark := _reaction("Mark", _effects, CbReaction.WHEN_EVENT, "melee.hit")
	mark.subject = NodePath("$at")
	mark.scene = spark
	mark.scene_lifetime = 1.0
	mark.place = CbReaction.PLACE_NODE
	mark.place_node = NodePath("$other/Head")
	var shake := _reaction("Shake", _effects, CbReaction.WHEN_EVENT, "melee.hit")
	shake.subject = NodePath("$at")
	shake.conditions = PackedStringArray(["is_local"])
	shake.shake = 0.1
	var blind := _reaction("Blind", _effects, CbReaction.WHEN_WHILE, "")
	blind.subject = NodePath("$local")
	blind.conditions = PackedStringArray(["is_local"])
	blind.target = NodePath("$local/Head")
	blind.property = "visible"
	blind.value = false


func _sparks_at(where: Vector3) -> bool:
	for child in _world.get_children():
		if child.scene_file_path == "user://check_reaction_spark.tscn" and (child as Node3D).global_position.is_equal_approx(where):
			return true
	return false


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames == 2:
		var before := _material.emission_energy_multiplier
		# State: a "while" on the item's own state.
		_world.set_state(_item, {"melee.hot": true})
		_world.update()
		_check("while: the entity's state turns it on (sets the property)", is_equal_approx(_material.emission_energy_multiplier, 4.0))
		_check("while: plays its animation", _player.current_animation == "glow")
		_world.set_state(_item, {"melee.hot": false})
		_world.update()
		_check("while ending: puts the property back", is_equal_approx(_material.emission_energy_multiplier, before))
		_check("while ending: plays its off animation", _player.current_animation == "fade")
		# A while on $local follows set_local.
		_check("while on $local: hides the local player's head", not _p0.get_node("Head").visible)
		_world.set_local(_p1)
		_world.update()
		_check("while on $local: moves with set_local (puts the old one back)", _p0.get_node("Head").visible and not _p1.get_node("Head").visible)
		_world.set_local(_p0)
		_world.update()

		# A cue at player_0 (the holder), about player_1.
		_world.set_state(_p1, {"combat.health": 40})
		_world.cue("melee.hit", _p0, _p1, {"value": 25, "point": Vector3(1, 2, 3)})
		_check("cue: subject ^^ is the holder, ^/Timer the item's (method)", not _timer.is_stopped())
		_check("cue: plays its animation", _player.current_animation == "flash")
		_check("cue: side B does not fire for the attacker's item", _hurt_timer.is_stopped())
		_check("cue: a world reaction on $other, with $other:state and event.value", _sparks_at(Vector3(1, 2, 3)))
		_check("cue: placed at $other/Head", _sparks_at(Vector3(5, 1.7, 0)))
		_check("cue: is_local and screen_effect", _shakes == 1)
		_check("never outside the world", _outside.is_stopped())
		# The other way round: the item's holder is the one hit.
		_world.cue("melee.hit", _p1, _p0, {"value": 0, "point": Vector3(7, 7, 7)})
		_check("cue: side B fires when the holder is the other one", not _hurt_timer.is_stopped())
		_check("cue: conditions hold back a world reaction (event.value 0)", not _sparks_at(Vector3(7, 7, 7)))
		_check("cue: is_local is false for the other player", _shakes == 1)
	if _frames == 4:
		_check("queue_free is refused", _item.get_node_or_null("Barrel") != null)
	if _frames > 4:
		var left := 0
		for child in _world.get_children():
			if child.scene_file_path == "user://check_reaction_spark.tscn":
				left += 1
		if left == 0 or _frames == 600:
			_check("cue scenes go after their lifetime", left == 0)
			print("reactions: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
			quit(0 if _failures == 0 else 1)
			return true
	return false
