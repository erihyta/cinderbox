#!/usr/bin/env bash
# The Godot checks (docs/testing.md, "The Godot checks"), headless, against the extension a build of
# a preset with CB_BUILD_GODOT put into godot/bin (and, on Windows, into every mod's project).
#
#   scripts/godot_checks.sh <godot executable> <build dir>
#
# The build dir is for cb_server: a view file for check_object_source. Every check that needs no
# running game is run: the ones that take no arguments, the keys page and the workshop in folders of
# their own, the pack validator's hostile packs, the viewer reading a view file, and the names of
# every mod's and character's project (written there by the server's build), and every mod's committed bakes
# (its items and motions baked again: unchanged). Not here: check_menu (a window and a
# server), check_pack (packs pack_mod.ps1 makes), check_item_preview (a picture to look at).
# Exit code 1 if any check failed; each failure's output is printed.

set -uo pipefail
godot="$1"
build="$2"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
tmp="$(mktemp -d)"
# Godot on Windows wants Windows paths.
native() {
	if command -v cygpath > /dev/null; then cygpath -m "$1"; else echo "$1"; fi
}
checks="$(native "$root/godot/addons/cinderbox_maps")"

failed=()
run() {
	local name="$1"
	shift
	local started=$SECONDS
	if timeout 600 "$godot" --headless "$@" > "$tmp/$name.txt" 2>&1; then
		echo "ok    $name ($((SECONDS - started)) s)"
	else
		echo "FAIL  $name ($((SECONDS - started)) s)"
		tail -n 60 "$tmp/$name.txt"
		failed+=("$name")
	fi
}

# A fresh checkout has no import cache (.godot): the projects import what they hold first. (Godot
# crashes as it quits after a project's first import with this extension loaded, M269, not after a
# later one: the import is done by then, so its exit code is not asked.)
for project in godot server_mods/*/client characters/*/client sdk; do
	if [[ -f "$project/project.godot" ]]; then
		timeout 900 "$godot" --headless --path "$project" --import > "$tmp/import.txt" 2>&1 || true
	fi
done

for name in anim_tree fracture grips guard item_cues item_parts look map_bodies map_breakable map_shapes map_surfaces \
	motion_blocks movement predictions rules sheet_editor; do
	run "$name" --path godot --script "res://addons/cinderbox_maps/check_$name.gd"
done
run keys --path godot --script res://addons/cinderbox_maps/check_keys.gd -- "--config=$(native "$tmp/keys.cfg")"
mkdir -p "$tmp/workshop" "$tmp/cache" "$tmp/store"
run workshop --path godot --script res://addons/cinderbox_maps/check_workshop.gd -- "--workshop=$(native "$tmp/workshop")" \
	"--cache=$(native "$tmp/cache")" "--store=$(native "$tmp/store")"
run mod_validator --path godot --script res://addons/cinderbox_maps/check_mod_validator.gd

server="$build/bin/cb_server"
[[ -f "$server.exe" ]] && server="$server.exe"
if "$server" --port 7811 --fake-players 2 --bench 1 --record-view "$(native "$tmp/view.cbv")" --quiet > "$tmp/server.txt" 2>&1; then
	run object_source --path godot --script res://addons/cinderbox_maps/check_object_source.gd -- "$(native "$tmp/view.cbv")"
else
	echo "FAIL  object_source (no view file: cb_server said)"
	tail -n 20 "$tmp/server.txt"
	failed+=("object_source")
fi

# Every mod's committed bakes are what its scenes and sheets bake to now (M283: the sheet compiler,
# src/sheet, bakes a sheet's motions; a change to it that changes a shipped motion shows here).
for project in server_mods/*/client; do
	if [[ -f "$project/project.godot" ]]; then
		name="bakes-${project//\//-}"
		started=$SECONDS
		if timeout 600 "$godot" --headless --path "$project" --script "$checks/bake_items.gd" > "$tmp/$name.txt" 2>&1 &&
			timeout 600 "$godot" --headless --path "$project" --script "$checks/bake_motions.gd" >> "$tmp/$name.txt" 2>&1 &&
			git diff --exit-code --stat -- "$project/items" "$project/motions" >> "$tmp/$name.txt" 2>&1; then
			echo "ok    $name ($((SECONDS - started)) s)"
		else
			echo "FAIL  $name ($((SECONDS - started)) s)"
			tail -n 60 "$tmp/$name.txt"
			failed+=("$name")
		fi
	fi
done

# (Not the SDK's: its starters name MODNAME, which a new mod's name replaces.)
for project in server_mods/*/client characters/*/client; do
	if [[ -f "$project/project.godot" ]]; then
		run "names-${project//\//-}" --path "$project" --script "$checks/check_names.gd"
	fi
done

if [[ ${#failed[@]} -gt 0 ]]; then
	echo "Godot checks FAILED: ${failed[*]}"
	exit 1
fi
echo "Godot checks passed"
