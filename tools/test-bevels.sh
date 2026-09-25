#!/usr/bin/env bash
# test-bevels.sh [map ...] - the proof behind docs/r3d/divergences.md #113.
#
# The Xbox build computes each face bevel's edge planes when a trace needs
# them (Mod_FaceBevelEdge) instead of storing them for every face at load.
# The PC test engine still stores them the upstream way, so its test_bevels
# command recomputes every stored plane with Mod_FaceBevelEdge and counts
# planes whose bytes differ. This runs it over the given maps, or every
# retail map, and fails on any mismatch.
#
# Needs build/engine/xash_tests_dedicated.exe (python waf build
# --target=xash_tests_dedicated) and a retail Half-Life install
# (R3D_HL_DIR, default D:/SteamLibrary/steamapps/common/Half-Life).
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
HL_DIR="${R3D_HL_DIR:-D:/SteamLibrary/steamapps/common/Half-Life}"
ENGINE="$REPO/build/engine/xash_tests_dedicated.exe"
FSDIR="$REPO/build/filesystem"
WORK="$REPO/.test-bevels"

for f in "$ENGINE" "$FSDIR/filesystem_stdio.dll" "$HL_DIR/valve/dlls/hl.dll"; do
	if [ ! -f "$f" ]; then
		echo "test-bevels: missing $f" >&2
		exit 1
	fi
done

MAPS=("$@")
if [ ${#MAPS[@]} -eq 0 ]; then
	for bsp in "$HL_DIR"/valve/maps/*.bsp; do
		MAPS+=("$(basename "$bsp" .bsp)")
	done
fi

mkdir -p "$WORK" || exit 1
export PATH="$FSDIR:$PATH"

failed=0
total_edges=0
for map in "${MAPS[@]}"; do
	rm -f "$WORK/engine.log"
	( cd "$WORK" && timeout 120 "$ENGINE" -dev 2 -log -rodir "$HL_DIR" -game valve \
		+test_bevels "$map" +quit > /dev/null 2>&1 )
	line="$(grep -a "^test_bevels: $map " "$WORK/engine.log" 2>/dev/null | tail -1 | tr -d '\r')"
	if [ -z "$line" ]; then
		echo "FAIL $map: no test_bevels result (see $WORK/engine.log)"
		failed=$((failed + 1))
		continue
	fi
	mismatches="$(printf '%s' "$line" | awk '{print $7}')"
	edges="$(printf '%s' "$line" | awk '{print $5}')"
	total_edges=$((total_edges + edges))
	if [ "$mismatches" != "0" ]; then
		echo "FAIL $line"
		failed=$((failed + 1))
	else
		echo "OK   $line"
	fi
done

echo "test-bevels: ${#MAPS[@]} map(s), $total_edges edge planes compared, $failed failing"
[ "$failed" -eq 0 ]
