#pragma once

// Sheet tests (M291; docs/sheets.md, "Tests"): a .cbtest file beside a sheet says what a player does
// with an item (what it holds, what it presses and when) and what must happen (events said, fields,
// statuses). It runs on the real server headless: the mods a cb_server runs by default, the test's
// own mod among them, read from their client folders as the net tests read them, and a bot played
// by the test's script. So it tests what players get, the mods' C++ included.
//
//   test "a press fires, an empty magazine clicks and reloads"
//   	item gun.pistol in hand
//   	set gun.rounds of item = 1
//   	press use at 0
//   	expect event item.use at 0
//   	expect gun.rounds of item == 0 at 0.1
//
// Times are seconds from when the script starts (everything placed, the item in hand, a moment to
// settle). The words are in RunSheetTests' file comment (sheet_test.cpp) and docs/sheets.md.

#include <string>
#include <vector>

namespace cb
{

struct SheetTestResult
{
	std::string name;	// the test's
	bool passed = false;
	std::vector<std::string> failures; // "file:line: why"
};

// Runs every test of a .cbtest file. `root`: the folder with server_mods/ (the file's mod is the
// one whose folder it is in). False with `error` when the file cannot be read or is not one.
bool RunSheetTests( const std::string& path, const std::string& root, std::vector<SheetTestResult>& results, std::string& error );

// The folder above `path` that holds server_mods/ (empty: none).
std::string SheetTestRoot( const std::string& path );

} // namespace cb
