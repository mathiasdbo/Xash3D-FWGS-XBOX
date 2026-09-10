#!/usr/bin/env bash
# X2 W3 - the differential proof that the engine loads a map from .mpak
# (docs/r3d/mpak-loading-design.md, section 8): for each map, load it as
# the world once from its retail .bsp and once from a directory holding
# ONLY the baked .mpak, dump every runtime BSP structure the engine built
# (test_dumpworld, engine/common/mod_bmodel.c, test builds only), and
# require the two dumps to be byte-identical. The .bsp path is the
# reference; any difference is a loader or baker bug by definition.
#
#   python waf configure -T debug --enable-tests --disable-mbedtls --dedicated
#   python waf build --targets=filesystem_stdio,xash_tests_dedicated
#   cmake --build tools/asset-studio/build --config Release
#   tools/mpak-diff.sh [--hl-dir <Half-Life root>] <mapname>...
#
# Why "a directory holding only the .mpak" and not "both files side by
# side": FS_AddGameDirectory mounts archives BEFORE the plain directory,
# so a loose maps/<name>.bsp beats its .mpak sibling (divergence #24). The
# mpak-only game directory (gameinfo.txt with basedir == gamedir, so no
# valve/ hierarchy is added underneath it) has exactly one source for
# "maps/<name>.bsp": the archive. A control run per map deletes the
# archive and requires the same command to produce NO dump - a dump that
# still appears would mean the bytes came from somewhere else, and the
# comparison would be proving nothing about .mpak at all. The retail
# install is reached through -rodir, never written to.
#
# What this does NOT prove: texture pixels. The dedicated build keeps
# names and dimensions only (Mod_LoadTextureData is client-side,
# mod_bmodel.c:2855), so the embedded miptex bytes are covered by the
# baker's own --verify and by the client run, not here.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

HL_DIR="${R3D_HL_DIR:-D:/SteamLibrary/steamapps/common/Half-Life}"
MAPS=()
while [ $# -gt 0 ]; do
	case "$1" in
		--hl-dir) HL_DIR="$2"; shift 2 ;;
		-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
		-*) echo "mpak-diff: unknown option $1" >&2; exit 2 ;;
		*) MAPS+=("$1"); shift ;;
	esac
done
if [ ${#MAPS[@]} -eq 0 ]; then
	echo "mpak-diff: no map names given (e.g. tools/mpak-diff.sh c1a3 crossfire)" >&2
	exit 2
fi

ENGINE="$REPO/build/engine/xash_tests_dedicated.exe"
FSDIR="$REPO/build/filesystem"
BAKER="$REPO/tools/asset-studio/build/Release/AssetStudio.exe"
for f in "$ENGINE" "$FSDIR/filesystem_stdio.dll" "$BAKER"; do
	if [ ! -f "$f" ]; then
		echo "mpak-diff: missing $f - see the build commands in this script's header." >&2
		exit 1
	fi
done
if [ ! -d "$HL_DIR/valve/maps" ]; then
	echo "mpak-diff: $HL_DIR/valve/maps not found (set --hl-dir or R3D_HL_DIR)." >&2
	exit 1
fi

WORK="$REPO/.mpak-diff"
GAME="mpakonly"
# Empty it rather than remove it: a shell parked inside the directory makes
# the directory itself "busy" on Windows while its contents still delete,
# and a stale dump surviving into the next run would compare as a pass.
if [ -d "$WORK" ] && ! find "$WORK" -mindepth 1 -delete; then
	echo "mpak-diff: cannot empty $WORK" >&2
	exit 1
fi
mkdir -p "$WORK/$GAME" "$WORK/valve" || exit 1
# basedir == gamedir == fallback_dir: FS_Rescan (filesystem/searchpath.c)
# then adds no hierarchy under this one, so nothing but this directory and
# its archives can answer "maps/<name>.bsp".
cat > "$WORK/$GAME/gameinfo.txt" <<EOF
basedir		"$GAME"
gamedir		"$GAME"
fallback_dir	"$GAME"
title		"mpak-diff"
EOF
# Measured, not assumed: the engine's one hard resource is gfx/conchars -
# Host_InitCommon Sys_Errors without it (engine/common/host.c:1136), and on
# a win32 dedicated build Sys_Error then waits on the console window
# forever. So "a directory holding only the archive" is really "the
# archive plus gfx.wad"; the same floor applies to W4's headless run.
if ! cp "$HL_DIR/valve/gfx.wad" "$WORK/$GAME/gfx.wad"; then
	echo "mpak-diff: cannot copy $HL_DIR/valve/gfx.wad" >&2
	exit 1
fi
# Also measured by the first differential run: hulls 1..3 of every brush
# model take their clip_mins/maxs from host.player_mins/maxs
# (Mod_MakeHulls, mod_bmodel.c:1943-1952), which only the game library's
# pfnGetHullBounds fills (sv_pmove.c:449, once SV_InitGame has loaded the
# library, sv_init.c:741) - without dlls/hl.dll the world loads with three empty hulls
# and the dump differs for a reason that has nothing to do with the
# archive. The game library is part of the floor, like gfx.wad.
mkdir -p "$WORK/$GAME/dlls"
if ! cp "$HL_DIR/valve/dlls/hl.dll" "$WORK/$GAME/dlls/hl.dll"; then
	echo "mpak-diff: cannot copy $HL_DIR/valve/dlls/hl.dll" >&2
	exit 1
fi
# And the third and last file a dedicated boot Sys_Errors without: once
# the game library is loaded, Delta_InitFields requires delta.lst
# (engine/common/net_encode.c:854). Grep-confirmed the complete list of
# file-dependent Sys_Error sites in engine/common + engine/server is
# exactly gfx/conchars and delta.lst; everything else missing (gfx/palette,
# xashcomm.lst, server.cfg, the client/menu libraries) only warns.
if ! cp "$HL_DIR/valve/delta.lst" "$WORK/$GAME/delta.lst"; then
	echo "mpak-diff: cannot copy $HL_DIR/valve/delta.lst" >&2
	exit 1
fi

export PATH="$FSDIR:$PATH"

# run_engine <game> <mapname> <dumpname> <timeout-seconds> - the dump lands
# in the game's write path ($WORK/<game>/<dumpname>); the engine log (-log
# writes engine.log in the working directory, the dedicated console window
# gets the rest, not stdout) is kept as $WORK/log-<game>-<dumpname>.
# Bounded: a load failure inside the dedicated host longjmps back to the
# frame loop and the queued +quit never runs, so the control run ends by
# this timeout (exit 124), never by an exit code.
run_engine() {
	local game="$1" map="$2" dump="$3" limit="$4"
	rm -f "$WORK/engine.log"
	( cd "$WORK" && timeout "$limit" "$ENGINE" -dev 2 -log -rodir "$HL_DIR" -game "$game" \
		+test_dumpworld "$map" "$dump" +quit > /dev/null 2>&1 )
	local rc=$?
	[ -f "$WORK/engine.log" ] && mv -f "$WORK/engine.log" "$WORK/log-$game-$dump"
	return $rc
}

# dump_complete <file> - a dump counts only if the engine wrote all of it:
# the dumper's last line is "end" (Mod_DumpWorld_f). Found in review: the
# first version accepted any non-empty file, so two runs dying at the same
# point would have compared equal prefixes and passed.
dump_complete() {
	[ -s "$1" ] && [ "$(tail -n 1 "$1")" = "end" ]
}

failed=0
for map in "${MAPS[@]}"; do
	bsp="$HL_DIR/valve/maps/$map.bsp"
	if [ ! -f "$bsp" ]; then
		echo "FAIL $map: $bsp not found"
		failed=$((failed + 1))
		continue
	fi

	# 1. bake (core profile; the save self-verifies against its sources)
	if ! "$BAKER" --game-dir "$HL_DIR/valve" --map "$bsp" --output "$WORK/$GAME/$map.mpak" \
		--target pc --profile core > "$WORK/bake-$map.txt" 2>&1 \
		|| ! grep -q "Successfully serialized and verified" "$WORK/bake-$map.txt"; then
		echo "FAIL $map: bake did not verify (see $WORK/bake-$map.txt)"
		failed=$((failed + 1))
		continue
	fi
	rm -f "$WORK/$GAME/$map.assets.txt"

	# 2. reference dump from the retail .bsp: the engine must exit cleanly
	#    through +quit and the dump must be complete
	run_engine valve "$map" "dump-bsp-$map.txt" 120
	rc=$?
	if [ $rc -ne 0 ] || ! dump_complete "$WORK/valve/dump-bsp-$map.txt"; then
		echo "FAIL $map: .bsp run exit=$rc, dump incomplete or missing (see $WORK/log-valve-dump-bsp-$map.txt)"
		failed=$((failed + 1))
		continue
	fi

	# 3. dump from the archive-only directory, same requirements
	run_engine "$GAME" "$map" "dump-mpak-$map.txt" 120
	rc=$?
	if [ $rc -ne 0 ] || ! dump_complete "$WORK/$GAME/dump-mpak-$map.txt"; then
		echo "FAIL $map: .mpak run exit=$rc, dump incomplete or missing (see $WORK/log-$GAME-dump-mpak-$map.txt)"
		failed=$((failed + 1))
		continue
	fi

	# 4. control: with the archive gone the same run must fail to load the
	#    map for exactly that reason (Mod_LoadModel's crash=true path) and
	#    produce nothing - an absent dump alone would also describe a run
	#    that died earlier for an unrelated reason
	mv "$WORK/$GAME/$map.mpak" "$WORK/$map.mpak.control"
	run_engine "$GAME" "$map" "dump-control-$map.txt" 60
	mv "$WORK/$map.mpak.control" "$WORK/$GAME/$map.mpak"
	if [ -e "$WORK/$GAME/dump-control-$map.txt" ]; then
		echo "FAIL $map: control run produced a dump WITHOUT the archive - the .mpak was not the source"
		failed=$((failed + 1))
		continue
	fi
	if ! grep -q "Could not load model maps/$map.bsp from disk" "$WORK/log-$GAME-dump-control-$map.txt" 2>/dev/null; then
		echo "FAIL $map: control run did not fail with the expected missing-map error (see $WORK/log-$GAME-dump-control-$map.txt)"
		failed=$((failed + 1))
		continue
	fi

	# 5. compare
	if cmp -s "$WORK/valve/dump-bsp-$map.txt" "$WORK/$GAME/dump-mpak-$map.txt"; then
		echo "OK   $map: .bsp and .mpak dumps identical ($(wc -c < "$WORK/valve/dump-bsp-$map.txt") bytes, $(grep -c '^surface ' "$WORK/valve/dump-bsp-$map.txt") surfaces, $(grep -c '^leaf ' "$WORK/valve/dump-bsp-$map.txt") leafs)"
	else
		echo "FAIL $map: dumps differ - first differences:"
		diff "$WORK/valve/dump-bsp-$map.txt" "$WORK/$GAME/dump-mpak-$map.txt" | head -20
		failed=$((failed + 1))
	fi
done

if [ $failed -ne 0 ]; then
	echo "mpak-diff: $failed map(s) FAILED (work dir kept: $WORK)"
	exit 1
fi
echo "mpak-diff: all ${#MAPS[@]} map(s) identical (work dir kept: $WORK)"
exit 0
