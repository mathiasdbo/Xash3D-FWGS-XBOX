#!/usr/bin/env bash
# Compile freevgui - the real, buildable-from-source VGUI1 reimplementation
# this project vendors for the Xbox target (X7.3, divergence-tracked in
# docs/r3d/divergences.md) - and report a pass/fail count.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-freevgui-sweep.sh [--expect-fail N] [--expect-total N]
#
# --expect-total/--expect-fail: same reasoning as every other
# tools/xbox-*-sweep.sh in this project (see tools/xbox-mainui-sweep.sh's
# own header) - omitting --expect-total only skips the exact-count
# comparison, not the "0 files attempted is always a hard failure" gate.
#
# Why freevgui, not Valve's own VGUI1 or FWGS's vgui_support: found running
# the first real combined server+client link (tools/xbox-game-link-check.sh
# hl --include-client, divergence #41) - `deps/hlsdk/utils/vgui` is
# headers-only plus one prebuilt win32 .lib, no buildable source anywhere.
# `3rdparty/vgui_support` (and its own nested `vgui-dev` submodule) turned
# out to be the exact same shape - Valve never publicly released VGUI1's
# real implementation source at all. freevgui is FWGS's own clean-room
# reimplementation (BSD-3-Clause, shares no code with the original or any
# other reimplementation - see 3rdparty/freevgui/README.md), written to be
# public-API-compatible with Valve's original headers (still
# `deps/hlsdk/utils/vgui/include/VGUI_*.h`, unmodified) so `cl_dll`'s own
# vgui_*.cpp files need zero changes - only the library backing those real,
# unrenamed Valve-mangled symbols changes.
#
# Source set: freevgui's own wscript builds '*.cpp', 'controls/*.cpp',
# 'platform/common/*.cpp', plus ONE of 'platform/win32/*.cpp' /
# 'platform/posix/*.cpp' picked by DEST_OS, plus 'platform/xash3d-fwgs/*.cpp'
# when FREEVGUI_XASH_SUPPORT is on (this project's own case - our engine
# already is Xash3D-FWGS, and that backend is what defines the real,
# already-looked-up-by-name `InitVGUISupportAPI` entry point
# engine/client/vgui/vgui_draw.c's own VGui_LoadProgs expects from the
# CLIENT module when vgui is statically linked into it, guarded by
# `#if defined( INTERNAL_VGUI_SUPPORT )` in
# platform/xash3d-fwgs/app.cpp - passed below).
#
# platform/win32 is not an option here: its font.cpp real-rasterises
# through Win32 GDI (CreateCompatibleDC/EnumFontFamiliesExA), which Xbox
# has none of. platform/posix/font.cpp is used instead, cherry-picked
# alone (NOT the whole platform/posix/*.cpp glob, which also has a real
# POSIX app.cpp superseded here by platform/xash3d-fwgs's own XashApp) -
# read directly: its entire body is `FontPlat_CreateSystem` returning
# `nullptr` with the comment "no system font backend here yet", already a
# real, working, upstream-shipped no-op - not a stand-in invented for this
# project. Matches this project's own established pattern (ref_null,
# GL/gl.h's two constants): correct, minimal bring-up scaffolding, with
# real font rendering a genuinely separate, later concern once a real
# Xbox renderer (X5) exists for text to be visible on at all.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

EXPECT_FAIL=""
EXPECT_TOTAL=""
while [ $# -gt 0 ]; do
	case "$1" in
		--expect-fail)
			EXPECT_FAIL="$2"
			shift 2
			;;
		--expect-total)
			EXPECT_TOTAL="$2"
			shift 2
			;;
		*)
			echo "xbox-freevgui-sweep: unrecognised argument: $1" >&2
			exit 1
			;;
	esac
done

MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-freevgui-sweep: $MSYS2_BIN not found." >&2
	echo "  This project requires MSYS2 installed at C:\\msys64 - see tools/xbox-env.sh." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

VGUI_H="$REPO/3rdparty/freevgui/vgui.h"
if [ ! -f "$VGUI_H" ]; then
	echo "xbox-freevgui-sweep: $VGUI_H does not exist." >&2
	echo "  Run: git submodule update --init 3rdparty/freevgui" >&2
	exit 1
fi

SCRATCH="${SCRATCH:-$REPO/.sweep}"
OUT_DIR="$SCRATCH/freevgui"
mkdir -p "$OUT_DIR" || { echo "xbox-freevgui-sweep: cannot create $OUT_DIR" >&2; exit 1; }

# Same reasoning as tools/xbox-mainui-sweep.sh: never let a stale .obj from
# an earlier run (a file later deleted, renamed, or newly-failing) survive
# untouched into a link that would then measure a source set nothing here
# actually just compiled.
rm -f "$OUT_DIR"/*.obj "$OUT_DIR/manifest.txt"

# engine/common/public: tools/xbox-cxx.sh's own OUR_INCLUDES is
# deliberately minimal (just compat/xbox - it is the generic one-
# translation-unit C++ tool, also used for opus/mainui, not
# engine-source-specific the way tools/xbox-cc.sh is), so the real
# engine/vgui_api.h + engine/cursor_type.h + engine/key_modifiers.h +
# common/xash3d_types.h paths platform/xash3d-fwgs/support.h depends on
# have to be passed explicitly here, same as tools/xbox-mainui-sweep.sh
# does for its own sdk_includes/ copies.
CXX_FLAGS=(
	-I3rdparty/freevgui -I3rdparty/freevgui/controls
	-I3rdparty/freevgui/platform/common -I3rdparty/freevgui/platform/xash3d-fwgs
	-Iengine -Icommon -Ipublic -I3rdparty/library_suffix/include
	-DINTERNAL_VGUI_SUPPORT
)

OK=0
FAIL=0
TOTAL=0
LOG="$OUT_DIR/errors.txt"
: > "$LOG"

for f in 3rdparty/freevgui/*.cpp 3rdparty/freevgui/controls/*.cpp \
         3rdparty/freevgui/platform/common/*.cpp \
         3rdparty/freevgui/platform/xash3d-fwgs/*.cpp \
         3rdparty/freevgui/platform/posix/font.cpp \
         compat/xbox/freevgui_app.cpp; do
	[ -f "$f" ] || continue
	TOTAL=$((TOTAL + 1))
	obj="$OUT_DIR/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.obj/')"
	if tools/xbox-cxx.sh "$f" "${CXX_FLAGS[@]}" -o "$obj" >> "$LOG" 2>&1; then
		OK=$((OK + 1))
	else
		FAIL=$((FAIL + 1))
		echo "=== FAIL: $f ===" >> "$LOG"
	fi
done

echo
echo "xbox-freevgui-sweep: $OK ok, $FAIL failing, $TOTAL attempted   (full log: $LOG)"
echo

# Same reasoning as tools/xbox-mainui-sweep.sh: zero attempted files is
# always a hard failure regardless of flags, since a broken glob matching
# nothing would otherwise print "0 ok, 0 failing" and --expect-fail 0
# would call that a pass.
if [ "$TOTAL" -eq 0 ]; then
	echo "xbox-freevgui-sweep: REGRESSION - 0 files matched any glob. The" >&2
	echo "  submodule may not be checked out, or the source list has moved -" >&2
	echo "  this is not a clean pass, it is nothing having been measured." >&2
	exit 1
fi

echo "OK=$OK FAIL=$FAIL TOTAL=$TOTAL" > "$OUT_DIR/manifest.txt"

if [ -n "$EXPECT_TOTAL" ]; then
	if [ "$TOTAL" -eq "$EXPECT_TOTAL" ]; then
		echo "xbox-freevgui-sweep: $TOTAL attempted matches --expect-total $EXPECT_TOTAL."
	else
		echo "xbox-freevgui-sweep: REGRESSION - $TOTAL files attempted, expected $EXPECT_TOTAL." >&2
		echo "  The source set drifted from what --expect-total tracks - update" >&2
		echo "  the tracked count (and whichever docs cite it) only after" >&2
		echo "  confirming the new count is real, not a broken glob." >&2
		exit 1
	fi
fi

# Found in Codex review: unlike tools/xbox-game-link-check.sh's own
# undefined-symbol handling (which refuses to report success on a first,
# unreviewed nonzero count), this script's own documented usage line
# makes --expect-fail optional - meaning a real compiler regression (or
# a broken toolchain, or nxdk itself going missing) would print a
# nonzero FAIL count and still exit 0, because the ONLY place FAIL is
# ever checked against anything was this `-n "$EXPECT_FAIL"` guard.
# Exactly the false-clean measurement class docs/r3d/lessons.md's own
# "a measuring tool that cannot report its own failure will eventually
# lie" entry exists to name. Fail closed instead: a nonzero FAIL with no
# --expect-fail given is refused, the same way tools/xbox-game-link-
# check.sh refuses to report a clean result on a first, unreviewed
# undefined-symbol count.
if [ "$FAIL" -gt 0 ] && [ -z "$EXPECT_FAIL" ]; then
	echo "xbox-freevgui-sweep: $FAIL failing, no --expect-fail given. This" >&2
	echo "  looks like a real compile regression, not something to silently" >&2
	echo "  pass - review the log above, then pass --expect-fail $FAIL once" >&2
	echo "  the count is reviewed, or fix the failures." >&2
	exit 1
fi

if [ -n "$EXPECT_FAIL" ]; then
	if [ "$FAIL" -eq "$EXPECT_FAIL" ]; then
		echo "xbox-freevgui-sweep: $FAIL failing matches --expect-fail $EXPECT_FAIL."
	elif [ "$FAIL" -gt "$EXPECT_FAIL" ]; then
		echo "xbox-freevgui-sweep: REGRESSION - $FAIL failing, expected $EXPECT_FAIL." >&2
		exit 1
	else
		echo "xbox-freevgui-sweep: IMPROVEMENT - $FAIL failing, expected $EXPECT_FAIL." >&2
		echo "  Update the tracked count and whichever docs cite it." >&2
		exit 1
	fi
fi
