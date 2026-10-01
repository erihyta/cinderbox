extends SceneTree
## Checks the scene guard (src/godot/cue/cue_guard.h): what a look may be made of.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_guard.gd
##
## Every scene the game ships in its moddable folders must pass. Then it builds scenes that try to
## do something (a node that reaches outside, a script, a wired signal, an animation that calls a
## method, a path that climbs out) and expects each to be refused, and a few that look close but
## are fine. Exit code 1 if anything is wrong.

var _failures := 0


func _check(what: String, ok: bool, detail := "") -> void:
	print("  %-62s %s%s" % [what, "ok" if ok else "FAILED", "" if ok or detail == "" else "  (" + detail + ")"])
	if not ok:
		_failures += 1


func _pack(root: Node) -> PackedScene:
	for node in root.find_children("*", "", true, false):
		node.owner = root
	var scene := PackedScene.new()
	scene.pack(root)
	root.free()
	return scene


func _refused(what: String, root: Node, because: String) -> void:
	var problem: String = CbDirector.check_scene(_pack(root))
	_check("refuses " + what, problem.contains(because), "said: \"%s\"" % problem)


func _accepted(what: String, root: Node) -> void:
	var scene := _pack(root)
	var problem: String = CbDirector.check_scene(scene)
	var node: Node = CbDirector.instantiate(scene)
	_check("accepts " + what, problem == "" and node != null, problem)
	if node:
		node.free()


func _with(root: Node, child: Node, child_name := "") -> Node:
	if child_name != "":
		child.name = child_name
	root.add_child(child)
	return root


func _animation_scene(track_path: String, method := "", property_track := false) -> Node:
	var root := Node3D.new()
	root.name = "Look"
	var sparks := GPUParticles3D.new()
	sparks.name = "Sparks"
	root.add_child(sparks)
	var animation := Animation.new()
	var track := animation.add_track(Animation.TYPE_VALUE if property_track else Animation.TYPE_METHOD)
	animation.track_set_path(track, NodePath(track_path))
	if property_track:
		animation.track_insert_key(track, 0.0, true)
	else:
		animation.track_insert_key(track, 0.0, {"method": method, "args": []})
	var library := AnimationLibrary.new()
	library.add_animation("go", animation)
	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	player.add_animation_library("", library)
	root.add_child(player)
	return root


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	print("the game's own scenes")
	var count := 0
	for folder in ["prefabs", "vfx", "ui", "maps", "characters/mannequin"]:
		for file in DirAccess.get_files_at("res://" + folder):
			if file.get_extension() != "tscn":
				continue
			var path := "res://%s/%s" % [folder, file]
			var problem: String = CbDirector.check_scene(load(path))
			count += 1
			if problem != "":
				_check("accepts " + path, false, problem)
	_check("all %d pass" % count, count > 10)

	print("nodes")
	_refused("an HTTPRequest", _with(Node3D.new(), HTTPRequest.new(), "Fetch"), "HTTPRequest")
	_refused("a Window", _with(Node3D.new(), Window.new(), "Popup"), "Window")
	_refused("a Camera3D", _with(Node3D.new(), Camera3D.new(), "Eye"), "Camera3D")
	_refused("a SubViewport", _with(Control.new(), SubViewport.new(), "View"), "SubViewport")
	_refused("a LinkButton", _with(Control.new(), LinkButton.new(), "Link"), "LinkButton")
	_refused("a Timer", _with(Node3D.new(), Timer.new(), "Tick"), "Timer")
	var nested := _pack(_with(Node3D.new(), HTTPRequest.new(), "Fetch"))
	_refused("a scene with one inside a scene", _with(Node3D.new(), nested.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE), "Inner"), "HTTPRequest")

	print("scripts and connections")
	var scripted := Node3D.new()
	var script := GDScript.new()
	script.source_code = "extends Node3D\n"
	script.reload()
	scripted.set_script(script)
	_refused("a node with a script", scripted, "script")
	var wired := Node3D.new()
	var button := Button.new()
	button.name = "Button"
	wired.add_child(button)
	var sparks := GPUParticles3D.new()
	sparks.name = "Sparks"
	wired.add_child(sparks)
	button.pressed.connect(sparks.restart, CONNECT_PERSIST)
	_refused("a signal wired to a method", wired, "connection")

	print("paths")
	var climbing := Node3D.new()
	var mesh := MeshInstance3D.new()
	mesh.name = "Mesh"
	mesh.skeleton = NodePath("../../..")
	climbing.add_child(mesh)
	_refused("a node path that climbs out of the scene", climbing, "leaves the scene")
	var absolute := Node3D.new()
	var mesh2 := MeshInstance3D.new()
	mesh2.name = "Mesh"
	mesh2.skeleton = NodePath("/root/Game")
	absolute.add_child(mesh2)
	_refused("a node path from the root", absolute, "starts at the root")

	print("animations")
	_refused("a method track that frees", _animation_scene("Sparks", "queue_free"), "queue_free")
	_refused("a method track that calls by name", _animation_scene("Sparks", "call_deferred"), "call_deferred")
	_refused("a track that climbs out", _animation_scene("../../Game", "stop"), "leaves the scene")
	_refused("a track from the root", _animation_scene("/root/Game", "stop"), "starts at the root")
	_refused("a track that sets a script", _animation_scene("Sparks:script", "", true), "script")

	print("what is fine")
	_accepted("a method track that restarts particles", _animation_scene("Sparks", "restart"))
	_accepted("a value track", _animation_scene("Sparks:emitting", "", true))
	var look := Node3D.new()
	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton3D"
	look.add_child(skeleton)
	var skinned := MeshInstance3D.new()
	skinned.name = "Body"
	skinned.skeleton = NodePath("../Skeleton3D")
	look.add_child(skinned)
	var light := OmniLight3D.new()
	light.name = "Glow"
	look.add_child(light)
	var sound := AudioStreamPlayer3D.new()
	sound.name = "Hum"
	look.add_child(sound)
	_accepted("meshes, a skeleton, a light, a sound, a sibling path", look)
	var reacting := Node3D.new()
	var reaction := CbReaction.new()
	reaction.name = "Glow"
	reaction.target = NodePath("../Barrel")
	reaction.method = "restart"
	reacting.add_child(reaction)
	var barrel := GPUParticles3D.new()
	barrel.name = "Barrel"
	reacting.add_child(barrel)
	_accepted("a reaction and the node it acts on", reacting)

	print("reactions")
	var director := CbDirector.new()
	root.add_child(director)
	var entity := Node3D.new()
	entity.name = "thing"
	director.add_child(entity)
	director.add_entity(entity, "prop", "")

	# A reaction calls a listed method, and does nothing for one that is not.
	var victim := Node3D.new()
	victim.name = "Victim"
	entity.add_child(victim)
	for case in [["queue_free", false], ["propagate_call", false], ["hide", true]]:
		var caller := CbReaction.new()
		caller.name = "Caller"
		caller.event = "poke"
		caller.target = NodePath("../Victim")
		caller.method = case[0]
		caller.method_args = ["queue_free"] if case[0] == "propagate_call" else []
		entity.add_child(caller)
		victim.show()
		director.cue("poke", entity, null, {})
		await process_frame
		var called: bool = not is_instance_valid(victim) or victim.is_queued_for_deletion() or not victim.visible
		_check("a reaction %s %s()" % ["calls" if case[1] else "does not call", case[0]], called == case[1])
		caller.free()

	# A reaction told to add a scene that is refused adds nothing.
	ResourceSaver.save(_pack(_with(Node3D.new(), HTTPRequest.new(), "Fetch")), "user://check_guard_bad.tscn")
	ResourceSaver.save(_pack(_with(Node3D.new(), GPUParticles3D.new(), "Sparks")), "user://check_guard_good.tscn")
	for case in [["user://check_guard_bad.tscn", 0], ["user://check_guard_good.tscn", 1]]:
		var spawner := CbReaction.new()
		spawner.name = "Spawner"
		spawner.event = "boom"
		spawner.scene = case[0]
		entity.add_child(spawner)
		var before := entity.get_child_count()
		director.cue("boom", entity, null, {})
		_check("a cue adds %s" % ("a checked scene" if case[1] == 1 else "nothing when its scene is refused"), entity.get_child_count() - before == case[1])
		spawner.free()
	DirAccess.remove_absolute(ProjectSettings.globalize_path("user://check_guard_bad.tscn"))
	DirAccess.remove_absolute(ProjectSettings.globalize_path("user://check_guard_good.tscn"))
	director.queue_free()

	print("check_guard: %s" % ("all ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
