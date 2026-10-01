extends SceneTree
## Checks that the viewer draws from any object that hands it frames as bytes, with no peer
## extension involved: here a script that reads the packets of a view file.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_object_source.gd -- FILE.cbv
##
## FILE.cbv is a view file (cb_server --record-view FILE.cbv). Exit code 0 when everything held.


## A view source in GDScript: the records of a view file, one per take().
## (File layout: "CBVF", u32 version, then u32 size, u8 key, packet: src/present/view_file.h.)
class FileSource extends RefCounted:
	var data: PackedByteArray
	var at := 8
	var takes := 0
	var wholes_asked := 0
	var garbage_once := false

	func take(whole: bool) -> PackedByteArray:
		takes += 1
		if whole:
			wholes_asked += 1
		if garbage_once:
			# Not a packet: the viewer must ask for a whole frame next, not fall over.
			garbage_once = false
			return PackedByteArray([1, 2, 3, 4, 5, 6, 7, 8])
		while at + 5 <= data.size():
			var size := data.decode_u32(at)
			var key := data[at + 4] != 0
			var packet := data.slice(at + 5, at + 5 + size)
			at += 5 + size
			# A delta is against the record before it, which is what was handed over last. After a
			# request for a whole frame, skip on to the next record that stands alone.
			if key or not whole:
				return packet
		return PackedByteArray()


var _failures := 0


func _check(what: String, ok: bool) -> void:
	print("  %s: %s" % [what, "ok" if ok else "FAILED"])
	if not ok:
		_failures += 1


func _init() -> void:
	var args := OS.get_cmdline_user_args()
	if args.is_empty():
		print("usage: ... check_object_source.gd -- FILE.cbv")
		quit(1)
		return
	var source := FileSource.new()
	source.data = FileAccess.get_file_as_bytes(args[0])
	if source.data.size() < 8 or source.data.slice(0, 4).get_string_from_ascii() != "CBVF":
		print("%s is not a view file" % args[0])
		quit(1)
		return
	_run.call_deferred(source)


func _run(source: FileSource) -> void:
	print("check_object_source:")
	var client := CinderboxClient.new()
	root.add_child(client)
	client.set_source(source)
	_check("the viewer has a source", client.is_running())
	_check("a script source takes no input", not client.takes_input())

	for i in 30:
		await process_frame
	_check("the first request was for a whole frame", source.wholes_asked == 1)
	_check("it is asked every drawn frame", source.takes >= 25)
	_check("the source's state arrives", client.get_source_state() == "playing")
	var stats: Dictionary = client.get_stats()
	_check("the world arrives (%d entities)" % stats.get("entities", 0), int(stats.get("entities", 0)) > 0)
	_check("the map's name arrives (%s)" % client.get_map_name(), client.get_map_name() != "")
	_check("the mods' names arrive", client.get_mod_names().size() > 0)
	var tick: int = stats.get("tick", 0)

	# Bytes that are not a packet: nothing breaks, and the viewer asks for a whole frame again.
	source.garbage_once = true
	var asked := source.wholes_asked
	for i in 5:
		await process_frame
	_check("after garbage it asks for a whole frame", source.wholes_asked > asked)
	for i in 400:
		await process_frame
	stats = client.get_stats()
	_check("and plays on from the next one that stands alone (tick %d -> %d)" % [tick, stats.get("tick", 0)], int(stats.get("tick", 0)) > tick)

	client.stop()
	_check("stopped", not client.is_running())
	client.queue_free()
	print("check_object_source: %s" % ("all ok" if _failures == 0 else "%d FAILED" % _failures))
	quit(0 if _failures == 0 else 1)
