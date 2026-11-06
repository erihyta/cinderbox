#!/usr/bin/env bash
# One CI build: configure a preset (simulation, server and tests only), build, run every test, then
# check the determinism references and leave what the cross-load job needs in OUT_DIR.
#
#   scripts/ci_check.sh <preset> <name> <out-dir>
#
# Works locally too (bash on Windows, Linux or macOS); it builds into build/<preset>.

set -euo pipefail
preset="$1"
name="$2"
out="$3"
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
build="build/$preset"
mkdir -p "$out"

echo "== toolchain"
cmake --version | head -1
ninja --version

cmake --preset "$preset" -B "$build" -DCB_BUILD_GODOT=OFF
grep -E "CMAKE_(C|CXX)_COMPILER(_ID|_VERSION)?:" "$build/CMakeCache.txt" || true
cmake --build "$build"

exe="$build/bin/cb_tests"
[[ -f "$exe.exe" ]] && exe="$exe.exe"

failed=0
if ! ctest --test-dir "$build" --output-on-failure; then
	failed=1
	# The network tests' binary too, so a crash's stack (offsets from its start) can be read.
	net="$build/bin/cb_net_tests"
	[[ -f "$net.exe" ]] && net="$net.exe"
	[[ -f "$net" ]] && cp "$net" "$out/"
fi

echo "== per-tick hashes vs tests/reference_hashes.txt"
"$exe" --compare tests/reference_hashes.txt || failed=1

anim_ref="$(head -1 tests/reference_anim_hash.txt | tr -d '\r')"
anim="$("$exe" --anim-hash | tr -d '\r')"
if [[ "$anim" == "$anim_ref" ]]; then
	echo "== anim pose hash $anim matches reference"
else
	echo "== anim pose hash $anim DIFFERS from reference $anim_ref"
	echo "== in parts: $("$exe" --anim-hash-parts | tr -d '\r')"
	failed=1
fi

"$exe" --dump "$out/hashes-$name.txt"
"$exe" --save-portable "$out/portable-$name.bin"
cp "$exe" "$out/"

if [[ $failed -ne 0 ]]; then
	echo "CHECK FAILED for $name"
	exit 1
fi
echo "check passed for $name"
