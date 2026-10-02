extends SceneTree
## Checks companion tracks (CbCompanionPlayer): the non-bone tracks of a character's animations,
## played in step with the simulation's clip times.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_companion.gd
##
## - a value key applies at its time;
## - a method key fires once, even when a rollback steps back over it and plays it again;
## - a looping clip fires its key every cycle;
## - a channel nothing plays, or one playing a clip without companion tracks, returns to RESET.
## Exit code 1 if anything is wrong.

var failures := 0
var _frames := 0
var _companion: CbCompanionPlayer
var _light: OmniLight3D
var _counter: Node


func _initialize() -> void:
	# Animation players only fire method keys once they have been through a frame, so the checks
	# run from _process.
	var root := Node3D.new()
	get_root().add_child(root)
	var light := OmniLight3D.new()
	light.name = "Light"
	light.visible = false
	root.add_child(light)
	var counter := Node.new()
	counter.name = "Counter"
	var script := GDScript.new()
	script.source_code = "extends Node\nvar hits := 0\nfunc hit() -> void:\n\thits += 1\n"
	script.reload()
	counter.set_script(script)
	root.add_child(counter)

	var library := AnimationLibrary.new()
	var swing := Animation.new()
	swing.length = 1.0
	var vis := swing.add_track(Animation.TYPE_VALUE)
	swing.track_set_path(vis, NodePath("Light:visible"))
	swing.value_track_set_update_mode(vis, Animation.UPDATE_DISCRETE)
	swing.track_insert_key(vis, 0.0, false)
	swing.track_insert_key(vis, 0.3, true)
	swing.track_insert_key(vis, 0.7, false)
	var call := swing.add_track(Animation.TYPE_METHOD)
	swing.track_set_path(call, NodePath("Counter"))
	swing.track_insert_key(call, 0.5, {"method": "hit", "args": []})
	library.add_animation("swing", swing)

	var loop := Animation.new()
	loop.length = 0.5
	loop.loop_mode = Animation.LOOP_LINEAR
	var loop_call := loop.add_track(Animation.TYPE_METHOD)
	loop.track_set_path(loop_call, NodePath("Counter"))
	loop.track_insert_key(loop_call, 0.25, {"method": "hit", "args": []})
	library.add_animation("loop", loop)

	var reset := Animation.new()
	reset.length = 0.001
	var reset_vis := reset.add_track(Animation.TYPE_VALUE)
	reset.track_set_path(reset_vis, NodePath("Light:visible"))
	reset.value_track_set_update_mode(reset_vis, Animation.UPDATE_DISCRETE)
	reset.track_insert_key(reset_vis, 0.0, false)
	library.add_animation("RESET", reset)

	_companion = CbCompanionPlayer.new()
	root.add_child(_companion)
	_companion.setup(library, root)
	_light = light
	_counter = counter


func _process(_delta: float) -> bool:
	_frames += 1
	if _frames < 2:
		return false
	_run(_companion, _light, _counter)
	return true


func _run(companion: CbCompanionPlayer, light: OmniLight3D, counter: Node) -> void:
	var t := 0.0

	# The swing at 60 Hz, with a rollback at 0.55 s that steps back to 0.45 s and plays forward again.
	var times: Array[float] = []
	while t <= 0.55:
		times.append(t)
		t += 1.0 / 60.0
	t = 0.45
	while t <= 1.0:
		times.append(t)
		t += 1.0 / 60.0
	var seen_on_at := -1.0
	var seen_off_after := false
	for time in times:
		_frame(companion, [[0, "swing", time, false]])
		if light.visible and seen_on_at < 0.0:
			seen_on_at = time
		if time > 0.75 and not light.visible:
			seen_off_after = true
	_expect(abs(seen_on_at - 0.3) < 0.02, "value key applies at its time", "light on at %.3f s" % seen_on_at)
	_expect(seen_off_after, "a later value key applies", "")
	_expect(counter.hits == 1, "a method key fires once through a rollback", "%d hits" % counter.hits)

	# A long jump (a join, a reconnect) applies values without firing.
	counter.hits = 0
	_frame(companion, [[0, "swing", 0.1, false]])
	_frame(companion, [[0, "swing", 0.9, false]])
	_expect(counter.hits == 0, "a long jump fires nothing", "%d hits" % counter.hits)

	# A loop fires every cycle: two seconds of a half-second loop.
	counter.hits = 0
	t = 0.0
	for i in 120:
		_frame(companion, [[1, "loop", fmod(t, 0.5), true]])
		t += 1.0 / 60.0
	_expect(counter.hits == 4, "a looping key fires every cycle", "%d hits" % counter.hits)

	# Channels with nothing to play go back to RESET.
	_frame(companion, [[0, "swing", 0.4, false]])
	_expect(light.visible, "(the light is on mid-swing)", "")
	_frame(companion, [])
	_expect(not light.visible and companion.get_channel_clip(0) == "", "an unplayed channel resets", "")
	_frame(companion, [[0, "swing", 0.4, false]])
	_frame(companion, [[0, "walk", 0.2, true]])
	_expect(not light.visible, "a clip without companion tracks resets", "")

	print("companion tracks: %s" % ("ok" if failures == 0 else "%d failure(s)" % failures))
	quit(0 if failures == 0 else 1)


func _frame(companion: CbCompanionPlayer, clips: Array) -> void:
	companion.begin_frame()
	for c in clips:
		companion.play_at(c[0], c[1], c[2], c[3])
	companion.end_frame()


func _expect(ok: bool, what: String, detail: String) -> void:
	print("  %-45s %s  %s" % [what, "ok  " if ok else "FAIL", detail])
	if not ok:
		failures += 1
