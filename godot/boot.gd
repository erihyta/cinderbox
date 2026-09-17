extends Node
## Loads mod packs, then starts the game.
##
## Mods are .zip resource packs (Godot "Export PCK/ZIP" with ZIP format) that replace or add files
## under the moddable folders below. They are cosmetic: they may not contain code of any kind,
## and they cannot change gameplay (the simulation lives in C++ and on the server).
##
## Search order (later packs override earlier ones, files within a folder load alphabetically):
##   <game folder>/mods, user://mods, and every --mods=<dir> on the command line.

const MODDABLE_PREFIXES := ["prefabs/", "vfx/", "ui/", "maps/", "assets/", ".godot/imported/", ".godot/exported/"]
const FORBIDDEN_EXTENSIONS := ["gd", "gdc", "cs", "gdextension", "dll", "so", "dylib", "exe", "pck", "zip", "json"]
const SCRIPT_MARKERS := ["GDScript", "CSharpScript", "type=\"Script\"", "GDExtension"]


func _ready() -> void:
	var dirs: Array[String] = [OS.get_executable_path().get_base_dir().path_join("mods"), "user://mods"]
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--mods="):
			dirs.append(arg.substr(7))
	for dir in dirs:
		_load_mods_in(dir)
	get_tree().change_scene_to_file.call_deferred("res://game.tscn")


func _load_mods_in(dir: String) -> void:
	if not DirAccess.dir_exists_absolute(dir):
		return
	var files := Array(DirAccess.get_files_at(dir))
	files.sort()
	for file in files:
		if file.get_extension().to_lower() != "zip":
			continue
		var path: String = dir.path_join(file)
		var problem := check_mod(path)
		if problem != "":
			push_warning("Mod %s rejected: %s" % [file, problem])
			continue
		if ProjectSettings.load_resource_pack(path, true):
			print("Mod loaded: ", file)
		else:
			push_warning("Mod %s could not be loaded" % file)


## Returns "" if the pack only contains cosmetic resources, otherwise the reason it is refused.
static func check_mod(path: String) -> String:
	var zip := ZIPReader.new()
	if zip.open(path) != OK:
		return "not a readable zip"
	var problem := ""
	for file in zip.get_files():
		if file.ends_with("/"):
			continue
		var ext := file.get_extension().to_lower()
		if ext in FORBIDDEN_EXTENSIONS:
			problem = "contains %s" % file
			break
		var allowed := false
		for prefix in MODDABLE_PREFIXES:
			if file.begins_with(prefix):
				allowed = true
				break
		# Exported packs keep .import/.remap files next to the replaced resources.
		if not allowed and not (ext in ["import", "remap"] and _in_moddable_folder(file)):
			problem = "%s is outside the moddable folders" % file
			break
		if ext in ["tscn", "scn", "tres", "res", "remap"] and _mentions_script(zip.read_file(file)):
			problem = "%s references a script" % file
			break
	zip.close()
	return problem


static func _in_moddable_folder(file: String) -> bool:
	for prefix in MODDABLE_PREFIXES:
		if file.begins_with(prefix):
			return true
	return false


static func _mentions_script(data: PackedByteArray) -> bool:
	# Works for text and binary resources: class names are stored as plain strings in both.
	var hex := data.hex_encode()
	for marker in SCRIPT_MARKERS:
		var needle: String = marker.to_ascii_buffer().hex_encode()
		var at := hex.find(needle)
		while at != -1:
			if at % 2 == 0:
				return true
			at = hex.find(needle, at + 1)
	return false
