extends SceneTree
## Checks where workshop items come from (workshop.gd, workshop_fetch.gd), in folders of its own:
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_workshop.gd -- --workshop=<abs> --cache=<abs> --store=<abs>
##
## (Three empty folders: it writes made-up packs into the store and fetches, subscribes and trims
## them.) Each item is missing, then in the store, fetched a piece at a time into the cache, kept by
## subscribing; a file whose content is not its hash is refused; the cache is trimmed by age and by
## size, the least recently used first. Exit code 0 when everything held.

const Workshop := preload("res://workshop.gd")
const WorkshopFetch := preload("res://workshop_fetch.gd")

var _failures := 0


func _check(what: String, ok: bool) -> void:
	print("  %-62s %s" % [what, "ok" if ok else "FAIL"])
	if not ok:
		_failures += 1


## A made-up pack of `size` bytes in the store: its name is its hash.
func _make(mod: String, size: int, seed: int) -> String:
	var bytes := PackedByteArray()
	bytes.resize(size)
	for i in size:
		bytes[i] = (i * 31 + seed) % 251
	var hashing := HashingContext.new()
	hashing.start(HashingContext.HASH_SHA256)
	hashing.update(bytes)
	var sha := hashing.finish().hex_encode()
	var path := Workshop.path_of(Workshop.store_folder(), mod, sha)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_buffer(bytes)
	file.close()
	return sha


func _initialize() -> void:
	if Workshop.store_folder() == "" or Workshop.cache() == "user://cache" or Workshop.folder() == "user://workshop":
		print("check_workshop needs --workshop, --cache and --store (folders of its own)")
		quit(1)
		return
	var big := _make("big", 9 * 1024 * 1024, 1)
	var small := _make("small", 3000, 2)
	_check("not here, but in the store", Workshop.status("big", big) == "in_store" and Workshop.find_item("big", big) == "")
	_check("its size is the store's", Workshop.size_of("big", big) == 9 * 1024 * 1024)
	_check("a size for people", Workshop.size_text(3000) == "3 KB" and Workshop.size_text(9 * 1024 * 1024) == "9.0 MB")
	_check("not anywhere: missing", Workshop.status("nope", "0".repeat(64)) == "missing")

	var fetch = WorkshopFetch.new([{"mod": "big", "sha256": big}, {"mod": "small", "sha256": small}])
	var steps := 0
	var said := ""
	while not fetch.step():
		steps += 1
		said = fetch.describe()
	print("    fetched in %d steps; on the way: \"%s\"" % [steps + 1, said])
	_check("fetched a piece at a time", steps >= 2 and fetch.error == "" and said.begins_with("Downloading big"))
	_check("both in the cache", Workshop.status("big", big) == "cached" and Workshop.status("small", small) == "cached")
	_check("found there, with the right content", Workshop.find_item("small", small) != "")

	_check("subscribing keeps it", Workshop.subscribe("small", small) == "" and Workshop.status("small", small) == "subscribed")
	_check("... and takes it out of the cache", not FileAccess.file_exists(Workshop.path_of(Workshop.cache(), "small", small)))

	# A store file whose content is not the item it is named after.
	var fake := "f".repeat(64)
	var lie := Workshop.path_of(Workshop.store_folder(), "lie", fake)
	DirAccess.make_dir_recursive_absolute(lie.get_base_dir())
	FileAccess.open(lie, FileAccess.WRITE).store_string("not that item")
	var refused = WorkshopFetch.new([{"mod": "lie", "sha256": fake}])
	while not refused.step():
		pass
	print("    \"%s\"" % refused.error)
	_check("a file that is not its hash is refused", refused.error.contains("not the item") and Workshop.status("lie", fake) == "in_store")

	# The cache: by age, then by size (the least recently used goes first).
	var older := _make("older", 2000, 3)
	var newer := _make("newer", 2000, 4)
	for item in [["older", older], ["newer", newer]]:
		Workshop.fetch_now(item[0], item[1])
	var now := int(Time.get_unix_time_from_system())
	_check("nothing is old yet", Workshop.trim_cache(now) == 0)
	var index := ConfigFile.new()
	index.load(Workshop.cache().path_join("used.cfg"))
	index.set_value("used", "older/" + older, now - 40 * 86400)
	index.save(Workshop.cache().path_join("used.cfg"))
	_check("one unused for 40 days goes", Workshop.trim_cache(now) == 1 and Workshop.status("older", older) == "in_store")
	_check("the rest stay", Workshop.status("newer", newer) == "cached" and Workshop.status("big", big) == "cached")
	index.load(Workshop.cache().path_join("used.cfg"))
	index.set_value("used", "big/" + big, now - 86400)
	index.save(Workshop.cache().path_join("used.cfg"))
	_check("past the size limit, the least recently used goes", Workshop.trim_cache(now, 30, 1024 * 1024) == 1
		and Workshop.status("big", big) == "in_store" and Workshop.status("newer", newer) == "cached")

	print("workshop: %s" % ("ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
