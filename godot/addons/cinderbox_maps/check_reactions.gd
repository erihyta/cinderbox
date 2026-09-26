extends SceneTree
## Checks what CbReaction does when the game drives it:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_reactions.gd
##
## A "while" sets a property (through a material's sub-path) and puts it back, plays an animation
## and its off animation; an event calls a built-in method, restarts an animation, and adds a scene
## that goes after its lifetime; a "while" adds a scene and removes it when it ends.

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
	if _frames == 4:
		_check("while ending: removes its scene", _thing.get_child_count() == _base + 1)
	if _frames > 4 and (_thing.get_child_count() == _base or _frames == 600):
		_check("event: its scene goes after its lifetime", _thing.get_child_count() == _base)
		print("reactions: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
		quit(0 if _failures == 0 else 1)
		return true
	return false
