#!/usr/bin/env bash
# test-phs.sh [map ...] - the proof behind docs/r3d/divergences.md #117.
#
# The Xbox engine loads each map's PHS from its MPAK v13 archive, built
# offline by tools/asset-studio (PhsBuilder.h), instead of computing it
# with Mod_CalcPHS when it hosts a multiplayer game. The PC test engine's
# test_phs command loads a map, runs Mod_CalcPHS, reads the baked PHS the
# way the Xbox does (Mod_ReadBakedPHS) and counts offsets and bytes that
# differ. This bakes each map for the Xbox (or takes archives already
# baked, from R3D_MPAK_DIR), runs it over the given maps or every retail
# map, and fails on any mismatch, any map that should have a PHS but was
# baked without one, and any map where nothing was compared.
#
# Needs build/engine/xash_tests_dedicated.exe (python waf build
# --target=xash_tests_dedicated), tools/asset-studio's AssetStudio.exe
# and a retail Half-Life install (R3D_HL_DIR, default
# D:/SteamLibrary/steamapps/common/Half-Life).
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
HL_DIR="${R3D_HL_DIR:-D:/SteamLibrary/steamapps/common/Half-Life}"
ENGINE="$REPO/build/engine/xash_tests_dedicated.exe"
BAKER="$REPO/tools/asset-studio/build/Release/AssetStudio.exe"
FSDIR="$REPO/build/filesystem"
WORK="$REPO/.test-phs"
MPAK_DIR="${R3D_MPAK_DIR:-}"

for f in "$ENGINE" "$FSDIR/filesystem_stdio.dll" "$HL_DIR/valve/dlls/hl.dll"; do
	if [ ! -f "$f" ]; then
		echo "test-phs: missing $f" >&2
		exit 1
	fi
done
if [ -z "$MPAK_DIR" ] && [ ! -f "$BAKER" ]; then
	echo "test-phs: missing $BAKER (or set R3D_MPAK_DIR to baked archives)" >&2
	exit 1
fi

MAPS=("$@")
if [ ${#MAPS[@]} -eq 0 ]; then
	for bsp in "$HL_DIR"/valve/maps/*.bsp; do
		MAPS+=("$(basename "$bsp" .bsp)")
	done
fi

# The archive goes where the engine mounts map archives: the game folder.
mkdir -p "$WORK/valve" || exit 1
rm -f "$WORK/phs-sizes.txt"
export PATH="$FSDIR:$PATH"

failed=0
compared=0
for map in "${MAPS[@]}"; do
	rm -f "$WORK/valve/"*.mpak "$WORK/engine.log"
	if [ -n "$MPAK_DIR" ]; then
		if [ ! -f "$MPAK_DIR/$map.mpak" ]; then
			echo "SKIP $map: no archive in $MPAK_DIR"
			continue
		fi
		cp "$MPAK_DIR/$map.mpak" "$WORK/valve/$map.mpak" || exit 1
	elif ! "$BAKER" --game-dir "$HL_DIR" --map "maps/$map.bsp" --output "$WORK/valve/$map.mpak" \
			--target xbox --profile nv2a > "$WORK/bake.log" 2>&1; then
		# The baker refuses maps that cannot load on the Xbox
		# (Nv2aMapFit.h); those never ship, so there is nothing to prove.
		echo "SKIP $map: the baker refused it (see $WORK/bake.log)"
		continue
	else
		# the baker's own PHS line (rows, compressed and resident bytes)
		printf '%s %s\n' "$map" "$(grep -a '^PHS:' "$WORK/bake.log" | tr -d '\r')" >> "$WORK/phs-sizes.txt"
	fi
	( cd "$WORK" && timeout 120 "$ENGINE" -dev 2 -log -rodir "$HL_DIR" -game valve \
		+test_phs "$map" +quit > /dev/null 2>&1 )
	line="$(grep -a "^test_phs: $map " "$WORK/engine.log" 2>/dev/null | tail -1 | tr -d '\r')"
	case "$line" in
	"")
		echo "FAIL $map: no test_phs result (see $WORK/engine.log)"
		failed=$((failed + 1)) ;;
	*" no-vis")
		echo "OK   $line (no PHS to build)" ;;
	*" unbaked")
		echo "FAIL $line"
		failed=$((failed + 1)) ;;
	*" 0 mismatches")
		echo "OK   $line"
		compared=$((compared + 1)) ;;
	*)
		echo "FAIL $line"
		failed=$((failed + 1)) ;;
	esac
done

echo "test-phs: ${#MAPS[@]} map(s), $compared compared, $failed failing"
[ "$failed" -eq 0 ] && [ "$compared" -gt 0 ]
