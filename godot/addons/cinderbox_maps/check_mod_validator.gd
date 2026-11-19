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

	var scene := _rsrc("PackedScene", [], ["resource_name", "script", "_bundled"])
	var boom_md5 := "res://vfx/boom.tscn".md5_text()
	var boom := ".godot/exported/1/export-%s-boom.scn" % boom_md5

	_hostile("a GDScript file", {"vfx/boom.gd": "extends Node"})
	_hostile("a script under the import cache", {".godot/exported/1/export-x-boom.gdc": "code"})
	_hostile("a built-in script", {"vfx/boom.tres": "[gd_resource type=\"Resource\" format=3]\n\n[sub_resource type=\"GDScript\" id=\"1\"]\nscript/source = \"extends Node\"\n\n[resource]\nscript = SubResource(\"1\")\n"})
	_hostile("a reference to the game's own script", {"prefabs/player.tscn": "[gd_scene format=3]\n\n[ext_resource type=\"Script\" path=\"res://game.gd\" id=\"1\"]\n\n[node name=\"P\" type=\"Node3D\"]\nscript = ExtResource(\"1\")\n"})
	_hostile("a redirect out of the pack", {"prefabs/player.tscn.remap": "[remap]\n\npath=\"res://boot.tscn\"\n"})
	_hostile("a compressed resource", {".godot/exported/1/export-x-player.scn": "RSCC" + "hidden".repeat(10)})
	_hostile("a file outside the moddable folders", {"project.binary": "x"})
	_hostile("a native library", {"assets/lib.dll": "MZ"})
	_hostile("a path escape", {"vfx/../../evil.tscn": "x"})
	_hostile("a script in a character folder", {"characters/critter/brain.gd": "extends Node"})
	_hostile("ozz data outside characters/", {"assets/skeleton.ozz": "ozz-skeleton"})
	_hostile("a script in an animation pack", {"anim/sneak.crouch/brain.gd": "extends Node"})
	# M261.9's finding: Godot decodes \u escapes in text resources and redirects before it reads
	# them, so a name spelled with one is not what the bytes say.
	_hostile("a built-in script whose type is escaped", {"vfx/boom.tres": "[gd_resource type=\"Resource\" format=3]\n\n[sub_resource type=\"GDScr\\u0069pt\" id=\"1\"]\nscript/source = \"extends Node\"\n\n[resource]\nscript = SubResource(\"1\")\n"})
	_hostile("a text scene", {"ui/hud.tscn": "[gd_scene format=3]\n\n[node name=\"Hud\" type=\"Control\"]\n"})
	_hostile("a redirect with an escaped path", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res:\\u002f/.godot/exported/1/export-%s-boom.scn\"\n" % boom_md5, boom: scene})
	_hostile("a redirect to a converted file not in the pack", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res://%s\"\n" % boom})
	_hostile("a converted file named for the game's own scene", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res://.godot/exported/1/export-%s-game.scn\"\n" % "res://game.tscn".md5_text(), ".godot/exported/1/export-%s-game.scn" % "res://game.tscn".md5_text(): scene})
	_hostile("a converted file no redirect names", {".godot/imported/a.wav-%s.sample" % "res://assets/a.wav".md5_text(): _rsrc("AudioStreamWAV", [], ["data"])})
	_hostile("a redirect with a key Godot would follow", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res://%s\"\ngroup_file=\"res://boot.tscn\"\n" % boom, boom: scene})
	_hostile("a redirect whose value builds an object", {"vfx/boom.tscn.remap": "[remap]

path=\"res://%s\"
metadata=Object(Node, \"name\": \"Boom\")
" % boom, boom: scene})
	_hostile("a binary scene naming a script by an empty type", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res://%s\"\n" % boom, boom: _rsrc("PackedScene", [["", "res://vfx/brain.txt", -1]], ["script"])})
	_hostile("a binary scene naming the game's script by its id", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res://%s\"\n" % boom, boom: _rsrc("PackedScene", [["Texture2D", "res://assets/glow.png", ResourceLoader.get_resource_uid("res://game.gd")]], ["script"])})
	_hostile("a binary scene naming a file outside the moddable folders", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res://%s\"\n" % boom, boom: _rsrc("PackedScene", [["PackedScene", "res://game.tscn", -1]], ["script"])})
	_hostile("a binary scene with a script class", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res://%s\"\n" % boom, boom: _rsrc("PackedScene", [], ["script"], "Boom")})
	_hostile("a big-endian binary scene", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res://%s\"\n" % boom, boom: _rsrc("PackedScene", [], ["script"], "", true)})
	_hostile("a binary scene cut short", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res://%s\"\n" % boom, boom: scene.slice(0, 40)})
	_hostile("a script in a sound", {"assets/a.wav.import": "[remap]\n\nimporter=\"wav\"\ntype=\"AudioStreamWAV\"\npath=\"res://.godot/imported/a.wav-%s.sample\"\n" % "res://assets/a.wav".md5_text(), ".godot/imported/a.wav-%s.sample" % "res://assets/a.wav".md5_text(): _rsrc("GDScript", [], ["script/source"])})

	# Things that look close but are fine.
	_friendly("a binary scene and its redirect", {"vfx/boom.tscn.remap": "[remap]\n\npath=\"res://%s\"\n" % boom, boom: scene})
	_friendly("a shader named .gdshader", {"vfx/glow.tres.remap": "[remap]\n\npath=\"res://.godot/exported/1/export-%s-glow.res\"\n" % "res://vfx/glow.tres".md5_text(), ".godot/exported/1/export-%s-glow.res" % "res://vfx/glow.tres".md5_text(): _rsrc("ShaderMaterial", [["Shader", "res://vfx/glow.gdshader", -1]], ["shader"])})
	_friendly("a character's baked data", {"characters/critter/skeleton.ozz": "ozz-skeleton", "characters/critter/anim.cfg": "skeleton = skeleton.ozz\n", "characters/critter/hitboxes.cfg": "head Head sphere 0 0 0 0 0 0 1 0.1\n"})
	_friendly("an animation pack's baked data", {"anim/sneak.crouch/skeleton.ozz": "ozz-skeleton", "anim/sneak.crouch/anim.cfg": "skeleton = skeleton.ozz\n", "anim/sneak.crouch/graph.cfg": "cinderbox_graph\t1\n"})
	_friendly("an import redirect", {"assets/sfx/a.wav.import": "[remap]\n\nimporter=\"wav\"\ntype=\"AudioStreamWAV\"\nuid=\"uid://b1\"\npath=\"res://.godot/imported/a.wav-%s.sample\"\n" % "res://assets/sfx/a.wav".md5_text(), ".godot/imported/a.wav-%s.sample" % "res://assets/sfx/a.wav".md5_text(): _rsrc("AudioStreamWAV", [], ["data"])})
	_friendly("a texture's import with its metadata", {"assets/a.png.import": "[remap]\n\nimporter=\"texture\"\ntype=\"CompressedTexture2D\"\npath.s3tc=\"res://.godot/imported/a.png-%s.s3tc.ctex\"\nmetadata={\n\"imported_formats\": [\"s3tc_bptc\"],\n\"vram_texture\": true\n}\n" % "res://assets/a.png".md5_text(), ".godot/imported/a.png-%s.s3tc.ctex" % "res://assets/a.png".md5_text(): "GST2pixels"})

	print("mod validator: %s" % ("ok" if failures == 0 else "%d failure(s)" % failures))
	quit(0 if failures == 0 else 1)


## The start of a Godot binary resource (format 6, as Godot 4.7 writes it), up to its external
## resources: what boot.gd reads. `ext` is [type, path, uid] each.
func _rsrc(type: String, ext: Array, strings: Array, script_class := "", big_endian := false) -> PackedByteArray:
	var out := StreamPeerBuffer.new()
	out.big_endian = big_endian
	out.put_data("RSRC".to_ascii_buffer())
	out.put_u32(1 if big_endian else 0)
	out.put_u32(0) # real_t is a float
	out.put_u32(4)
	out.put_u32(7)
	out.put_u32(6)
	_put_string(out, type)
	out.put_u64(0) # import md5
	out.put_u32(2 | (4 if script_class != "" else 0) | 1) # uids, a script class, named scene ids
	out.put_u64(-1)
	if script_class != "":
		_put_string(out, script_class)
	for i in 11:
		out.put_u32(0)
	out.put_u32(strings.size())
	for s in strings:
		_put_string(out, s)
	out.put_u32(ext.size())
	for e in ext:
		_put_string(out, e[0])
		_put_string(out, e[1])
		out.put_u64(e[2])
	out.put_u32(0) # no internal resources
	return out.data_array


func _put_string(out: StreamPeerBuffer, s: String) -> void:
	var bytes := s.to_utf8_buffer()
	out.put_u32(bytes.size() + 1)
	out.put_data(bytes)
	out.put_u8(0)


func _pack(files: Dictionary) -> String:
	var path := OS.get_user_data_dir().path_join("validator_check.zip")
	var zip := ZIPPacker.new()
	zip.open(path)
	for name in files:
		zip.start_file(name)
		var content = files[name]
		zip.write_file(content if content is PackedByteArray else String(content).to_utf8_buffer())
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
	print("  %-55s %s  %s" % [what, "ok  " if ok else "FAIL", detail])
	if not ok:
		failures += 1
