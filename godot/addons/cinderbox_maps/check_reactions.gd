extends SceneTree
## Checks what CbReaction does when the game drives it:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_reactions.gd
##
## A "while" sets a property (through a material's sub-path) and puts it back, plays an animation
## and its off animation; an event calls a built-in method, restarts an animation, and adds a scene
## that goes after its lifetime; a "while" adds a scene and removes it when it ends. As the game
## drives them: node paths from another entity's root (act_on), never outside the scene they are
## resolved in, and never "queue_free".

var _failures := 0
var _frames := 0
var _thing: Node3D
var _material: StandardMaterial3D
var _timer: Timer
var _player: AnimationPlayer
var _glow: CbReaction
var _hit: CbReaction
var _burst: CbReaction
var _aura: CbReaction
var _far: CbReaction
var _far_while: CbReaction
var _escape: CbReaction
var _bad: CbReaction
var _other: Node3D
var _other_timer: Timer
var _outside_timer: Timer
var _base := 0


func _check(what: String, ok: bool) -> void:
	print("  %-58s %s" % [what, "ok" if ok else "FAIL"])
	if not ok:
		_failures += 1


func _initialize() -> void:
	_thing = Node3D.new()
	_thing.name = "Thing"
	get_root().add_child(_thing)
	var barrel := MeshInstance3D.new()
	barrel.name = "Barrel"
	barrel.mesh = BoxMesh.new()
	_material = StandardMaterial3D.new()
	barrel.set_surface_override_material(0, _material)
	_thing.add_child(barrel)
	_timer = Timer.new()
	_timer.name = "Timer"
	_timer.wait_time = 10.0
	_thing.add_child(_timer)
	_player = AnimationPlayer.new()
	_player.name = "AnimationPlayer"
	var library := AnimationLibrary.new()
	for name in ["flash", "glow", "fade"]:
		var a := Animation.new()
		a.length = 1.0
		library.add_animation(name, a)
	_player.add_animation_library("", library)
	_thing.add_child(_player)

	_glow = _reaction("Glow", CbReaction.WHEN_WHILE)
	_glow.target = NodePath("../Barrel")
	_glow.property = "surface_material_override/0:emission_energy_multiplier"
	_glow.value = 4.0
	_glow.animation_player = NodePath("../AnimationPlayer")
	_glow.animation = "glow"
	_glow.animation_off = "fade"
	_hit = _reaction("Hit", CbReaction.WHEN_EVENT)
	_hit.target = NodePath("../Timer")
	_hit.method = "start"
	_hit.animation_player = NodePath("../AnimationPlayer")
	_hit.animation = "flash"
	var spark := PackedScene.new()
	var spark_root := Node3D.new()
	spark_root.name = "Spark"
	spark.pack(spark_root)
	spark_root.free()
	ResourceSaver.save(spark, "user://check_reaction_spark.tscn")
	_burst = _reaction("Burst", CbReaction.WHEN_EVENT)
	_burst.scene = "user://check_reaction_spark.tscn"
	_burst.scene_lifetime = 1.0 # outlives frame 4 even when a frame is slow
	_aura = _reaction("Aura", CbReaction.WHEN_WHILE)
	_aura.scene = "user://check_reaction_spark.tscn"

	# Another entity's scene, and a node that belongs to no entity.
	_other = Node3D.new()
	_other.name = "Other"
	get_root().add_child(_other)
	_other_timer = Timer.new()
	_other_timer.name = "OtherTimer"
	_other_timer.wait_time = 10.0
	_other.add_child(_other_timer)
	_outside_timer = Timer.new()
	_outside_timer.name = "Outside"
	get_root().add_child(_outside_timer)
	_far = _reaction("Far", CbReaction.WHEN_EVENT)
	_far.target = NodePath("OtherTimer")
	_far.method = "start"
	_far_while = _reaction("FarWhile", CbReaction.WHEN_WHILE)
	_far_while.target = NodePath("OtherTimer")
	_far_while.property = "wait_time"
	_far_while.value = 3.0
	_escape = _reaction("Escape", CbReaction.WHEN_EVENT)
	_escape.target = NodePath("../../Outside")
	_escape.method = "start"
	_bad = _reaction("Bad", CbReaction.WHEN_EVENT)
	_bad.target = NodePath("../Barrel")
	_bad.method = "queue_free"


func _reaction(name: String, when: int) -> CbReaction:
	var r := CbReaction.new()
	r.name = name
	r.when = when
	r.event = "test.event"
	r.conditions = PackedStringArray(["test.on"])
	_thing.add_child(r)
	return r


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames == 2:
		_base = _thing.get_child_count()
		var before := _material.emission_energy_multiplier
		# A "while": the property set, its animation playing; its end puts things back.
		_glow.set_on(true)
		_check("while: sets the property through a material sub-path", is_equal_approx(_material.emission_energy_multiplier, 4.0))
		_check("while: plays its animation", _player.current_animation == "glow")
		_glow.set_on(true) # nothing changes
		_glow.set_on(false)
		_check("while ending: puts the property back", is_equal_approx(_material.emission_energy_multiplier, before))
		_check("while ending: plays its off animation", _player.current_animation == "fade")
		# An event: a built-in method, its animation from the start.
		_hit.fire()
		_check("event: calls the method (the timer runs)", not _timer.is_stopped())
		_check("event: plays its animation", _player.current_animation == "flash")
		# Scenes: a "while" adds one and takes it away; an event's goes by itself.
		_aura.set_on(true)
		_check("while: adds its scene", _thing.get_child_count() == _base + 1)
		_burst.fire()
		_check("event: adds its scene", _thing.get_child_count() == _base + 2)
		_aura.set_on(false)
		# As the game drives them.
		_far.fire_in(_other, _other)
		_check("act on another scene: paths from its root", not _other_timer.is_stopped())
		_far_while.set_on_in(true, _other, _other)
		_check("act on another scene: a while sets there", is_equal_approx(_other_timer.wait_time, 3.0))
		_far_while.set_on_in(false, null, null)
		_check("act on another scene: and puts it back there", is_equal_approx(_other_timer.wait_time, 10.0))
		_escape.fire_in(null, _thing)
		_check("never outside the scene it acts in", _outside_timer.is_stopped())
		_bad.fire()
	if _frames == 4:
		_check("queue_free is refused", _thing.get_node_or_null("Barrel") != null)
		_check("while ending: removes its scene", _thing.get_child_count() == _base + 1)
	if _frames > 4 and (_thing.get_child_count() == _base or _frames == 600):
		_check("event: its scene goes after its lifetime", _thing.get_child_count() == _base)
		print("reactions: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
		quit(0 if _failures == 0 else 1)
		return true
	return false
