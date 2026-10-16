extends RefCounted
## Where workshop items live on this machine, and where missing ones come from.
##
## A server announces the items its mods need (a mod name and the SHA-256 of the item's pack). It
## never sends them. A player has them two ways:
##   subscribed   kept: <workshop>/<mod>/<sha256>.zip (user://workshop, or --workshop=DIR), filled
##                by subscribing (and by tools/publish_mod.ps1 on a developer's machine)
##   cached       fetched on connect for a server that needs them: user://cache/<mod>/<sha256>.zip
##                (or --cache=DIR), removed after CACHE_DAYS unused, or the oldest first past CACHE_BYTES
## Missing ones come from the store: for now a folder of the same layout (--store=DIR, or the
## "store" setting in player.cfg). Steam Workshop can replace this class later, with the rest of the
## game unchanged. Every file is checked against its hash, wherever it came from.

const CACHE_DAYS := 30
const CACHE_BYTES := 2 * 1024 * 1024 * 1024


## The cache (user://cache, or --cache=DIR).
static func cache() -> String:
	return _arg("cache", "user://cache")


## A size for people: "640 KB", "3.2 MB".
static func size_text(bytes: int) -> String:
	return "%d KB" % maxi(1, int(ceil(bytes / 1024.0))) if bytes < 1048576 else "%.1f MB" % (bytes / 1048576.0)


static func folder() -> String:
	return _arg("workshop", "user://workshop")


## The store missing items are fetched from ("": none set).
static func store_folder() -> String:
	var from_args := _arg("store", "")
	if from_args != "":
		return from_args
	var config := ConfigFile.new()
	config.load(_arg("config", "user://player.cfg"))
	return String(config.get_value("settings", "store", ""))


static func _arg(name: String, fallback: String) -> String:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--%s=" % name):
			return arg.substr(name.length() + 3)
	return fallback


static func path_of(root: String, mod: String, sha256: String) -> String:
	return root.path_join(mod).path_join(sha256 + ".zip")


## The installed pack for this exact item (subscribed, or in the cache), or "" when there is none.
static func find_item(mod: String, sha256: String) -> String:
	for root in [folder(), cache()]:
		var path := path_of(root, mod, sha256)
		if not FileAccess.file_exists(path):
			continue
		# The name is only a hint: the content has to be the item the server announced.
		if FileAccess.get_sha256(path) != sha256:
			push_warning("workshop item %s at %s does not match its hash" % [mod, path])
			continue
		if root == cache():
			touch(mod, sha256)
		return path
	return ""


## Where an item stands: "subscribed", "cached", "in_store" (it can be fetched) or "missing".
static func status(mod: String, sha256: String) -> String:
	if FileAccess.file_exists(path_of(folder(), mod, sha256)):
		return "subscribed"
	if FileAccess.file_exists(path_of(cache(), mod, sha256)):
		return "cached"
	var store := store_folder()
	if store != "" and FileAccess.file_exists(path_of(store, mod, sha256)):
		return "in_store"
	return "missing"


## How big an item is where it can be had (the store's copy for one to fetch), in bytes; 0 unknown.
static func size_of(mod: String, sha256: String) -> int:
	for root in [folder(), cache(), store_folder()]:
		if root != "" and FileAccess.file_exists(path_of(root, mod, sha256)):
			var file := FileAccess.open(path_of(root, mod, sha256), FileAccess.READ)
			return file.get_length() if file else 0
	return 0


## Keeps an item: from the cache or the store into the subscribed ones. "" or why not.
static func subscribe(mod: String, sha256: String) -> String:
	if status(mod, sha256) == "subscribed":
		return ""
	var from := path_of(cache(), mod, sha256)
	if not FileAccess.file_exists(from):
		from = path_of(store_folder(), mod, sha256) if store_folder() != "" else ""
	if from == "" or not FileAccess.file_exists(from):
		return "%s is not in the store" % mod
	var to := path_of(folder(), mod, sha256)
	DirAccess.make_dir_recursive_absolute(to.get_base_dir())
	if DirAccess.copy_absolute(from, to) != OK or FileAccess.get_sha256(to) != sha256:
		DirAccess.remove_absolute(to)
		return "%s could not be copied" % mod
	if from.begins_with(cache()):
		DirAccess.remove_absolute(from)
	return ""


## Fetches one item into the cache at once (a join without the menu). "" or why not.
static func fetch_now(mod: String, sha256: String) -> String:
	var fetch = load("res://workshop_fetch.gd").new([{"mod": mod, "sha256": sha256}])
	while not fetch.step(1 << 30):
		pass
	return fetch.error


# --- The cache ------------------------------------------------------------------------------------

static func _index() -> ConfigFile:
	var index := ConfigFile.new()
	index.load(cache().path_join("used.cfg"))
	return index


static func touch(mod: String, sha256: String) -> void:
	var index := _index()
	index.set_value("used", "%s/%s" % [mod, sha256], int(Time.get_unix_time_from_system()))
	DirAccess.make_dir_recursive_absolute(cache())
	index.save(cache().path_join("used.cfg"))


## Removes what was not used for CACHE_DAYS, then the least recently used past CACHE_BYTES. Returns
## how many items went.
static func trim_cache(now := int(Time.get_unix_time_from_system()), days := CACHE_DAYS, limit := CACHE_BYTES) -> int:
	if not DirAccess.dir_exists_absolute(cache()):
		return 0
	var index := _index()
	var entries := [] # [used, key, path, size]
	for mod in DirAccess.get_directories_at(cache()):
		for file in DirAccess.get_files_at(cache().path_join(mod)):
			if not file.ends_with(".zip"):
				continue
			var key := "%s/%s" % [mod, file.get_basename()]
			var path := cache().path_join(mod).path_join(file)
			var handle := FileAccess.open(path, FileAccess.READ)
			entries.append([int(index.get_value("used", key, 0)), key, path, handle.get_length() if handle else 0])
	entries.sort_custom(func(a, b): return a[0] > b[0]) # newest first
	var kept := 0
	var removed := 0
	for entry in entries:
		var stale: bool = now - int(entry[0]) > days * 86400
		if stale or kept + int(entry[3]) > limit:
			DirAccess.remove_absolute(entry[2])
			if index.has_section_key("used", entry[1]):
				index.erase_section_key("used", entry[1])
			removed += 1
		else:
			kept += int(entry[3])
	index.save(cache().path_join("used.cfg"))
	return removed
