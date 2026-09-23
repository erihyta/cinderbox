#!/usr/bin/env bash
# Linux/macOS: build with Clang (and GCC on Linux) and compare per-tick hashes, optionally against a
# dump from another platform:
#
#   scripts/check_determinism.sh [reference-hashes.txt]
#
# To produce a reference on Windows: cb_tests.exe --dump hashes.txt

set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# Presets build into cinderbox-build/<project folder>/<preset>, so two checkouts never share one.
build_root="${HOME}/.cache/cinderbox-build/$(basename "$root")"
work="${build_root}/determinism-check"
mkdir -p "$work"
reference="${1:-}"

presets=(unix-clang-release)
if [[ "$(uname)" == "Linux" ]] && command -v g++ >/dev/null; then
	presets+=(unix-gcc-release)
fi

cd "$root"
for p in "${presets[@]}"; do
	echo "== building $p"
	cmake --preset "$p" >/dev/null
	cmake --build --preset "$p" >/dev/null
done

failed=0
first="${presets[0]}"
"$build_root/$first/bin/cb_tests" --dump "$work/hashes-$first.txt"

for p in "${presets[@]}"; do
	exe="$build_root/$p/bin/cb_tests"
	echo "== $p vs $first"
	"$exe" --compare "$work/hashes-$first.txt" || failed=1
	if [[ -n "$reference" ]]; then
		echo "== $p vs $reference"
		"$exe" --compare "$reference" || failed=1
	fi
	"$exe" --save-portable "$work/portable-$p.bin" >/dev/null
done

anim_ref="$(head -1 "$root/tests/reference_anim_hash.txt" | tr -d '\r')"
for p in "${presets[@]}"; do
	h="$("$build_root/$p/bin/cb_tests" --anim-hash)"
	if [[ "$h" == "$anim_ref" ]]; then
		echo "== anim pose hash $p: $h matches reference"
	else
		echo "== anim pose hash $p: $h DIFFERS from reference $anim_ref"
		failed=1
	fi
done

for from in "${presets[@]}"; do
	for to in "${presets[@]}"; do
		printf "== portable %s -> %s: " "$from" "$to"
		"$build_root/$to/bin/cb_tests" --load-portable "$work/portable-$from.bin" || failed=1
	done
done

if [[ $failed -ne 0 ]]; then
	echo "DETERMINISM CHECK FAILED"
	exit 1
fi
echo "determinism check passed for: ${presets[*]}"
