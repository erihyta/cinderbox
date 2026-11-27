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

const MODDABLE_PREFIXES := ["prefabs/", "vfx/", "ui/", "maps/", "assets/", "characters/", "anim/", "items/", "motions/", "motion_sets/"]
## (motion_sets/ since M294: a motion set's sheets, which the server compiles; its scenes stay out of
## the pack, the editor's only.)
## Baked animation data: a character item's next to its scene under characters/<name>/, and a mod's
## animation packs under anim/<pack>/: ozz skeleton and clips (.ozz) and anim.cfg / graph.cfg /
## hitboxes.cfg; a mod's item bodies under items/ (<kind>.cfg) and its motions under motions/
## (<set>.cfg: the parts its sheets' motions name, M294), both read by the server. Read by the
## engine as data, never loaded as resources.
const DATA_PREFIXES := ["characters/", "anim/", "items/", "motions/"]
const CHARACTER_DATA_EXTENSIONS := ["ozz", "cfg"]
## Converted resources Godot writes into exported packs.
const EXPORTED_PREFIX := ".godot/exported/"
const IMPORTED_PREFIX := ".godot/imported/"
## Binary resources, checked before a pack is accepted.
const RESOURCE_EXTENSIONS := ["scn", "res"]
## Text resources: refused (Godot decodes escapes in them before it reads a name).
const TEXT_RESOURCE_EXTENSIONS := ["tscn", "tres"]
## Plain media: pixels, samples and glyphs, nothing that can name a script.
const MEDIA_EXTENSIONS := ["ctex", "stex", "sample", "oggvorbisstr", "mp3str", "fontdata",
	"png", "jpg", "jpeg", "webp", "svg", "wav", "ogg", "mp3", "ttf", "otf"]
## Sheets (.cbsheet, plain text; a CbRules node names one by its path): read as data, line by line; never code.
const RULES_EXTENSIONS := ["cbsheet"]
## Redirect files: they must point at converted resources inside the pack.
const REDIRECT_EXTENSIONS := ["remap", "import"]
## The keys an export writes into a redirect, besides `path.<feature>` (a texture's, per format).
const REDIRECT_KEYS := ["path", "dest_files", "importer", "type", "uid", "metadata"]
## Godot's binary resource format's flags (resource_format_binary.cpp).
const FORMAT_FLAG_UIDS := 2
const FORMAT_FLAG_HAS_SCRIPT_CLASS := 4
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
## (never loaded):
##   - only Godot's binary resources (what an export writes): a text resource (.tscn, .tres) is
##     refused, since Godot decodes escapes in it before it reads a type name (GDScript with its
##     "i" written as a \u escape), which no search of the bytes can follow;
##   - a binary resource is refused if it is compressed (its contents could not be checked), names a
##     script type or a script file anywhere in its bytes, has a script class, or names an external
##     resource that is not a plain file in the moddable folders (an empty type, a path elsewhere, or
##     an id that is another file of the game's: Godot follows the id before the path);
##   - a redirect (.remap, .import) may hold no backslash (an escape), only the keys an export
##     writes and only plain values (Godot's parser builds objects from `Object(...)`), and each
##     res:// in it must be a converted file of the pack named for the redirect's own file (Godot names them by the MD5 of the source path), so a pack's converted file can
##     only take the place of the converted copy of a file it may replace anyway;
##   - every converted file (.godot/exported, .godot/imported) must be named by one of those
##     redirects.
## What this cannot promise is that Godot's own resource parsers are safe against a deliberately
## malformed file: packs still come from people, so only install what you trust. Cooked items read
## by our own reader end that (ROADMAP step 6b).
static func check_mod(path: String) -> String:
	var zip := ZIPReader.new()
	if zip.open(path) != OK:
		return "not a readable zip"
	var files: Array[String] = []
	for file in zip.get_files():
		if not file.ends_with("/"):
			files.append(file)
	var in_pack := {}
	for file in files:
		in_pack[file] = true
	var named := {}
	var problem := ""
	for file in files:
		problem = _check_file(zip, file, in_pack, named)
		if problem != "":
			break
	if problem == "":
		for file in files:
			if (file.begins_with(EXPORTED_PREFIX) or file.begins_with(IMPORTED_PREFIX)) and not named.has(file):
				problem = "%s: a converted file no redirect of the pack names" % file
				break
	zip.close()
	return problem


static func _check_file(zip: ZIPReader, file: String, in_pack: Dictionary, named: Dictionary) -> String:
	var ext := file.get_extension().to_lower()
	if file.contains("..") or file.begins_with("/") or file.contains("\\"):
		return "%s: bad path" % file

	if file.begins_with(EXPORTED_PREFIX):
		if not ext in RESOURCE_EXTENSIONS:
			return "%s: unexpected converted file" % file
		return _check_resource(file, zip.read_file(file))
	if file.begins_with(IMPORTED_PREFIX):
		if ext in MEDIA_EXTENSIONS:
			return _check_media(file, zip.read_file(file))
		if ext in ["scn", "res", "mesh"]:
			return _check_resource(file, zip.read_file(file))
		return "%s: unexpected imported file" % file
	if not _in_moddable(file):
		return "%s is outside the moddable folders" % file
	if ext in REDIRECT_EXTENSIONS:
		return _check_redirect(file, zip.read_file(file), in_pack, named)
	if ext in TEXT_RESOURCE_EXTENSIONS:
		return "%s is a text resource (a pack holds the binary ones Godot's export writes)" % file
	if ext in RESOURCE_EXTENSIONS:
		return _check_resource(file, zip.read_file(file))
	if ext in MEDIA_EXTENSIONS:
		return _check_media(file, zip.read_file(file))
	if ext in RULES_EXTENSIONS:
		return ""
	for prefix in DATA_PREFIXES:
		if file.begins_with(prefix) and ext in CHARACTER_DATA_EXTENSIONS:
			return ""
	return "%s: files of this kind are not allowed" % file


static func _in_moddable(file: String) -> bool:
	for prefix in MODDABLE_PREFIXES:
		if file.begins_with(prefix):
			return true
	return false


## .remap and .import files say where the real resource is: a converted file of the pack's own,
## named for this redirect's file. Godot reads them as text with escapes decoded, so none may have
## one.
static func _check_redirect(file: String, data: PackedByteArray, in_pack: Dictionary, named: Dictionary) -> String:
	var text := data.get_string_from_utf8()
	if text.contains("\\"):
		return "%s has a backslash (an escape)" % file
	var source := "res://" + file.get_basename()
	var source_md5 := source.md5_text()
	var depth := 0
	for line in text.split("\n"):
		var entry := line.strip_edges()
		if depth == 0:
			if entry == "" or entry.begins_with(";") or entry.begins_with("["):
				continue
			var key := entry.get_slice("=", 0).strip_edges()
			if not (key in REDIRECT_KEYS or key.begins_with("path.")):
				return "%s has a key a pack does not need (%s)" % [file, key]
		# A value over several lines (a dictionary, an array) goes on until its brackets close.
		depth = maxi(0, depth + entry.count("{") + entry.count("[") - entry.count("}") - entry.count("]"))
	var parts := text.split("\"")
	# Outside its strings, a value is plain data: Godot's parser builds an object from Object(...).
	for i in range(0, parts.size(), 2):
		if parts[i].contains("("):
			return "%s builds an object (a value with parentheses)" % file
	for i in range(1, parts.size(), 2):
		var part := parts[i]
		if not part.contains(":") or part.begins_with("uid://"):
			continue
		var target := part.trim_prefix("res://")
		if not part.begins_with("res://") or not (target.begins_with(EXPORTED_PREFIX) or target.begins_with(IMPORTED_PREFIX)):
			return "%s redirects to %s" % [file, part]
		if not in_pack.has(target):
			return "%s redirects to %s, which is not in the pack" % [file, part]
		if not target.get_file().contains(source_md5):
			return "%s redirects to %s, which is not named for %s" % [file, part, source]
		named[target] = true
	return ""


## Plain media are pixels, samples and glyphs; Godot keeps some of them (a sample, a font) as binary
## resources, which are checked as resources.
static func _check_media(file: String, data: PackedByteArray) -> String:
	if data.size() >= 4 and data.slice(0, 4).get_string_from_ascii() in ["RSRC", "RSCC"]:
		return _check_resource(file, data)
	return ""


static func _check_resource(file: String, data: PackedByteArray) -> String:
	var magic := data.slice(0, 4).get_string_from_ascii() if data.size() >= 4 else ""
	if magic == "RSCC":
		return "%s is a compressed resource" % file
	if magic != "RSRC":
		return "%s is not a Godot binary resource" % file
	# Class names and paths are stored as plain strings in binary resources, so the raw bytes are
	# searched (as hex, two digits per byte, matched on byte boundaries).
	var hex := data.hex_encode()
	for marker in SCRIPT_TYPES:
		if _has_match(hex, marker, true):
			return "%s references a script (%s)" % [file, marker]
	for suffix in [".gd", ".cs", ".gdextension", ".gdc"]:
		if _has_match(hex, suffix, false):
			return "%s references a script file (*%s)" % [file, suffix]
	var problem := _check_external(data)
	return "%s %s" % [file, problem] if problem != "" else ""


## The start of a binary resource as Godot 4 writes it (ResourceFormatSaverBinary): its header, its
## string table and its external resources, which Godot loads by id first and by path second.
static func _check_external(data: PackedByteArray) -> String:
	var in_ := StreamPeerBuffer.new()
	in_.data_array = data
	in_.seek(4)
	if in_.get_u32() != 0:
		return "is big-endian"
	for i in 4:
		in_.get_u32() # real_t's size; the engine's major and minor version; the format's
	var ok := [true]
	_get_string(in_, ok) # the main resource's type
	in_.get_u64() # an import's MD5
	var flags := in_.get_u32()
	in_.get_u64() # its id
	if flags & FORMAT_FLAG_HAS_SCRIPT_CLASS:
		var script_class := _get_string(in_, ok)
		if script_class != "":
			return "has a script class (%s)" % script_class
	for i in 11:
		in_.get_u32() # reserved
	var strings := in_.get_u32()
	if not ok[0] or strings > in_.get_size():
		return "is cut short"
	for i in strings:
		_get_string(in_, ok)
		if not ok[0]:
			return "is cut short"
	var count := in_.get_u32()
	if count > in_.get_size():
		return "is cut short"
	for i in count:
		var type := _get_string(in_, ok)
		var path := _get_string(in_, ok)
		var uid: int = in_.get_u64() if flags & FORMAT_FLAG_UIDS else ResourceUID.INVALID_ID
		if not ok[0] or in_.get_position() > in_.get_size():
			return "is cut short"
		if type == "" or type in SCRIPT_TYPES:
			return "names %s as a resource of type \"%s\"" % [path, type]
		var target := path.trim_prefix("res://")
		if not path.begins_with("res://") or target.contains("..") or target.contains("\\") or not _in_moddable(target):
			return "names %s, outside the moddable folders" % path
		if uid != ResourceUID.INVALID_ID and ResourceUID.has_id(uid) and ResourceUID.get_id_path(uid) != path:
			return "names %s by the id of %s" % [path, ResourceUID.get_id_path(uid)]
	if in_.get_position() + 4 > in_.get_size():
		return "is cut short"
	return ""


## A string as the binary format keeps it: its size (with the closing zero), then UTF-8. `ok[0]`
## turns false when the size goes past the end.
static func _get_string(in_: StreamPeerBuffer, ok: Array) -> String:
	var size := in_.get_u32()
	if size > in_.get_size() - in_.get_position():
		ok[0] = false
		return ""
	return (in_.get_data(size)[1] as PackedByteArray).get_string_from_utf8()


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
