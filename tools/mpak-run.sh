#!/usr/bin/env bash
# X2 W4 - the headless run (docs/r3d/mpak-loading-design.md, sections 8
# and 10): start a dedicated server with "+map <name>" from a directory
# that holds the map's baked .mpak, the measured boot floor and the loose
# files the map's manifest lists - no maps/<name>.bsp anywhere in its
# search path - let server frames tick, then run test_mapprobe
# (engine/server/sv_world.c, test builds only): SV_PointContents, a
# player-hull SV_Move, SV_TraceTexture, SV_TraceSurface and
# SV_RecursiveLightPoint at the spawn point. The X2 exit line on PC.
#
#   python waf configure -T debug --enable-tests --disable-mbedtls --dedicated
#   python waf build --targets=filesystem_stdio,xash_tests_dedicated
#   cmake --build tools/asset-studio/build --config Release
#   tools/mpak-run.sh [--hl-dir <Half-Life root>] <mapname>...
#
# The directory: gameinfo.txt with basedir == gamedir (so no valve/
# hierarchy is mounted underneath - a loose .bsp would win, divergence
# #24), the boot floor tools/mpak-diff.sh measured (gfx.wad: host.c:1137;
# dlls/hl.dll: hull bounds come from the game library, sv_pmove.c:449;
# delta.lst: net_encode.c:854), the archive, and every file the baker's
# <map>.assets.txt names, copied from the retail install (reached
# read-only through -rodir). Files the game library precaches from CODE
# rather than from entity keyvalues are NOT in the manifest; the engine
# logs "Could not load model" for each and this script reports them as a
# measurement, not a failure - the server runs without them. test_mapprobe
# writes the server's model/sound precache tables to
# <game>/<map>.precache.txt - a PARTIAL inventory of what the map needs on
# disk, measured from the running game rather than derived from the BSP,
# but not a deployment manifest: studio external sequence groups,
# client-only companion texture models and generic resources are loaded
# outside those two tables (see test_mapprobe's own comment), so a
# deployment step still has to expand dependencies from it.
#
# A control per map removes the archive: "map" must then refuse to start
# and test_mapprobe must report no active server.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

HL_DIR="${R3D_HL_DIR:-D:/SteamLibrary/steamapps/common/Half-Life}"
MAPS=()
while [ $# -gt 0 ]; do
	case "$1" in
		--hl-dir) HL_DIR="$2"; shift 2 ;;
		-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
		-*) echo "mpak-run: unknown option $1" >&2; exit 2 ;;
		*) MAPS+=("$1"); shift ;;
	esac
done
if [ ${#MAPS[@]} -eq 0 ]; then
	echo "mpak-run: no map names given (e.g. tools/mpak-run.sh c1a3)" >&2
	exit 2
fi

ENGINE="$REPO/build/engine/xash_tests_dedicated.exe"
FSDIR="$REPO/build/filesystem"
BAKER="$REPO/tools/asset-studio/build/Release/AssetStudio.exe"
for f in "$ENGINE" "$FSDIR/filesystem_stdio.dll" "$BAKER" "$HL_DIR/valve/gfx.wad" "$HL_DIR/valve/dlls/hl.dll" "$HL_DIR/valve/delta.lst"; do
	if [ ! -f "$f" ]; then
		echo "mpak-run: missing $f" >&2
		exit 1
	fi
done

WORK="$REPO/.mpak-run"
GAME="mpakrun"
if [ -d "$WORK" ] && ! find "$WORK" -mindepth 1 -delete; then
	echo "mpak-run: cannot empty $WORK" >&2
	exit 1
fi
mkdir -p "$WORK/$GAME/dlls" || exit 1
cat > "$WORK/$GAME/gameinfo.txt" <<EOF
basedir		"$GAME"
gamedir		"$GAME"
fallback_dir	"$GAME"
title		"mpak-run"
EOF
cp "$HL_DIR/valve/gfx.wad" "$WORK/$GAME/gfx.wad" && cp "$HL_DIR/valve/dlls/hl.dll" "$WORK/$GAME/dlls/hl.dll" && cp "$HL_DIR/valve/delta.lst" "$WORK/$GAME/delta.lst" || exit 1

export PATH="$FSDIR:$PATH"

# run_engine <mapname> <logname> <timeout-seconds>: +map, thirty frames of
# wait so SV_SpawnServer/SV_ActivateServer (spread over frames by
# COM_LoadLevel) and real server frames have run, then the probe, then
# quit. The engine log (-log, working directory) is kept as $WORK/<logname>.
run_engine() {
	local map="$1" logname="$2" limit="$3"
	rm -f "$WORK/engine.log"
	( cd "$WORK" && timeout "$limit" "$ENGINE" -dev 2 -log -rodir "$HL_DIR" -game "$GAME" \
		+sv_hibernate_when_empty 0 +map "$map" +wait 30 +test_mapprobe "$map.precache.txt" +quit > /dev/null 2>&1 )
	local rc=$?
	[ -f "$WORK/engine.log" ] && mv -f "$WORK/engine.log" "$WORK/$logname"
	return $rc
}

failed=0
for map in "${MAPS[@]}"; do
	bsp="$HL_DIR/valve/maps/$map.bsp"
	if [ ! -f "$bsp" ]; then
		echo "FAIL $map: $bsp not found"
		failed=$((failed + 1))
		continue
	fi

	# 1. bake; the save self-verifies and writes the manifest next to the archive
	if ! "$BAKER" --game-dir "$HL_DIR/valve" --map "$bsp" --output "$WORK/$GAME/$map.mpak" \
		--target pc --profile core > "$WORK/bake-$map.txt" 2>&1 \
		|| ! grep -q "Successfully serialized and verified" "$WORK/bake-$map.txt"; then
		echo "FAIL $map: bake did not verify (see $WORK/bake-$map.txt)"
		failed=$((failed + 1))
		continue
	fi

	# 2. the manifest's loose files, from the retail install. The bake just
	#    wrote the manifest, so a missing or unreadable one is a broken setup,
	#    not "nothing to stage"; a failed mkdir/cp is a staging error and
	#    fails the map. Files the retail install genuinely lacks are the one
	#    measured, non-fatal count (found in review: all three used to fall
	#    through to a passing verdict).
	copied=0; absent=0; stagefail=0
	manifest="$WORK/$GAME/$map.assets.txt"
	if [ ! -r "$manifest" ]; then
		echo "FAIL $map: manifest $manifest missing or unreadable after the bake"
		failed=$((failed + 1))
		continue
	fi
	while IFS= read -r asset || [ -n "$asset" ]; do
		asset="${asset//$'\r'/}"
		[ -z "$asset" ] && continue
		lower="$(printf '%s' "$asset" | tr 'A-Z' 'a-z')"
		if [ -f "$HL_DIR/valve/$asset" ]; then
			if mkdir -p "$WORK/$GAME/$(dirname "$lower")" && cp "$HL_DIR/valve/$asset" "$WORK/$GAME/$lower"; then
				copied=$((copied + 1))
			else
				echo "     staging error: $asset"
				stagefail=$((stagefail + 1))
			fi
		else
			absent=$((absent + 1))
		fi
	done < "$manifest"
	if [ $stagefail -ne 0 ]; then
		echo "FAIL $map: $stagefail manifest file(s) could not be staged"
		failed=$((failed + 1))
		continue
	fi

	# 3. the run
	run_engine "$map" "log-run-$map.txt" 180
	rc=$?
	log="$WORK/log-run-$map.txt"
	if [ $rc -ne 0 ] || ! grep -q "^test_mapprobe: OK" "$log" 2>/dev/null; then
		echo "FAIL $map: exit=$rc, no 'test_mapprobe: OK' in $log"
		grep -E "test_mapprobe|Host_Error|Sys_Error|Could not|couldn't" "$log" 2>/dev/null | head -8
		failed=$((failed + 1))
		continue
	fi
	missing=$(grep -c "Could not load model" "$log")
	# the export is part of the probe's verdict; check the file the OK
	# line vouches for actually landed, complete
	if ! [ -s "$WORK/$GAME/$map.precache.txt" ] || [ "$(tail -n 1 "$WORK/$GAME/$map.precache.txt")" != "end" ]; then
		echo "FAIL $map: precache export $WORK/$GAME/$map.precache.txt missing or incomplete"
		failed=$((failed + 1))
		continue
	fi

	# 4. control: without the archive the map must not start
	mv "$WORK/$GAME/$map.mpak" "$WORK/$map.mpak.control"
	run_engine "$map" "log-control-$map.txt" 60
	mv "$WORK/$map.mpak.control" "$WORK/$GAME/$map.mpak"
	if grep -q "^test_mapprobe: OK" "$WORK/log-control-$map.txt" 2>/dev/null \
		|| ! grep -q "test_mapprobe: no active server" "$WORK/log-control-$map.txt" 2>/dev/null; then
		echo "FAIL $map: control run without the archive still reached an active server (see $WORK/log-control-$map.txt)"
		failed=$((failed + 1))
		continue
	fi

	echo "OK   $map: $(grep -m1 '^test_mapprobe: map=' "$log" | sed 's/^test_mapprobe: //')"
	echo "     $(grep -m1 '^test_mapprobe: floor' "$log" | sed 's/^test_mapprobe: //')"
	echo "     $(grep -m1 '^test_mapprobe: precache' "$log" | sed 's/^test_mapprobe: //') - the server's tables (partial inventory, see header) are in $WORK/$GAME/$map.precache.txt"
	echo "     manifest: $copied file(s) copied, $absent absent from retail; 'Could not load model' lines: $missing"
done

if [ $failed -ne 0 ]; then
	echo "mpak-run: $failed map(s) FAILED (work dir kept: $WORK)"
	exit 1
fi
echo "mpak-run: all ${#MAPS[@]} map(s) ran headless from .mpak (work dir kept: $WORK)"
exit 0
