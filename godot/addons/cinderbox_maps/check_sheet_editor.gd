extends SceneTree
## Checks what the editor says about a rules sheet as it is typed (cue_names.h: SheetProblems,
## Completions, WordKind), through CbNames, against the project's names file.
##
##   godot --headless --path godot --script res://addons/cinderbox_maps/check_sheet_editor.gd

var _failures := 0


func _check(what: String, ok: bool, detail = "") -> void:
	print("  %-62s %s  %s" % [what, "ok" if ok else "FAIL", "" if ok else str(detail)])
	if not ok:
		_failures += 1


func _initialize() -> void:
	var names := CbNames.new()
	_check("the project has a names file", names.is_known())
	var problems: Dictionary = names.sheet_problems("on brawl.jab.hitt\n    sound res://x.wav\n\non\n")
	_check("a name nobody declares, at its line, with the closest", problems.has(1) and String(problems[1]).contains("brawl.jab.hit?"), problems)
	_check("a line that cannot be read, at its line", problems.has(4) and String(problems[4]).contains("on what"), problems)
	_check("a good sheet says nothing", names.sheet_problems("on brawl.jab.hit\n    sound res://x.wav\n").is_empty())
	var offered: PackedStringArray = names.complete("brawl.cd")
	_check("names offered: brawl.cd -> brawl.cd_dash ...", offered.has("brawl.cd_dash") and offered.has("brawl.cd_slam"), offered)
	_check("... carried.<kind> too", names.complete("carried.brawl.d").has("carried.brawl.dash_strike"))
	_check("... and the sheet's own words", names.complete("sou").has("sound") and names.complete("pre").has("predict"))
	_check("a word, a name, a number, a name nobody declares", names.word_kind("sound") == "word" and names.word_kind("brawl.cd_dash") == "name"
		and names.word_kind("3.5") == "number" and names.word_kind("brawl.nope") == "unknown" and names.word_kind("!combat.dead") == "name", [
			names.word_kind("sound"), names.word_kind("brawl.cd_dash"), names.word_kind("3.5"), names.word_kind("brawl.nope"), names.word_kind("!combat.dead")])
	print("sheet editor: ", "ok" if _failures == 0 else "%d FAILED" % _failures)
	quit(0 if _failures == 0 else 1)
