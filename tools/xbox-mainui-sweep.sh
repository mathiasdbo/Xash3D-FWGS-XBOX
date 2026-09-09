#!/usr/bin/env bash
# Compile every C++ source file the menu module's own wscript declares, for
# the Xbox target, and report a pass/fail count.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-mainui-sweep.sh [--expect-fail N] [--expect-total N]
#
# --expect-total is written [optional] on purpose, same as
# tools/xbox-link-check.sh's --expect-undefined: omitting it only skips the
# exact-count comparison, not the gate itself - zero files attempted is
# always a hard failure regardless, since a broken glob matching nothing
# would otherwise print "0 ok, 0 failing" and --expect-fail 0 would call
# that a pass.
#
# Same reasoning as every other tools/xbox-*.sh measurement in this project:
# a one-off shell loop typed into a terminal is not a repository-backed
# measurement - the exact "not repository-backed" mistake this project's own
# review process has already caught more than once (the duplicate-symbol
# sweep-command gap, the third-party audio codec link measurement before
# tools/xbox-link-check.sh existed). This script exists so "does the menu
# module compile for Xbox" can be re-run, not just re-read.
#
# Source list and include roots: 3rdparty/mainui/wscript's own build()
# (source glob) and includes list, verbatim - '*.cpp' plus six
# subdirectories, not "everything under the directory" (mainui also ships
# tests/, vs2022/, dictgen.py and friends that are not part of the library).
# STDINT_H=<cstdint> is 3rdparty/mainui/wscript's own configure()-time
# DEFINES entry, unconditional - not platform-picked the way opus's
# VAR_ARRAYS was.
#
# MAINUI_USE_CUSTOM_FONT_RENDER/MAINUI_USE_STB: found in review, 2026-09-09
# - an earlier version of this sweep omitted both, which let font/
# WinAPIFont.cpp (real Windows GDI: CreateCompatibleDC, EnumFontFamiliesExA
# - nxdk has neither) compile down to an empty translation unit instead of
# actually being tested, since its entire body is gated on
# `defined(MAINUI_USE_CUSTOM_FONT_RENDER) && !defined(MAINUI_USE_STB)`. That
# made the sweep's pass count vacuous for that one file, not a real result.
# wscript's own configure() defines MAINUI_USE_CUSTOM_FONT_RENDER
# unconditionally (not platform-picked) and sets MAINUI_USE_STB for
# DEST_OS in [android, darwin, nswitch, psvita, emscripten] - every
# console/embedded target with no real desktop windowing API. Xbox has no
# DEST_OS assigned yet, but belongs in that exact bucket for the identical
# reason (no GDI, no window manager - divergence #9), not a guess: real
# FreeType is not a live alternative either - font/FreeTypeFont.cpp's own
# guard is `!defined(_WIN32) && defined(MAINUI_USE_FREETYPE)`, and _WIN32
# is genuinely true here, so upstream's own code already refuses FreeType
# on this branch regardless of nxdk having a real libfreetype.lib. StbFont
# is the only font backend upstream's own code leaves reachable. Both
# defines are passed below; the 82/82 result is the real configuration,
# not a bypassed one.
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
			echo "xbox-mainui-sweep: unrecognised argument: $1" >&2
			exit 1
			;;
	esac
done

MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-mainui-sweep: $MSYS2_BIN not found." >&2
	echo "  This project requires MSYS2 installed at C:\\msys64 - see tools/xbox-env.sh." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

# The miniutl patch preflight lives here, not in tools/xbox-cxx.sh - found
# in review, 2026-09-09: that script is meant to stay a generic one-
# translation-unit C++ compiler (it also has to serve the HL SDK's
# client/server later), and a mainui-only nested-submodule requirement
# does not belong forced onto every caller. This is the only script that
# actually needs it, so it owns the full preflight: existence check plus
# idempotent auto-apply, same mechanism/reasoning as the library_suffix/
# opusfile preflights tools/xbox-cc.sh and tools/xbox-cxx.sh already carry
# for their own patches.
WINLITE_H="$REPO/3rdparty/mainui/miniutl/winlite.h"
if [ ! -f "$WINLITE_H" ]; then
	echo "xbox-mainui-sweep: 3rdparty/mainui/miniutl/winlite.h does not exist." >&2
	echo "  Run:" >&2
	echo "  git submodule update --init 3rdparty/mainui" >&2
	echo "  git -C 3rdparty/mainui submodule update --init miniutl" >&2
	exit 1
elif ! grep -q "NXDK: neither header exists" "$WINLITE_H"; then
	if ! git -c core.autocrlf=false -C "$REPO/3rdparty/mainui/miniutl" apply \
		"$REPO/patches/miniutl-xbox-winlite.patch" 2>&1; then
		echo "xbox-mainui-sweep: could not apply patches/miniutl-xbox-winlite.patch" >&2
		echo "  to 3rdparty/mainui/miniutl/winlite.h - check it still applies" >&2
		echo "  cleanly against the submodule's pinned commit." >&2
		exit 1
	fi
fi

SCRATCH="${SCRATCH:-$REPO/.sweep}"
OUT_DIR="$SCRATCH/mainui"
mkdir -p "$OUT_DIR" || { echo "xbox-mainui-sweep: cannot create $OUT_DIR" >&2; exit 1; }

# Found in review, 2026-09-09: this loop only ever ADDS .obj files to
# $OUT_DIR, never removes any. A file that compiled successfully in an
# earlier run and later gets deleted, renamed, or starts failing leaves
# its stale .obj sitting here untouched - tools/xbox-link-check.sh has no
# way to tell that object apart from a genuinely current one, so a link
# using it would be measuring a source set that no longer exists. Clear
# every .obj (and the manifest below) before compiling, so what remains
# after this run is exactly and only what this run actually produced.
rm -f "$OUT_DIR"/*.obj "$OUT_DIR/manifest.txt"

CXX_FLAGS=(
	-I3rdparty/mainui -I3rdparty/mainui/miniutl -I3rdparty/mainui/font
	-I3rdparty/mainui/controls -I3rdparty/mainui/menus -I3rdparty/mainui/model
	-I3rdparty/mainui/sdk_includes/common -I3rdparty/mainui/sdk_includes/engine
	-I3rdparty/mainui/sdk_includes/public -I3rdparty/mainui/sdk_includes/pm_shared
	-DSTDINT_H='<cstdint>' -DMAINUI_USE_CUSTOM_FONT_RENDER -DMAINUI_USE_STB
)

OK=0
FAIL=0
TOTAL=0
LOG="$OUT_DIR/errors.txt"
: > "$LOG"

for f in 3rdparty/mainui/*.cpp 3rdparty/mainui/miniutl/*.cpp 3rdparty/mainui/font/*.cpp \
         3rdparty/mainui/menus/*.cpp 3rdparty/mainui/menus/dynamic/*.cpp \
         3rdparty/mainui/model/*.cpp 3rdparty/mainui/controls/*.cpp; do
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
echo "xbox-mainui-sweep: $OK ok, $FAIL failing, $TOTAL attempted   (full log: $LOG)"
echo

# Manifest: found in review, 2026-09-09, same pass as the stale-.obj
# Found in review, 2026-09-09: the glob loop's own `[ -f "$f" ] || continue`
# silently skips an unmatched glob (a moved/renamed source file, a stale
# submodule layout, a future wscript source-list drift), so a broken
# checkout that matched NOTHING would report "0 ok, 0 failing" - and
# --expect-fail 0 would accept that as a pass, indistinguishable from a
# genuine all-clean 82-file run. The exact false-clean measurement class
# this project's own review process keeps catching. Zero attempted files
# is always a hard failure, unconditionally, regardless of what flags were
# passed - there is no legitimate reason this sweep should ever compile
# nothing.
#
# This check runs BEFORE the manifest is written, not after - found in a
# second review pass on the manifest itself: an earlier version wrote
# "OK=0 FAIL=0 TOTAL=0" here before reaching this exit, and FAIL=0 reads
# as "clean" to anything only checking that one field (which is exactly
# what tools/xbox-link-check.sh's own manifest check originally did) -
# a broken, empty sweep would have certified itself as trustworthy right
# before failing. No manifest is ever written for a run this exits out
# of; tools/xbox-link-check.sh's "no manifest" refusal already covers it.
if [ "$TOTAL" -eq 0 ]; then
	echo "xbox-mainui-sweep: REGRESSION - 0 files matched any glob. The" >&2
	echo "  submodule may not be checked out, or the source list has moved -" >&2
	echo "  this is not a clean pass, it is nothing having been measured." >&2
	exit 1
fi

# Manifest, now that TOTAL>0 is already established above.
# tools/xbox-link-check.sh cannot trust "82 .obj files exist in
# $OUT_DIR" on its own - the file count says nothing about whether THIS
# run's compiles actually succeeded. Written after a run that at least
# attempted something real, so a caller can tell "never swept, or swept
# to a broken empty result" (no manifest) apart from "swept, but not
# cleanly" (manifest says FAIL>0) apart from "swept and clean"
# (FAIL=0, TOTAL>0) without re-deriving any of this itself.
echo "OK=$OK FAIL=$FAIL TOTAL=$TOTAL" > "$OUT_DIR/manifest.txt"

if [ -n "$EXPECT_TOTAL" ]; then
	if [ "$TOTAL" -eq "$EXPECT_TOTAL" ]; then
		echo "xbox-mainui-sweep: $TOTAL attempted matches --expect-total $EXPECT_TOTAL."
	else
		echo "xbox-mainui-sweep: REGRESSION - $TOTAL files attempted, expected $EXPECT_TOTAL." >&2
		echo "  The source set drifted from what --expect-total tracks - update" >&2
		echo "  the tracked count (and whichever docs cite it) only after" >&2
		echo "  confirming the new count is real, not a broken glob." >&2
		exit 1
	fi
fi

if [ -n "$EXPECT_FAIL" ]; then
	if [ "$FAIL" -eq "$EXPECT_FAIL" ]; then
		echo "xbox-mainui-sweep: $FAIL failing matches --expect-fail $EXPECT_FAIL."
	elif [ "$FAIL" -gt "$EXPECT_FAIL" ]; then
		echo "xbox-mainui-sweep: REGRESSION - $FAIL failing, expected $EXPECT_FAIL." >&2
		exit 1
	else
		echo "xbox-mainui-sweep: IMPROVEMENT - $FAIL failing, expected $EXPECT_FAIL." >&2
		echo "  Update the tracked count and whichever docs cite it." >&2
		exit 1
	fi
fi
