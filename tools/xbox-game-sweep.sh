#!/usr/bin/env bash
# Compile every source a title's manifest (tools/xbox-game-manifest.sh)
# declares, for the Xbox target, and report a pass/fail count - the actual
# compile step X7.1's manifest tool exists to feed, same shape as
# tools/xbox-mainui-sweep.sh but reading its TU list, defines and include
# roots from the manifest instead of a hand-maintained glob and flag
# array (docs/r3d/fork-plan.md's X7.1/X7.4).
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-game-sweep.sh <hl> <server|client> [--expect-fail N] [--expect-total N]
#
# --expect-total is written [optional] on purpose, same as
# tools/xbox-mainui-sweep.sh's own: omitting it only skips the exact-count
# comparison, not the gate itself - zero files attempted is always a hard
# failure regardless (a manifest that somehow declared zero sources would
# already have been refused by tools/xbox-game-manifest.sh itself, but this
# script does not trust that indirectly - it checks its own TOTAL too).
#
# The manifest is regenerated FRESH on every run (this script calls
# tools/xbox-game-manifest.sh itself, never reads a manifest some earlier,
# possibly-stale invocation left behind) - re-reading a cached manifest
# across an SDK edit is exactly the kind of stale-measurement bug this
# project's tools keep getting caught shipping (most recently the
# manifest tool's own stale-output gap, fixed the same day this script
# was written). The manifest's own TITLE/TARGET lines are compared
# against what was asked for before anything is compiled, refusing a
# mismatch outright - X7.1's own stated design, the same guard shape
# tools/xbox-link-check.sh already enforces via its own manifest.txt.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

EXPECT_FAIL=""
EXPECT_TOTAL=""
POSITIONAL=()
while [ $# -gt 0 ]; do
	case "$1" in
	--expect-fail)
		EXPECT_FAIL="${2:?--expect-fail needs a number}"
		shift 2
		;;
	--expect-total)
		EXPECT_TOTAL="${2:?--expect-total needs a number}"
		shift 2
		;;
	-*)
		echo "xbox-game-sweep: unrecognised argument: $1" >&2
		exit 1
		;;
	*)
		POSITIONAL+=("$1")
		shift
		;;
	esac
done

if [ "${#POSITIONAL[@]}" -ne 2 ]; then
	echo "usage: xbox-game-sweep.sh <hl> <server|client> [--expect-fail N] [--expect-total N]" >&2
	exit 1
fi
TITLE="${POSITIONAL[0]}"
TARGET="${POSITIONAL[1]}"

MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-game-sweep: $MSYS2_BIN not found." >&2
	echo "  This project requires MSYS2 installed at C:\\msys64 - see tools/xbox-env.sh." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

SCRATCH="${SCRATCH:-$REPO/.sweep}"

echo "xbox-game-sweep: regenerating the $TITLE/$TARGET manifest..." >&2
MANIFEST="$(SCRATCH="$SCRATCH" bash tools/xbox-game-manifest.sh "$TITLE" "$TARGET")"
MANIFEST_STATUS=$?
if [ "$MANIFEST_STATUS" -ne 0 ] || [ -z "$MANIFEST" ] || [ ! -f "$MANIFEST" ]; then
	echo "xbox-game-sweep: tools/xbox-game-manifest.sh failed to produce a manifest" >&2
	echo "  for $TITLE/$TARGET (exit $MANIFEST_STATUS) - nothing compiled." >&2
	exit 1
fi

MTITLE=""
MTARGET=""
DEFINES=()
INCLUDES=()
SOURCES=()
while IFS=' ' read -r kind val; do
	case "$kind" in
	TITLE) MTITLE="$val" ;;
	TARGET) MTARGET="$val" ;;
	DEFINE) DEFINES+=("$val") ;;
	INCLUDE) INCLUDES+=("$val") ;;
	SOURCE) SOURCES+=("$val") ;;
	"") ;;
	*)
		echo "xbox-game-sweep: unrecognized manifest line kind '$kind' in $MANIFEST -" >&2
		echo "  this script's parsing assumptions no longer match" >&2
		echo "  tools/xbox-game-manifest.sh's output format." >&2
		exit 1
		;;
	esac
done < "$MANIFEST"

if [ "$MTITLE" != "$TITLE" ] || [ "$MTARGET" != "$TARGET" ]; then
	echo "xbox-game-sweep: manifest mismatch - asked for $TITLE/$TARGET, $MANIFEST" >&2
	echo "  declares TITLE=$MTITLE TARGET=$MTARGET. Refusing to compile against" >&2
	echo "  the wrong configuration." >&2
	exit 1
fi

if [ "${#SOURCES[@]}" -eq 0 ]; then
	echo "xbox-game-sweep: manifest $MANIFEST declares zero sources - nothing to" >&2
	echo "  compile. tools/xbox-game-manifest.sh should already have refused this;" >&2
	echo "  treating it as a hard failure here too rather than trusting that" >&2
	echo "  indirectly." >&2
	exit 1
fi

# The SDK's own code-generation flag for every Clang/GCC build, nxdk's
# included: deps/hlsdk/CMakeLists.txt:83 (-fpermissive -fno-strict-aliasing
# -Wno-invalid-offsetof). xbox-game-manifest.sh carries only its defines,
# includes and sources, so the one of the three that changes generated code
# is repeated here. It was inert while the Xbox build was -O0; since
# tools/xbox-opt.sh made it -O2, type-based alias analysis would otherwise
# apply to 25-year-old game code its own build never exposes to it.
# (-fpermissive is not a clang flag, and -Wno-invalid-offsetof only
# silences a warning.)
CXX_SDK_FLAGS=( -fno-strict-aliasing )

CXX_DEFINES=()
for d in "${DEFINES[@]}"; do
	CXX_DEFINES+=("-D$d")
done
CXX_INCLUDES=()
for i in "${INCLUDES[@]}"; do
	CXX_INCLUDES+=("-I$i")
done

OUT_DIR="$SCRATCH/game-$TITLE/$TARGET-objs"
mkdir -p "$OUT_DIR" || { echo "xbox-game-sweep: cannot create $OUT_DIR" >&2; exit 1; }

# Same reasoning as tools/xbox-mainui-sweep.sh: clear every .obj (and the
# manifest below) before compiling, so what remains after this run is
# exactly and only what THIS run actually produced - a source that
# compiled in an earlier run and later starts failing (or gets removed
# from the SDK's own manifest) must not leave a stale .obj a link step
# could still pick up. Found in Codex review: the old version ignored
# `rm -f`'s own exit status - `-f` already suppresses the common "nothing
# to remove" case (an empty or first-ever run), so a REAL failure here
# (a locked file, e.g. under the documented Windows/MSYS environment)
# meant something rm genuinely could not remove, and the old version
# compiled straight ahead as if the directory were clean anyway. Checked
# explicitly now, and re-verified empty of .obj files afterward - not
# just trusting rm's own exit code - since a stale object silently
# surviving into "this run's output" is exactly the false-clean
# measurement class a downstream link step must never inherit.
if ! rm -f "$OUT_DIR"/*.obj "$OUT_DIR/manifest.txt"; then
	echo "xbox-game-sweep: could not clear stale output in $OUT_DIR - refusing" >&2
	echo "  to compile against a directory that may still hold objects from" >&2
	echo "  an earlier run." >&2
	exit 1
fi
for stale in "$OUT_DIR"/*.obj; do
	if [ -e "$stale" ]; then
		echo "xbox-game-sweep: $stale survived cleanup - refusing to compile" >&2
		echo "  against a directory that may still hold objects from an" >&2
		echo "  earlier run." >&2
		exit 1
	fi
	break
done

OK=0
FAIL=0
TOTAL=0
LOG="$OUT_DIR/errors.txt"
: > "$LOG"

# Found while running this for real against ~107 real sources, reproduced
# twice: reopening $LOG for append (`>> "$LOG"`) once per compile - over a
# hundred separate open/close cycles on the SAME file in quick succession
# - silently lost data on this Windows/MSYS setup. Not every compile's
# output was affected the same way; the fail-count (from each compile's
# own exit status, never written through this file at all) was correct
# every time, but 3 of 5 real failures' "=== FAIL" markers - and their
# actual compiler output - never made it into the file, while the other 2
# did. A file this script's own next line trusts silently missing 60% of
# what should be in it is exactly the false-clean measurement class this
# project's tools keep getting caught by, just at the logging layer this
# time instead of the compile layer. Fixed by opening ONE file descriptor
# for the log ONCE, before the loop, and reusing it for every compile -
# no repeated open/close cycle for Windows to race - closed explicitly
# after the loop. Re-ran the same real 107-source sweep twice more after
# this fix: all 5 real failures' markers and full compiler output present
# both times.
exec 3>> "$LOG"

for f in "${SOURCES[@]}"; do
	TOTAL=$((TOTAL + 1))
	obj="$OUT_DIR/$(printf '%s' "$f" | tr '/' '_' | sed 's/\.cpp$/.obj/')"
	if tools/xbox-cxx.sh "$f" "${CXX_SDK_FLAGS[@]}" "${CXX_INCLUDES[@]}" "${CXX_DEFINES[@]}" -o "$obj" >&3 2>&3; then
		OK=$((OK + 1))
	else
		FAIL=$((FAIL + 1))
		echo "=== FAIL: $f ===" >&3
	fi
done

exec 3>&-

echo
echo "xbox-game-sweep: $TITLE/$TARGET - $OK ok, $FAIL failing, $TOTAL attempted   (full log: $LOG)"
echo

# Same false-clean guard as tools/xbox-mainui-sweep.sh: zero attempted is
# always a hard failure, unconditionally, regardless of --expect-fail -
# there is no legitimate reason this sweep should ever compile nothing
# when the manifest above already refused an empty SOURCES list.
if [ "$TOTAL" -eq 0 ]; then
	echo "xbox-game-sweep: REGRESSION - 0 files attempted despite a non-empty" >&2
	echo "  manifest. This is not a clean pass, it is nothing having been" >&2
	echo "  measured." >&2
	exit 1
fi

# All gates run BEFORE manifest.txt is (re-)written - found in Codex
# review round 2: the old order wrote OK/FAIL/TOTAL right after the
# TOTAL==0 check, then ran --expect-total/--expect-fail afterward, so a
# run one of those gates REJECTED (a source-set drift to 108 files, say,
# all compiling clean) still published a "clean-looking" manifest.txt
# (FAIL=0, TOTAL=108) before exiting nonzero - a downstream consumer
# checking only that file (the exact pattern tools/xbox-link-check.sh's
# own manifest already uses for its sibling sweeps) would accept a TU set
# the gate explicitly rejected. manifest.txt was already removed at the
# very top of this script (with the round-1 fix verifying that removal
# actually happened) and is not written again until every gate below has
# passed without exiting - so a rejected run leaves NO manifest.txt at
# all, the same "no manifest means nothing to trust" contract
# tools/xbox-game-manifest.sh's own stale-output fix already established.
if [ -n "$EXPECT_TOTAL" ]; then
	if [ "$TOTAL" -eq "$EXPECT_TOTAL" ]; then
		echo "xbox-game-sweep: $TOTAL attempted matches --expect-total $EXPECT_TOTAL."
	else
		echo "xbox-game-sweep: REGRESSION - $TOTAL files attempted, expected $EXPECT_TOTAL." >&2
		echo "  The source set drifted from what --expect-total tracks - update" >&2
		echo "  the tracked count (and whichever docs cite it) only after" >&2
		echo "  confirming the new count is real, not a broken manifest." >&2
		exit 1
	fi
fi

if [ -n "$EXPECT_FAIL" ]; then
	if [ "$FAIL" -eq "$EXPECT_FAIL" ]; then
		echo "xbox-game-sweep: $FAIL failing matches --expect-fail $EXPECT_FAIL."
	elif [ "$FAIL" -gt "$EXPECT_FAIL" ]; then
		echo "xbox-game-sweep: REGRESSION - $FAIL failing, expected $EXPECT_FAIL." >&2
		exit 1
	else
		echo "xbox-game-sweep: IMPROVEMENT - $FAIL failing, expected $EXPECT_FAIL." >&2
		echo "  Update the tracked count and whichever docs cite it." >&2
		exit 1
	fi
elif [ "$FAIL" -gt 0 ]; then
	# --expect-fail is optional (an exploratory "just show me what's
	# happening" run needs no baseline), but that must never mean a bare
	# invocation silently exits 0 while compiles are failing - a broken
	# toolchain producing 107/107 failures would otherwise look
	# identical, to anything checking only the exit code, to a clean
	# run. Any failure without an explicit --expect-fail acknowledging it
	# is fatal by default; pass the real, current count to get a clean
	# exit despite known failures.
	echo "xbox-game-sweep: $FAIL of $TOTAL failing and no --expect-fail was" >&2
	echo "  given - refusing to exit 0. Pass --expect-fail $FAIL to acknowledge" >&2
	echo "  the current known baseline, or fix the failures." >&2
	exit 1
fi

# Every applicable gate passed - only now is this run's result trustworthy
# enough to publish.
echo "OK=$OK FAIL=$FAIL TOTAL=$TOTAL" > "$OUT_DIR/manifest.txt"
