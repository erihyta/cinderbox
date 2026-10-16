extends RefCounted
## Fetching workshop items from the store into the cache, a piece at a time (step() each frame), so
## a menu can show how far it is (workshop.gd).

const Workshop := preload("res://workshop.gd")
const PIECE := 4 * 1024 * 1024

var items: Array = [] # [{ mod, sha256 }] still to fetch, the current one first
var done_bytes := 0
var total_bytes := 0
var error := ""
var _from: FileAccess
var _to: FileAccess
var _to_path := ""



func _init(wanted: Array) -> void:
	for item in wanted:
		items.append(item)
		total_bytes += Workshop.size_of(String(item["mod"]), String(item["sha256"]))



func finished() -> bool:
	return error != "" or items.is_empty()


## What it is doing now, for a menu.

func describe() -> String:
	if error != "":
		return error
	if items.is_empty():
		return "Everything is here."
	return "Downloading %s (%s of %s)" % [items[0]["mod"], Workshop.size_text(done_bytes), Workshop.size_text(total_bytes)]


## Copies up to `budget` bytes. True when everything is fetched (or it failed: see error).

func step(budget := PIECE) -> bool:
	while budget > 0 and not finished():
		var mod := String(items[0]["mod"])
		var sha := String(items[0]["sha256"])
		if _from == null:
			var store := Workshop.store_folder()
			_from = FileAccess.open(Workshop.path_of(store, mod, sha), FileAccess.READ) if store != "" else null
			if _from == null:
				error = "%s is not in the store (%s)." % [mod, store if store != "" else "no store is set"]
				return true
			_to_path = Workshop.path_of(Workshop.cache(), mod, sha)
			DirAccess.make_dir_recursive_absolute(_to_path.get_base_dir())
			_to = FileAccess.open(_to_path + ".part", FileAccess.WRITE)
			if _to == null:
				error = "%s could not be written to the cache." % mod
				return true
		var bytes := _from.get_buffer(mini(budget, PIECE))
		_to.store_buffer(bytes)
		budget -= bytes.size()
		done_bytes += bytes.size()
		if _from.eof_reached() or bytes.is_empty():
			_from = null
			_to.close()
			_to = null
			# What arrived has to be the item the server named.
			if FileAccess.get_sha256(_to_path + ".part") != sha:
				DirAccess.remove_absolute(_to_path + ".part")
				error = "%s from the store is not the item the server named." % mod
				return true
			DirAccess.remove_absolute(_to_path)
			DirAccess.rename_absolute(_to_path + ".part", _to_path)
			Workshop.touch(mod, sha)
			items.pop_front()
	return finished()
