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
	# Version 1 (M285, docs/sheets.md): its words, its mistakes at its own lines.
	var v1 := "cbsheet 1\n\nreact on brawl.jab.hitt\n\tas actor\n\t\tsound x\n\nreact on brawl.jab.hit\n\tat point\n"
	var v1_problems: Dictionary = names.sheet_problems(v1)
	_check("version 1: a name nobody declares, at its line", v1_problems.has(3) and String(v1_problems[3]).contains("brawl.jab.hit?"), v1_problems)
	_check("version 1: a scope with nothing under it, at its line", v1_problems.has(8) and String(v1_problems[8]).contains("nothing is under it"), v1_problems)
	_check("version 1's words are words: react, hud, create, as, known, of", names.word_kind("react") == "word" and names.word_kind("hud") == "word"
		and names.word_kind("create") == "word" and names.word_kind("as") == "word" and names.word_kind("known") == "word" and names.word_kind("of") == "word")
	_check("a good version 1 sheet says nothing", names.sheet_problems("cbsheet 1\nreact on brawl.jab.hit\n\tas actor\n\t\tsound x\n").is_empty())
	# M290's words; a line where it may not stand marked as it is typed (M291: the Sheet screen's bar
	# says the same); the formatter the screen's Format button runs.
	_check("M290's words are words: grab, toward, throw, marks, first", names.word_kind("grab") == "word" and names.word_kind("toward") == "word"
		and names.word_kind("throw") == "word" and names.word_kind("marks") == "word" and names.word_kind("first") == "word")
	var misplaced: Dictionary = names.sheet_problems("cbsheet 1\nmotion A on press brawl.guard\n\tfind sphere 2\n\t\tlink length 2\n")
	_check("a line where it may not stand, at its line, as it is typed", misplaced.has(4) and String(misplaced[4]).contains("cannot stand in a find"), misplaced)
	var formatted := names.format_sheet("cbsheet 1\nreact on brawl.jab.hit\n    as actor\n        sound x\nreact on brawl.slam.hit\n\tas actor\n\t\tsound x\n")
	_check("Format: its indentation in tabs", formatted == "cbsheet 1\nreact on brawl.jab.hit\n\tas actor\n\t\tsound x\nreact on brawl.slam.hit\n\tas actor\n\t\tsound x\n", formatted)
	_check("Format: formatted again, the same", names.format_sheet(formatted) == formatted)
	print("sheet editor: ", "ok" if _failures == 0 else "%d FAILED" % _failures)
	quit(0 if _failures == 0 else 1)
