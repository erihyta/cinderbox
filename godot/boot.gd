extends Node
## Loads the player's own mods, then starts the game.
##
## Two kinds of content packs exist, and both are cosmetic .zip resource packs (Godot "Export PCK/ZIP"
## with ZIP format) that replace or add files under the moddable folders below:
##   - workshop items: the look of a server mod (the pistol's HUD, effects, sounds). A server
##     announces which items it needs; game.gd finds them in the workshop and loads them on join.
##     Servers never send them.
##   - the player's own mods: installed by hand, loaded here at startup and again after a server's
##     items, so they can restyle what an item ships.
## Neither may contain code of any kind, and neither can change gameplay: the simulation lives in
## C++, and the rules in the server's mods.
##
## Player mods are searched in (later packs override earlier ones, alphabetically within a folder):
##   <game folder>/mods, user://mods, and every --mods=<dir> on the command line.

const MODDABLE_PREFIXES := ["prefabs/", "vfx/", "ui/", "maps/", "assets/", "characters/"]
## A character item's baked data, next to its scene under characters/<name>/: ozz skeleton and clips
## (.ozz) and anim.cfg / hitboxes.cfg. Read by the engine as data, never loaded as resources.
const CHARACTER_PREFIX := "characters/"
const CHARACTER_DATA_EXTENSIONS := ["ozz", "cfg"]
## Converted resources Godot writes into exported packs.
const EXPORTED_PREFIX := ".godot/exported/"
const IMPORTED_PREFIX := ".godot/imported/"
## Resources that are scanned for scripts before a pack is accepted.
const RESOURCE_EXTENSIONS := ["tscn", "scn", "tres", "res"]
## Plain media: pixels, samples and glyphs, nothing that can name a script.
const MEDIA_EXTENSIONS := ["ctex", "stex", "sample", "oggvorbisstr", "mp3str", "fontdata",
	"png", "jpg", "jpeg", "webp", "svg", "wav", "ogg", "mp3", "ttf", "otf"]
## Redirect files: they must point at converted resources inside the pack.
const REDIRECT_EXTENSIONS := ["remap", "import"]
## Types that are code, by name. Binary and text resources store type names as plain strings.
const SCRIPT_TYPES := ["GDScript", "CSharpScript", "Script", "GDExtension", "NativeScript", "VisualScript",
	"GDExtensionResourceLoader"]

## The player's mods that were accepted, in load order. game.gd loads them again after a server's
## workshop items so they keep the last word.
static var player_mods: PackedStringArray = []


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
			player_mods.append(path)
		else:
			push_warning("Mod %s could not be loaded" % file)


## Returns "" if the pack only contains cosmetic resources, otherwise the reason it is refused.
##
## An allowlist: every file must be a known kind in a known place. Resources are opened as bytes
## (never loaded) and refused if they are compressed (their contents could not be checked), name a
## script type, or reference a script file. Redirects must point back into the pack's converted
## resources. What this cannot promise is that Godot's own resource parsers are safe against a
## deliberately malformed file: packs still come from people, so only install what you trust.
static func check_mod(path: String) -> String:
	var zip := ZIPReader.new()
	if zip.open(path) != OK:
		return "not a readable zip"
	var problem := ""
	for file in zip.get_files():
		if file.ends_with("/"):
			continue
		problem = _check_file(zip, file)
		if problem != "":
			break
	zip.close()
	return problem


static func _check_file(zip: ZIPReader, file: String) -> String:
	var ext := file.get_extension().to_lower()
	if file.contains("..") or file.begins_with("/"):
		return "%s: bad path" % file
	var in_moddable := false
	for prefix in MODDABLE_PREFIXES:
		in_moddable = in_moddable or file.begins_with(prefix)

	if file.begins_with(EXPORTED_PREFIX):
		if not ext in ["scn", "res"]:
			return "%s: unexpected converted file" % file
		return _check_resource(file, zip.read_file(file))
	if file.begins_with(IMPORTED_PREFIX):
		if ext in MEDIA_EXTENSIONS:
			return ""
		if ext in ["scn", "res", "mesh"]:
			return _check_resource(file, zip.read_file(file))
		return "%s: unexpected imported file" % file
	if not in_moddable:
		return "%s is outside the moddable folders" % file
	if ext in REDIRECT_EXTENSIONS:
		return _check_redirect(file, zip.read_file(file).get_string_from_utf8())
	if ext in RESOURCE_EXTENSIONS:
		return _check_resource(file, zip.read_file(file))
	if ext in MEDIA_EXTENSIONS:
		return ""
	if file.begins_with(CHARACTER_PREFIX) and ext in CHARACTER_DATA_EXTENSIONS:
		return ""
	return "%s: files of this kind are not allowed" % file


## .remap and .import files say where the real resource is; it must be one of the pack's own.
static func _check_redirect(file: String, text: String) -> String:
	for line in text.split("\n"):
		var entry := line.strip_edges()
		if not (entry.begins_with("path") or entry.begins_with("dest_files")):
			continue
		for part in entry.split("\""):
			if part.begins_with("res://") and not (part.begins_with("res://" + EXPORTED_PREFIX) or part.begins_with("res://" + IMPORTED_PREFIX)):
				return "%s redirects to %s" % [file, part]
	return ""


static func _check_resource(file: String, data: PackedByteArray) -> String:
	if data.size() >= 4 and data[0] == 82 and data[1] == 83 and data[2] == 67 and data[3] == 67: # "RSCC"
		return "%s is a compressed resource" % file
	# Class names and paths are stored as plain strings in text and binary resources alike, so
	# the raw bytes are searched (as hex, two digits per byte, matched on byte boundaries).
	var hex := data.hex_encode()
	for marker in SCRIPT_TYPES:
		if _has_match(hex, marker, true):
			return "%s references a script (%s)" % [file, marker]
	for suffix in [".gd", ".cs", ".gdextension", ".gdc"]:
		if _has_match(hex, suffix, false):
			return "%s references a script file (*%s)" % [file, suffix]
	return ""


## Identifier characters: a match must not be part of a longer name.
static func _is_word_byte(b: int) -> bool:
	return (b >= 48 and b <= 57) or (b >= 65 and b <= 90) or (b >= 97 and b <= 122) or b == 95


## `text` occurs in the bytes with no identifier character after it (".gdshader" is not ".gd"), and
## for whole words none before it either ("Script" but not "ScriptEditor" or "GDScript").
static func _has_match(hex: String, text: String, whole_word: bool) -> bool:
	var needle := text.to_ascii_buffer().hex_encode()
	var at := hex.find(needle)
	while at != -1:
		if at % 2 == 0:
			var end := at + needle.length()
			var after := hex.substr(end, 2).hex_to_int() if end < hex.length() else 0
			var before := hex.substr(at - 2, 2).hex_to_int() if at >= 2 else 0
			if not _is_word_byte(after) and (not whole_word or not _is_word_byte(before)):
				return true
		at = hex.find(needle, at + 1)
	return false
