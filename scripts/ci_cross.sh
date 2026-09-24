#!/usr/bin/env bash
# The cross-load step of CI: every build's hash dump must be identical, and every build of this OS
# must continue every build's portable snapshot (from any OS) to the same final state.
#
#   scripts/ci_cross.sh <artifacts-dir> <windows|linux|macos>
#
# <artifacts-dir> holds one folder per build (det-<name>/), as written by ci_check.sh.

set -euo pipefail
art="$1"
family="$2"
failed=0

echo "== hash dumps"
first=""
for f in "$art"/det-*/hashes-*.txt; do
	if [[ -z "$first" ]]; then
		first="$f"
	elif ! cmp -s <(tr -d '\r' < "$first") <(tr -d '\r' < "$f"); then
		echo "$(basename "$f") DIFFERS from $(basename "$first")"
		failed=1
		continue
	fi
	echo "$(basename "$f") identical"
done

binaries=0
for dir in "$art"/det-"$family"-*/; do
	exe="$dir/cb_tests"
	[[ -f "$exe.exe" ]] && exe="$exe.exe"
	[[ -f "$exe" ]] || continue
	chmod +x "$exe"
	binaries=$((binaries + 1))
	for snap in "$art"/det-*/portable-*.bin; do
		printf "== %s loads %s: " "$(basename "$dir")" "$(basename "$snap")"
		"$exe" --load-portable "$snap" || failed=1
	done
done

if [[ $binaries -eq 0 ]]; then
	echo "no $family builds to run"
	failed=1
fi
if [[ $failed -ne 0 ]]; then
	echo "CROSS CHECK FAILED"
	exit 1
fi
echo "cross check passed on $family"
