#!/usr/bin/env bash
# Attempt a real Xbox link across the main engine plus the third-party
# audio codec libraries this project has verified compile for Xbox, and
# gate on duplicate/undefined symbol counts.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-sweep.sh engine filesystem public common compat 3rdparty/library_suffix/src
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-link-check.sh --expect-undefined N
#
# --expect-undefined is written [optional] nowhere in this file on purpose:
# omitting it only skips the regression/improvement COMPARISON, not the
# gate itself - any undefined symbol at all with no baseline given is a
# hard failure (see the check below), because "undefined symbols exist,
# nobody said how many were expected" is not a passing result to report.
#
# Two steps, on purpose: this script does NOT recompile the main engine -
# that is what tools/xbox-sweep.sh already does, correctly, with its own
# hardening (--expect-fail, per-directory empty checks, preflight
# detection). Duplicating that here would be a second copy of logic that
# has already needed fixing more than once. This script REUSES whatever
# tools/xbox-sweep.sh most recently left in $SCRATCH/*.obj, compiles the
# third-party libraries fresh every run (they are not part of any sweep
# yet - see "Third-party scope" below for why), and links the two
# together.
#
# Found in review, 2026-09-08 (X1): an earlier version of this
# measurement was done entirely by hand - one-off shell loops, not a
# script - and claimed results the repository itself could not reproduce.
# That is the exact "not repository-backed" failure class this project's
# own review process has already caught once (tools/xbox-check-nxdk-cc-
# drift.sh, CHANGELOG's own X1 entries). This script exists so the same
# measurement can be re-run, not just re-read.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

# Same reasoning as every other tools/xbox-*.sh script this project has
# written: whatever invoked this bash process may have Git Bash's
# /usr/bin ahead of MSYS2's on PATH regardless of MSYSTEM, and mixing
# binaries from different msys builds has silently corrupted output here
# before (tools/xbox-check-nxdk-cc-drift.sh's own history). Pin it.
MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-link-check: $MSYS2_BIN not found." >&2
	echo "  This project requires MSYS2 installed at C:\\msys64 - see tools/xbox-env.sh." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

EXPECT_UNDEFINED=""
while [ $# -gt 0 ]; do
	case "$1" in
		--expect-undefined)
			EXPECT_UNDEFINED="$2"
			shift 2
			;;
		*)
			echo "xbox-link-check: unrecognised argument: $1" >&2
			exit 1
			;;
	esac
done

SCRATCH="${SCRATCH:-$REPO/.sweep}"
mkdir -p "$SCRATCH/3p" || { echo "xbox-link-check: cannot create $SCRATCH/3p" >&2; exit 1; }

ENGINE_OBJS=$(ls "$SCRATCH"/*.obj 2>/dev/null \
	| grep -vE '_platform_(android|apple|dos|ios|irix|linux|nswitch|posix|psvita|sdl1|sdl3|stub|win32)_' \
	| grep -vE '_tests_')
if [ -z "$ENGINE_OBJS" ]; then
	echo "xbox-link-check: no usable objects in $SCRATCH/*.obj." >&2
	echo "  Run tools/xbox-sweep.sh engine filesystem public common compat" >&2
	echo "  3rdparty/library_suffix/src first - this script links what that" >&2
	echo "  sweep left behind, it does not compile the engine itself." >&2
	exit 1
fi

# Third-party scope: found compiling, X1, 2026-09-08. Each of these four
# submodules' own wscript names its real source list - not everything
# under the directory, each excludes its own demo/test/CLI files. Most of
# the flags below (includes, and OPUS_FLAGS' OPUS_BUILD/FLOAT_APPROX/
# PACKAGE_VERSION/CUSTOM_MODES/ENABLE_HARDENING) are exactly what each
# wscript's build() step hardcodes. VAR_ARRAYS is different, found in
# review, 2026-09-08: it is not in that hardcoded list at all - opus's
# wscript configure() (3rdparty/opus/wscript:51-53) picks it at configure
# time via a real C99-VLA compiler check, which this script does not run.
# It is included here as the deliberate, checked equivalent for nxdk's
# clang target: nxdk's pdclib has no alloca.h (opus's own fallback), and
# this toolchain genuinely supports C99 VLAs (the same VLA syntax opus's
# check fragment probes for compiles clean under tools/xbox-cc.sh - see
# docs/r3d/fork-plan.md's X1 section), so a real waf configure() run
# against this exact toolchain would take the same VAR_ARRAYS branch.
# vq_sse2.c is the one deliberate exclusion: it fails to compile at all
# under -march=pentium3 (SSE2 intrinsics, Pentium III has none), a real,
# loud clang error, not a judgment call.
#
# opusfile: fixed and included, 2026-09-08. stream.c's _WIN32 branch needed
# three things nxdk has no equivalent of at all (GetFileType, _wfopen/
# _wfreopen's wide-char CRT) - patched to take NXDK-specific branches
# instead of shimming Win32 APIs that do not exist here (see
# patches/opusfile-xbox-stream.patch, applied automatically by
# tools/xbox-cc.sh). The two calls stream.c makes that DO have a real
# nxdk equivalent (_fileno, and a _get_osfhandle/_filelengthi64 pair built
# on GetFileSizeEx) are real compat/xbox functions, not stubs - see
# compat/xbox/xbox_posix_io.c. op_fdopen's unconditional fdopen() call
# (pdclib has no such function, on any platform) fails honestly instead:
# grepped the whole tree, nothing calls op_fdopen. info.c/internal.c/
# opusfile.c needed no changes at all.
#
# config_types.h is regenerated every run rather than committed: it is
# libogg's own waf 'subst' template output (INCLUDE_INTTYPES_H=1, the
# same branch a real configure-time check would pick - nxdk's pdclib has
# inttypes.h), and the submodule's own .gitignore already treats it as
# generated, not source. Found in review, 2026-09-08: an earlier version
# of this copied the generated file into 3rdparty/libogg/libogg/include/
# ogg/config_types.h directly - gitignored, so not a commit risk, but
# still a mutation of vendored checkout state a failed/interrupted run
# could leave behind, changing what a later plain compile sees versus a
# fresh clone. Written under $SCRATCH instead, in a directory shape that
# shadows the header via include-path order rather than by touching the
# submodule at all.
mkdir -p "$SCRATCH/3p/ogg-shadow/ogg"
cat > "$SCRATCH/3p/ogg-shadow/ogg/config_types.h" <<'OGGCONFIG'
#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__
#define INCLUDE_INTTYPES_H 1
#define INCLUDE_STDINT_H 0
#define INCLUDE_SYS_TYPES_H 0
#if INCLUDE_INTTYPES_H
#  include <inttypes.h>
#endif
#if INCLUDE_STDINT_H
#  include <stdint.h>
#endif
#if INCLUDE_SYS_TYPES_H
#  include <sys/types.h>
#endif
typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;
#endif
OGGCONFIG

OPUS_FLAGS=(
	-Iopus -I3rdparty/opus/opus -I3rdparty/opus/opus/include
	-I3rdparty/opus/opus/celt -I3rdparty/opus/opus/silk -I3rdparty/opus/opus/silk/float
	-I3rdparty/opus/opus/silk/x86 -I3rdparty/opus/opus/celt/x86
	-DOPUS_BUILD -DFLOAT_APPROX '-DPACKAGE_VERSION="1.4.0"' -DCUSTOM_MODES -DENABLE_HARDENING -DVAR_ARRAYS
)
# The shadow path goes first so <ogg/config_types.h> resolves to the
# generated one under $SCRATCH before the real (unpopulated, gitignored)
# path inside the submodule is ever consulted.
VORBIS_FLAGS=( -I"$SCRATCH/3p/ogg-shadow" -I3rdparty/vorbis/vorbis-src/include -I3rdparty/vorbis/vorbis-src/lib -I3rdparty/libogg/libogg/include )
OGG_FLAGS=( -I"$SCRATCH/3p/ogg-shadow" -I3rdparty/libogg/libogg/include )
ME_FLAGS=( -I3rdparty/MultiEmulator/include -I3rdparty/MultiEmulator/src )
# wscript's own `use = 'ogg opus'` on the opusfile target - its real include
# path plus the two libraries' export_includes.
OPUSFILE_FLAGS=(
	-I"$SCRATCH/3p/ogg-shadow" -I3rdparty/opusfile/opusfile/include
	-I3rdparty/opus/opus/include -I3rdparty/libogg/libogg/include
)

# Found in review, 2026-09-08: compile_3p's own `exit 2` only ends the
# command-substitution subshell each call site ran it in ($(...)) - the
# calling loop had no `set -e` and never checked the substitution's own
# status, so a failed compile silently produced an empty path and the
# loop, then the link, continued anyway with an incomplete object set.
# Every call site now checks the substitution's exit status explicitly
# (the `||` below), so a compile failure stops the script before it ever
# reaches nxdk-link, instead of producing a believable-looking link
# result over the wrong input.
compile_3p() {
	local f="$1"; shift
	local obj="$SCRATCH/3p/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.obj/')"
	tools/xbox-cc.sh "$f" "$@" -o "$obj" 2>"$SCRATCH/3p/lasterr.txt"
	local status=$?
	if [ "$status" -ne 0 ]; then
		echo "xbox-link-check: THIRD-PARTY FILE THAT WAS SUPPOSED TO COMPILE DID NOT: $f" >&2
		cat "$SCRATCH/3p/lasterr.txt" >&2
		return 2
	fi
	echo "$obj"
}

THIRDPARTY_OBJS=""
for f in 3rdparty/opus/opus/src/*.c 3rdparty/opus/opus/celt/*.c \
         3rdparty/opus/opus/silk/*.c 3rdparty/opus/opus/silk/float/*.c; do
	base=$(basename "$f")
	case "$base" in
		repacketizer_demo.c|opus_demo.c|opus_compare.c|opus_custom_demo.c) continue ;;
	esac
	obj=$(compile_3p "$f" "${OPUS_FLAGS[@]}") || exit 2
	THIRDPARTY_OBJS="$THIRDPARTY_OBJS $obj"
done
for f in 3rdparty/opus/opus/silk/x86/x86_silk_map.c \
         3rdparty/opus/opus/celt/x86/pitch_sse.c \
         3rdparty/opus/opus/celt/x86/pitch_sse2.c \
         3rdparty/opus/opus/celt/x86/x86_celt_map.c \
         3rdparty/opus/opus/celt/x86/x86cpu.c; do
	obj=$(compile_3p "$f" "${OPUS_FLAGS[@]}") || exit 2
	THIRDPARTY_OBJS="$THIRDPARTY_OBJS $obj"
done
for name in mdct smallft block envelope window lsp lpc analysis synthesis psy \
            info floor1 floor0 res0 mapping0 registry codebook sharedbook \
            lookup bitrate vorbisfile; do
	obj=$(compile_3p "3rdparty/vorbis/vorbis-src/lib/$name.c" "${VORBIS_FLAGS[@]}") || exit 2
	THIRDPARTY_OBJS="$THIRDPARTY_OBJS $obj"
done
for f in 3rdparty/libogg/libogg/src/*.c; do
	obj=$(compile_3p "$f" "${OGG_FLAGS[@]}") || exit 2
	THIRDPARTY_OBJS="$THIRDPARTY_OBJS $obj"
done
for f in 3rdparty/MultiEmulator/src/*.c; do
	obj=$(compile_3p "$f" "${ME_FLAGS[@]}") || exit 2
	THIRDPARTY_OBJS="$THIRDPARTY_OBJS $obj"
done
for name in info internal opusfile stream; do
	obj=$(compile_3p "3rdparty/opusfile/opusfile/src/$name.c" "${OPUSFILE_FLAGS[@]}") || exit 2
	THIRDPARTY_OBJS="$THIRDPARTY_OBJS $obj"
done

echo "xbox-link-check: $(echo $THIRDPARTY_OBJS | wc -w) third-party objects compiled." >&2

NXDK_DIR="deps/nxdk"
LIBS=(
	"$NXDK_DIR/lib/xboxkrnl/libxboxkrnl.lib" "$NXDK_DIR/lib/libpdclib.lib"
	"$NXDK_DIR/lib/libwinapi.lib" "$NXDK_DIR/lib/libnxdk_hal.lib"
	"$NXDK_DIR/lib/libnxdk.lib" "$NXDK_DIR/lib/libnxdk_automount_d.lib"
	"$NXDK_DIR/lib/libpbkit.lib" "$NXDK_DIR/lib/nxdk_usb.lib"
	"$NXDK_DIR/lib/libxboxrt.lib" "$NXDK_DIR/lib/libzlib.lib"
	"$NXDK_DIR/lib/libpng.lib" "$NXDK_DIR/lib/libjpeg.lib"
	"$NXDK_DIR/lib/libnxdk_net.lib" "$NXDK_DIR/lib/libSDL2.lib"
	"$NXDK_DIR/lib/libSDL2_image.lib" "$NXDK_DIR/lib/libSDL_ttf.lib"
	"$NXDK_DIR/lib/libfreetype.lib" "$NXDK_DIR/lib/winmm.lib"
)

LOG="$SCRATCH/link-check.txt"
# shellcheck disable=SC2086
deps/nxdk/bin/nxdk-link -errorlimit:0 -include:_automount_d_drive \
	-out:"$SCRATCH/link-check.exe" $ENGINE_OBJS $THIRDPARTY_OBJS "${LIBS[@]}" > "$LOG" 2>&1
LINK_STATUS=$?

DUP=$(grep -c 'duplicate symbol' "$LOG")
UNDEF=$(grep -c 'undefined symbol' "$LOG")

echo
echo "link-check: $DUP duplicate, $UNDEF undefined   (full log: $LOG)"
echo

# Found in review, 2026-09-08: grepping the log for duplicate/undefined
# counts without ever checking nxdk-link's own exit status means a fatal
# failure that isn't one of those two error shapes (missing library, bad
# option, corrupt object, a lld crash) prints "0 duplicate, 0 undefined"
# and the script falls through to exit 0 - the exact false-clean
# measurement shape this project has hit before (xbox-sweep.sh,
# xbox-check-nxdk-cc-drift.sh). lld's own error lines are always
# "lld: error: <reason>"; duplicate/undefined symbol errors are the two
# reasons this script treats as expected outcomes, everything else is
# not. A clean link (LINK_STATUS 0) with DUP+UNDEF both 0 is fine; a
# failed link (LINK_STATUS non-zero) is only fine if every "lld: error:"
# line is accounted for by the duplicate/undefined counts already taken.
TOTAL_ERR=$(grep -c '^lld: error:' "$LOG")
OTHER_ERR=$(( TOTAL_ERR - DUP - UNDEF ))
if [ "$OTHER_ERR" -gt 0 ] || { [ "$LINK_STATUS" -ne 0 ] && [ "$TOTAL_ERR" -eq 0 ]; }; then
	echo "link-check: REGRESSION - nxdk-link failed (exit $LINK_STATUS) for a reason other than duplicate/undefined symbols." >&2
	echo "  Full log: $LOG" >&2
	tail -n 20 "$LOG" >&2
	exit 1
fi

# Any duplicate symbol at all is always a regression: this project has no
# accepted/expected duplicate-symbol case, unlike undefined symbols, which
# has a large, deliberately-parked networking group. Fail loudly on any
# nonzero count rather than requiring a caller to pass --expect-duplicate.
if [ "$DUP" -gt 0 ]; then
	echo "link-check: REGRESSION - $DUP duplicate symbol(s), expected 0." >&2
	grep 'duplicate symbol' "$LOG" | sed 's/.*duplicate symbol: //' | sort -u >&2
	exit 1
fi

# Found in review, 2026-09-08: --expect-undefined was documented as
# optional, and the gate below only ran when it was actually passed - so
# the exact command this script's own usage comment shows at the top,
# without the flag, exited 0 on a link that failed with 64 real undefined
# symbols. Indistinguishable from clean in the exit code, only visible in
# text output nothing downstream reads. The same false-clean class this
# tool exists to prevent. UNDEF > 0 with no baseline is now itself a hard
# failure - there is no "just report it" mode once undefined symbols
# exist, only "compare against a tracked count" or "fail".
if [ "$UNDEF" -gt 0 ] && [ -z "$EXPECT_UNDEFINED" ]; then
	echo "link-check: REGRESSION - $UNDEF undefined symbol(s), no --expect-undefined given." >&2
	echo "  Pass --expect-undefined N (see the log above) to compare against a" >&2
	echo "  tracked baseline, or fix them - a link with unresolved symbols is" >&2
	echo "  never a passing result on its own." >&2
	exit 1
fi

if [ -n "$EXPECT_UNDEFINED" ]; then
	if [ "$UNDEF" -eq "$EXPECT_UNDEFINED" ]; then
		echo "link-check: $UNDEF undefined matches --expect-undefined $EXPECT_UNDEFINED."
	elif [ "$UNDEF" -gt "$EXPECT_UNDEFINED" ]; then
		echo "link-check: REGRESSION - $UNDEF undefined, expected $EXPECT_UNDEFINED." >&2
		echo "  New undefined symbol(s), not yet accounted for:" >&2
		exit 1
	else
		echo "link-check: IMPROVEMENT - $UNDEF undefined, expected $EXPECT_UNDEFINED." >&2
		echo "  Update the tracked count and whichever docs cite it." >&2
		exit 1
	fi
fi
