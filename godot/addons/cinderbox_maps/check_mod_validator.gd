extends SceneTree
## Checks the content-pack validator (boot.gd): real packs pass, and packs that try to smuggle code
## in are refused.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_mod_validator.gd -- <pack.zip>...
##
## Every pack named on the command line must pass. The script then builds hostile packs itself and
## expects each one to be refused. The exit code is 1 if anything is wrong.

const Boot := preload("res://boot.gd")

var failures := 0


func _initialize() -> void:
	for path in OS.get_cmdline_user_args():
		var problem: String = Boot.check_mod(path)
		_expect(problem == "", "accepts %s" % path.get_file(), problem)

	_hostile("a GDScript file", {"vfx/boom.gd": "extends Node"})
	_hostile("a script under the import cache", {".godot/exported/1/export-x-boom.gdc": "code"})
	_hostile("a built-in script", {"vfx/boom.tres": "[gd_resource type=\"Resource\" format=3]\n\n[sub_resource type=\"GDScript\" id=\"1\"]\nscript/source = \"extends Node\"\n\n[resource]\nscript = SubResource(\"1\")\n"})
	_hostile("a reference to the game's own script", {"prefabs/player.tscn": "[gd_scene format=3]\n\n[ext_resource type=\"Script\" path=\"res://game.gd\" id=\"1\"]\n\n[node name=\"P\" type=\"Node3D\"]\nscript = ExtResource(\"1\")\n"})
	_hostile("a redirect out of the pack", {"prefabs/player.tscn.remap": "[remap]\n\npath=\"res://boot.tscn\"\n"})
	_hostile("a compressed resource", {".godot/exported/1/export-x-player.scn": "RSCC" + "hidden".repeat(10)})
	_hostile("a file outside the moddable folders", {"project.binary": "x"})
	_hostile("a native library", {"assets/lib.dll": "MZ"})
	_hostile("a path escape", {"vfx/../../evil.tscn": "x"})
	_hostile("a script in a character folder", {"characters/robot/brain.gd": "extends Node"})
	_hostile("ozz data outside characters/", {"assets/skeleton.ozz": "ozz-skeleton"})
	_hostile("a script in an animation pack", {"anim/sneak.crouch/brain.gd": "extends Node"})

	# Things that look close but are fine.
	_friendly("a shader named .gdshader", {"vfx/glow.tres": "[gd_resource type=\"ShaderMaterial\" format=3]\n\n[ext_resource type=\"Shader\" path=\"res://vfx/glow.gdshader\" id=\"1\"]\n\n[resource]\nshader = ExtResource(\"1\")\n"})
	_friendly("a character's baked data", {"characters/robot/skeleton.ozz": "ozz-skeleton", "characters/robot/anim.cfg": "skeleton = skeleton.ozz\n", "characters/robot/hitboxes.cfg": "head Head sphere 0 0 0 0 0 0 1 0.1\n"})
	_friendly("an animation pack's baked data", {"anim/sneak.crouch/skeleton.ozz": "ozz-skeleton", "anim/sneak.crouch/anim.cfg": "skeleton = skeleton.ozz
", "anim/sneak.crouch/graph.cfg": "cinderbox_graph	1
"})
	_friendly("an import redirect", {"assets/sfx/a.wav.import": "[remap]\n\nimporter=\"wav\"\npath=\"res://.godot/imported/a.wav-1.sample\"\n\n[deps]\n\nsource_file=\"res://assets/sfx/a.wav\"\ndest_files=[\"res://.godot/imported/a.wav-1.sample\"]\n"})

	print("mod validator: %s" % ("ok" if failures == 0 else "%d failure(s)" % failures))
	quit(0 if failures == 0 else 1)


func _pack(files: Dictionary) -> String:
	var path := OS.get_user_data_dir().path_join("validator_check.zip")
	var zip := ZIPPacker.new()
	zip.open(path)
	for name in files:
		zip.start_file(name)
		zip.write_file(String(files[name]).to_utf8_buffer())
		zip.close_file()
	zip.close()
	return path


func _hostile(what: String, files: Dictionary) -> void:
	var problem: String = Boot.check_mod(_pack(files))
	_expect(problem != "", "refuses %s" % what, problem)


func _friendly(what: String, files: Dictionary) -> void:
	var problem: String = Boot.check_mod(_pack(files))
	_expect(problem == "", "accepts %s" % what, problem)


func _expect(ok: bool, what: String, detail: String) -> void:
	print("  %-45s %s  %s" % [what, "ok  " if ok else "FAIL", detail])
	if not ok:
		failures += 1
