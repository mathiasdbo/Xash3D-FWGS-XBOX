#!/usr/bin/env bash
# Attempt a real Xbox link across the main engine, the third-party audio
# codec libraries, and (optionally) the menu module - whatever this
# project's own sweep scripts have most recently compiled - and gate on
# duplicate/undefined symbol counts.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-sweep.sh engine filesystem public common compat 3rdparty/library_suffix/src
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-mainui-sweep.sh   # optional - adds the menu module to the link
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
XBE_OUT=""
CLIENT_MAIN=0
while [ $# -gt 0 ]; do
	case "$1" in
		--expect-undefined)
			EXPECT_UNDEFINED="$2"
			shift 2
			;;
		--xbe)
			XBE_OUT="$2"
			shift 2
			;;
		--client-main)
			# Link engine/common/launcher.c's real main() (Host_Main - the
			# same path the dedicated target already uses) instead of
			# engine/platform/xbox/xbox_main.c's placeholder (X1,
			# divergence #23). Requires launcher.obj to have been compiled
			# with XBOX_CLIENT_MAIN=1 (tools/xbox-cc.sh) - this flag only
			# changes which object this script SELECTS from $SCRATCH, it
			# does not recompile anything. Without this flag, behaviour is
			# byte-for-byte the X1-tracked default: xbox_main.obj included,
			# launcher.obj's own XASH_ENABLE_MAIN-gated main() compiles to
			# nothing and is link-inert either way.
			CLIENT_MAIN=1
			shift
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

# Freshness gate (Codex review, X5): an object in $SCRATCH is only evidence
# of what its source and tools/xbox-cc.sh's flags were WHEN IT WAS BUILT.
# Linking whatever sits there could report "0 duplicate, 0 undefined" on an
# engine that predates this checkout's source edits, or a -D added to the
# driver since (XASH_REF_SOFT_ENABLED, the change that made this bite), or
# a source that now fails and whose old object survived. So every engine
# object linked here must be listed OK by tools/xbox-sweep.sh's own
# manifest, that manifest must name the SAME tools/xbox-cc.sh (by
# checksum) that is on disk now, and the object must be newer than both
# its source and the driver. Three objects are compiled by this script
# or its callers rather than the sweep and are exempted by name:
# engine_common_launcher.obj (--client-main, compiled with
# XASH_ENABLE_MAIN=1 by the caller), ref_null_r_context.obj (below), and
# engine_common_net_chan.obj - net_chan.c is in the sweep's tracked
# failing baseline (bzlib.h) and this script recompiles it itself with
# bzip2's include path further down; a leftover from that step is
# exactly what the manifest, by design, marks FAIL.
MANIFEST="$SCRATCH/sweep-manifest.txt"
if [ ! -f "$MANIFEST" ]; then
	echo "xbox-link-check: $MANIFEST does not exist - run tools/xbox-sweep.sh" >&2
	echo "  (the canonical five-directory sweep) first; this script refuses to" >&2
	echo "  link objects no sweep vouches for." >&2
	exit 1
fi
DRIVER_SUM_NOW="$(cksum tools/xbox-cc.sh | awk '{print $1}')"
DRIVER_SUM_THEN="$(awk '$1 == "DRIVER" { print $3; exit }' "$MANIFEST")"
if [ "$DRIVER_SUM_NOW" != "$DRIVER_SUM_THEN" ]; then
	echo "xbox-link-check: tools/xbox-cc.sh changed since the sweep that produced" >&2
	echo "  $MANIFEST (checksum $DRIVER_SUM_THEN then, $DRIVER_SUM_NOW now) - every" >&2
	echo "  object may carry stale flags. Re-run the sweep." >&2
	exit 1
fi
STALE=0
# The manifest must describe THIS checkout's source set, not just the
# objects that happen to exist (Codex review, round 2): a source deleted
# since the sweep leaves a manifest-approved object behind with nothing to
# be newer than, and a source added since the sweep has no object at all
# and never enters the per-object loop below. So enumerate the current
# sources with the sweep's own rule (find *.c/*.cpp under the canonical
# directories), and require every one of them to have a manifest line
# (OK or FAIL) and every manifest source to still exist.
SWEEP_DIRS="engine filesystem public common compat 3rdparty/library_suffix/src"
# shellcheck disable=SC2086
CURRENT_SRCS="$(find $SWEEP_DIRS \( -name '*.c' -o -name '*.cpp' \) 2>/dev/null | sort)"
MANIFEST_SRCS="$(awk '$1 == "OK" || $1 == "FAIL" { print $2 }' "$MANIFEST" | sort)"
ADDED="$(comm -23 <(printf '%s\n' "$CURRENT_SRCS") <(printf '%s\n' "$MANIFEST_SRCS"))"
REMOVED="$(comm -13 <(printf '%s\n' "$CURRENT_SRCS") <(printf '%s\n' "$MANIFEST_SRCS"))"
if [ -n "$ADDED" ]; then
	echo "xbox-link-check: source(s) present now but absent from the sweep manifest - swept before they existed:" >&2
	printf '  %s\n' $ADDED >&2
	STALE=$((STALE + $(printf '%s\n' "$ADDED" | wc -l)))
fi
if [ -n "$REMOVED" ]; then
	echo "xbox-link-check: manifest source(s) that no longer exist - their objects are orphans:" >&2
	printf '  %s\n' $REMOVED >&2
	STALE=$((STALE + $(printf '%s\n' "$REMOVED" | wc -l)))
fi
for o in $ENGINE_OBJS; do
	case "$(basename "$o")" in
		engine_common_launcher.obj|ref_null_r_context.obj|engine_common_net_chan.obj) continue ;;
	esac
	src="$(awk -v obj="$o" '$1 == "OK" && $3 == obj { print $2; exit }' "$MANIFEST")"
	if [ -z "$src" ]; then
		echo "xbox-link-check: $o is not listed OK in the sweep manifest - stale," >&2
		echo "  failed since, or never swept. Refusing to link it." >&2
		STALE=$((STALE + 1))
		continue
	fi
	if [ "$src" -nt "$o" ] || [ tools/xbox-cc.sh -nt "$o" ]; then
		echo "xbox-link-check: $o is older than $src or tools/xbox-cc.sh - re-sweep." >&2
		STALE=$((STALE + 1))
	fi
done
if [ "$STALE" -ne 0 ]; then
	echo "xbox-link-check: $STALE stale or unvouched engine object(s) - not linking." >&2
	exit 1
fi
echo "xbox-link-check: every engine object is vouched for by the sweep manifest and newer than its source and driver." >&2

if [ "$CLIENT_MAIN" -eq 1 ]; then
	LAUNCHER_OBJ="$SCRATCH/engine_common_launcher.obj"
	if [ ! -f "$LAUNCHER_OBJ" ]; then
		echo "xbox-link-check: --client-main given but $LAUNCHER_OBJ does not exist." >&2
		echo "  Compile it first: XBOX_CLIENT_MAIN=1 tools/xbox-cc.sh" >&2
		echo "  engine/common/launcher.c -o $LAUNCHER_OBJ" >&2
		exit 1
	fi
	# xbox_main.obj's own placeholder main() (X1, divergence #23) would
	# collide with launcher.obj's real one at link time - both define
	# `_main` once XBOX_CLIENT_MAIN=1 makes XASH_ENABLE_MAIN real.
	ENGINE_OBJS=$(echo "$ENGINE_OBJS" | grep -v '_platform_xbox_xbox_main\.obj$')
fi
# Raw ref/soft and ref/common objects (a `tools/xbox-sweep.sh ref/soft` run,
# or a by-hand tools/xbox-cc.sh of one file, drops them into $SCRATCH under
# these names) must never reach the link: the module is linked ONLY in its
# isolated form (below), and the raw copies re-create exactly the 20
# duplicate symbols isolation exists to remove - measured, X5, when
# leftovers from a compile experiment sat next to the isolated set.
# grep -E with plain, unescaped grouping/alternation - NOT the backslash-
# escaped `\(...\|...\)` GNU BRE form. Same real bug tools/xbox-ref-
# isolate.sh's own leak-check had (docs/r3d/lessons.md): the grep this
# script's PATH resolves to on this host silently treats `\(`/`\|` as
# literal characters rather than grouping/alternation, so this exclusion
# has always been a silent no-op here - every line survives `-v`
# unfiltered, defeating the exact safety net the comment above describes.
# Never manifested as a real duplicate-symbol failure only because the
# sweep scope this project currently uses never compiles ref/ at all
# (`tools/xbox-sweep.sh engine filesystem public common compat ...`) - a
# by-hand or future-scope-change compile of ref/soft or ref/common would
# have hit exactly the X5 leftover-artifact scenario this line exists to
# prevent, silently.
ENGINE_OBJS=$(echo "$ENGINE_OBJS" | grep -vE '/ref_(soft|common)_[^/]*\.obj$')

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

# bz_version.h: bzip2's own CMake-generated header (bz_version.h.in -> a
# single #define BZ_VERSION "..."), never committed for the same reason
# libogg's config_types.h above is not - CMakeLists.txt's own
# `project(bzip2 VERSION 1.1.0 ...)` names the real value, not guessed.
mkdir -p "$SCRATCH/3p/bz-shadow"
printf '#define BZ_VERSION "1.1.0"\n' > "$SCRATCH/3p/bz-shadow/bz_version.h"
BZIP2_FLAGS=( -I"$SCRATCH/3p/bz-shadow" -I3rdparty/bzip2/bzip2 )

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
# bzip2: engine/common/net_chan.c's own `#if !XASH_DEDICATED #include
# <bzlib.h> #endif` (file-fragment compression for the client's non-
# dedicated path) is real, not a stub - CMakeLists.txt's own BZ2_SOURCES
# names these 7 files (blocksort/huffman/crctable/randtable/compress/
# decompress/bzlib.c), excluding the bzip2/bzip2recover/dlltest CLI and
# test-only files under the same directory, same "each submodule's own
# build file names the real list" rule the other three libraries above
# follow. BZ_NO_STDIO on bzlib.c only: its file-stream wrapper API
# (BZ2_bzopen et al, guarded by #ifndef BZ_NO_STDIO) calls setmode/fdopen,
# neither of which nxdk's pdclib provides - net_chan.c only ever calls the
# memory-buffer API (BZ2_bzBuffToBuffCompress/Decompress), which does not
# need them; a real, upstream-documented define, not invented here.
for name in blocksort huffman crctable randtable compress decompress; do
	obj=$(compile_3p "3rdparty/bzip2/bzip2/$name.c" "${BZIP2_FLAGS[@]}") || exit 2
	THIRDPARTY_OBJS="$THIRDPARTY_OBJS $obj"
done
obj=$(compile_3p "3rdparty/bzip2/bzip2/bzlib.c" "${BZIP2_FLAGS[@]}" -DBZ_NO_STDIO) || exit 2
THIRDPARTY_OBJS="$THIRDPARTY_OBJS $obj"
# BZ2_bz__AssertH__fail: bzlib.c's own definition (fprintf+exit) sits
# INSIDE the same #ifndef BZ_NO_STDIO block that guards the setmode/fdopen
# calls BZ_NO_STDIO exists to skip above - so defining BZ_NO_STDIO drops
# this function's only definition too, even though bzlib_private.h's
# AssertH() macro (blocksort.c/compress.c/decompress.c/huffman.c's own
# internal consistency checks - upstream bzip2's ordinary self-checks, not
# something this port added) calls it unconditionally. Real, not a no-op
# stub: a genuine internal-consistency panic still needs to actually stop
# execution, the same contract upstream's exit(3) provides, just without
# the fprintf(stderr,...) call this platform's BZ_NO_STDIO path already
# established has no target for.
BZASSERT_C="$SCRATCH/3p/bz_assert_fail.c"
{
	echo '#include <stdlib.h>'
	echo 'void BZ2_bz__AssertH__fail(int errcode) { (void)errcode; abort(); }'
} > "$BZASSERT_C"
obj=$(compile_3p "$BZASSERT_C" "${BZIP2_FLAGS[@]}") || exit 2
THIRDPARTY_OBJS="$THIRDPARTY_OBJS $obj"

echo "xbox-link-check: $(echo $THIRDPARTY_OBJS | wc -w) third-party objects compiled." >&2

# net_chan.c itself: an ENGINE file, not third-party, but it needs bzlib.h
# on its include path (above) to compile at all for the client (non-
# dedicated) target - tools/xbox-sweep.sh's plain sweep has no per-file
# extra-flags mechanism, so it fails there by design (one of the tracked
# "30 failing" - not a bug in that script) and gets recompiled here
# instead, the same way filesystem_stdio's generated exports file is
# recompiled below rather than asked of the sweep. Not third-party
# (net_chan.c is ours, matches upstream) so it goes into ENGINE_OBJS, not
# THIRDPARTY_OBJS - and must replace whatever the plain sweep already
# left in $SCRATCH for it (nothing, today, since it fails there - but a
# future sweep of a from-scratch checkout could try and fail differently,
# so exclude defensively rather than trust the sweep never produced one).
NET_CHAN_OBJ="$SCRATCH/engine_common_net_chan.obj"
if ! tools/xbox-cc.sh engine/common/net_chan.c "${BZIP2_FLAGS[@]}" -o "$NET_CHAN_OBJ" 2>"$SCRATCH/3p/lasterr.txt"; then
	echo "xbox-link-check: engine/common/net_chan.c (with bzlib.h on the include path) failed to compile:" >&2
	cat "$SCRATCH/3p/lasterr.txt" >&2
	exit 2
fi
ENGINE_OBJS=$(echo "$ENGINE_OBJS" | grep -v '_common_net_chan\.obj$')
ENGINE_OBJS="$ENGINE_OBJS
$NET_CHAN_OBJ"
echo "xbox-link-check: engine/common/net_chan.c compiled with bzlib.h on the include path." >&2

# Menu module objects: optional, included only if present AND the sweep
# that produced them says it was clean. Not compiled here (same "does not
# recompile what another script already built" reasoning as $ENGINE_OBJS
# above) - tools/xbox-mainui-sweep.sh is what produces
# $SCRATCH/mainui/*.obj, and this script reuses its output the same way
# it reuses tools/xbox-sweep.sh's. Optional rather than required: a caller
# measuring just the engine+third-party picture (the original scope of
# this script) should not be forced to also run the mainui sweep first.
#
# Found in review, 2026-09-09: listing *.obj on its own only proves files
# exist, not that THIS checkout's source produced them - a file that
# compiled once, then got deleted/renamed/started failing, would leave a
# stale object nothing here could tell apart from a genuinely current
# one, silently linking a source set that no longer matches HEAD.
# tools/xbox-mainui-sweep.sh now clears $OUT_DIR before every run and
# writes manifest.txt (OK=/FAIL=/TOTAL=) after every run, pass or fail -
# required here, not just consulted: no manifest (never swept, or swept
# by a version predating this fix) or FAIL>0 (last sweep was not clean)
# both refuse to include mainui rather than guessing either way.
MAINUI_DIR="$SCRATCH/mainui"
MAINUI_OBJS=""
if [ -d "$MAINUI_DIR" ]; then
	MANIFEST="$MAINUI_DIR/manifest.txt"
	if [ ! -f "$MANIFEST" ]; then
		echo "xbox-link-check: $MAINUI_DIR exists but has no manifest.txt -" >&2
		echo "  it predates the staleness fix, or the sweep never finished." >&2
		echo "  Run tools/xbox-mainui-sweep.sh again before trusting these objects." >&2
		exit 1
	fi
	# Found running this for real, 2026-09-12: reading $MANIFEST with `sed`
	# right after the third-party compile step above (186 objects, heavy
	# I/O) came back EMPTY - reproduced 5 times in a row through this
	# script, never once reproduced reading the exact same file moments
	# later by hand. A retry loop around the same two `sed` calls (up to 3
	# attempts, 1s apart) did NOT fix it either - ruling out a simple
	# sub-second transient race, the mitigation shape that worked for the
	# two similar-looking bugs found earlier this session (tools/xbox-
	# game-sweep.sh's log, tools/xbox-game-isolate.sh's temp dir). What DID
	# keep working, every time, including inside the very run where both
	# `sed` calls came back empty: this same script's own next line,
	# `$(cat "$MANIFEST" ...)`, in the error message - a single whole-file
	# read via `cat`, not `sed`'s own per-line regex engine. Rather than
	# chase why `sed` specifically misbehaves here, avoid depending on it
	# for this extraction at all: read the file once into a variable with
	# `cat` (the primitive already proven reliable in this exact spot) and
	# extract both fields from that in-memory string with bash's own
	# regex matching - no second subprocess, no repeat file access.
	MANIFEST_CONTENT="$(cat "$MANIFEST" 2>/dev/null)"
	MANIFEST_FAIL=""
	MANIFEST_TOTAL=""
	if [[ "$MANIFEST_CONTENT" =~ FAIL=([0-9]+) ]]; then
		MANIFEST_FAIL="${BASH_REMATCH[1]}"
	fi
	if [[ "$MANIFEST_CONTENT" =~ TOTAL=([0-9]+) ]]; then
		MANIFEST_TOTAL="${BASH_REMATCH[1]}"
	fi
	# Found in review, 2026-09-09 (second pass): checking FAIL alone was
	# not enough - tools/xbox-mainui-sweep.sh used to write "OK=0 FAIL=0
	# TOTAL=0" for its own zero-files failure case before that case's own
	# exit, and FAIL=0 read as clean here even though nothing was actually
	# measured. Fixed at the source (that script now rejects TOTAL=0
	# before ever writing a manifest), but this check requires TOTAL>0
	# explicitly anyway - defense in depth against a hand-edited or
	# older-script-written manifest, not a re-trust of the same field.
	if [ -z "$MANIFEST_FAIL" ] || [ "$MANIFEST_FAIL" -gt 0 ] \
		|| [ -z "$MANIFEST_TOTAL" ] || [ "$MANIFEST_TOTAL" -eq 0 ]; then
		echo "xbox-link-check: $MAINUI_DIR/manifest.txt is not clean -" >&2
		echo "  ($MANIFEST_CONTENT) - the last mainui sweep was not" >&2
		echo "  clean, or measured nothing." >&2
		echo "  Fix the failures and re-run tools/xbox-mainui-sweep.sh, or remove" >&2
		echo "  $MAINUI_DIR entirely to measure engine+third-party only." >&2
		exit 1
	fi
	MAINUI_OBJS=$(ls "$MAINUI_DIR"/*.obj 2>/dev/null)
	MAINUI_OBJ_COUNT=$(echo $MAINUI_OBJS | wc -w)
	# The manifest's own OK count against what is actually on disk right
	# now - the manifest could be stale relative to $OUT_DIR even though
	# it was clean when written (an object deleted by hand after the
	# sweep, a half-finished manual cleanup). Found in the same pass as
	# the FAIL/TOTAL fix above: this was a THIRD `sed` call against the
	# same file, missed the first time - hit the identical empty-read
	# failure on its own, right after the FAIL/TOTAL fix stopped masking
	# it. Reuses $MANIFEST_CONTENT (already read once, above) instead of
	# a fresh `sed`/file read.
	MANIFEST_OK=""
	if [[ "$MANIFEST_CONTENT" =~ OK=([0-9]+) ]]; then
		MANIFEST_OK="${BASH_REMATCH[1]}"
	fi
	if [ -z "$MANIFEST_OK" ] || [ "$MAINUI_OBJ_COUNT" -ne "$MANIFEST_OK" ]; then
		echo "xbox-link-check: $MAINUI_OBJ_COUNT objects on disk in $MAINUI_DIR" >&2
		echo "  do not match manifest.txt's OK=$MANIFEST_OK - re-run" >&2
		echo "  tools/xbox-mainui-sweep.sh to refresh both together." >&2
		exit 1
	fi
	echo "xbox-link-check: $MAINUI_OBJ_COUNT menu module objects included (from tools/xbox-mainui-sweep.sh, manifest clean)." >&2

	# Found in review, 2026-09-09: mainui's 82 objects being IN the link
	# does not mean the menu module is actually REACHABLE - lib_static.c's
	# Lib_Find only resolves names present in generated_library_tables.h,
	# and an earlier version of this branch never registered "menu" there
	# at all, so COM_LoadLibrary("menu") would have returned NULL even
	# with every mainui symbol correctly linked in. Compiled here, inside
	# this if-block rather than alongside stub_game below, deliberately:
	# generated_lib_menu_exports.c declares `extern void GetMenuAPI(void)`
	# etc. and takes their real addresses (3rdparty/mainui/udll_int.cpp's
	# own functions, not stub code) - compiling it when $MAINUI_OBJS is
	# empty would introduce undefined symbols for a module that was never
	# linked in, breaking the "measure engine+third-party only" case this
	# script's own mainui-optionality exists to support.
	MENU_EXPORTS_C="$SCRATCH/gen/generated_lib_menu_exports.c"
	if [ ! -f "$MENU_EXPORTS_C" ]; then
		echo "xbox-link-check: $MENU_EXPORTS_C does not exist." >&2
		echo "  Run tools/xbox-sweep.sh (or any tools/xbox-cc.sh compile) first -" >&2
		echo "  it generates $SCRATCH/gen/generated_lib_*_exports.c as a side effect." >&2
		exit 1
	fi
	MENU_EXPORTS_OBJ="$SCRATCH/gen/generated_lib_menu_exports.obj"
	if ! tools/xbox-cc.sh "$MENU_EXPORTS_C" -o "$MENU_EXPORTS_OBJ" 2>"$SCRATCH/gen/stub-lasterr.txt"; then
		echo "xbox-link-check: STUB GAME MODULE FILE THAT WAS SUPPOSED TO COMPILE DID NOT: $MENU_EXPORTS_C" >&2
		cat "$SCRATCH/gen/stub-lasterr.txt" >&2
		exit 2
	fi
	MAINUI_OBJS="$MAINUI_OBJS $MENU_EXPORTS_OBJ"
	echo "xbox-link-check: menu registered in the static library table (GetMenuAPI/GetExtAPI, real mainui functions)." >&2
else
	# Found in review, 2026-09-09 (second pass on the same fix): the outer
	# table generated_library_tables.h writes ALWAYS declares
	# `extern table_t lib_menu_exports[]` now (tools/xbox-cc.sh passes
	# "menu:..." to tools/gen-library-tables.sh unconditionally, since that
	# script has no way to know at generation time whether this run will
	# include mainui) - so when mainui is NOT linked, that declaration
	# would itself be an undefined symbol (`_lib_menu_exports`, verified:
	# this is exactly what a real run with $MAINUI_DIR absent produced
	# before this fix). A trivial, real definition here - a genuinely
	# empty, terminated table_t array, the same shape gen-library-tables.sh
	# itself would write for an empty exports.txt - keeps the "measure
	# engine+third-party only" case working without referencing any
	# mainui symbol at all.
	MENU_EXPORTS_OBJ="$SCRATCH/gen/generated_lib_menu_exports_empty.obj"
	MENU_EXPORTS_STUB_C="$SCRATCH/gen/generated_lib_menu_exports_empty.c"
	{
		echo "typedef struct {const char *name;void *func;} table_t;"
		echo "table_t lib_menu_exports[] = { {0,0} };"
	} > "$MENU_EXPORTS_STUB_C"
	if ! tools/xbox-cc.sh "$MENU_EXPORTS_STUB_C" -o "$MENU_EXPORTS_OBJ" 2>"$SCRATCH/gen/stub-lasterr.txt"; then
		echo "xbox-link-check: FALLBACK EMPTY lib_menu_exports FAILED TO COMPILE (should never happen):" >&2
		cat "$SCRATCH/gen/stub-lasterr.txt" >&2
		exit 2
	fi
	MAINUI_OBJS="$MENU_EXPORTS_OBJ"
	echo "xbox-link-check: mainui not swept - menu registered with an empty table (COM_LoadLibrary(\"menu\") would resolve but export nothing)." >&2
fi

# stub_game: X1's own exit line names a fourth piece besides engine,
# filesystem and menu - "a stub game module". Compiled fresh every run,
# same reasoning as the third-party objects above (small, cheap, no
# separate sweep script earns its keep for three files). Not optional the
# way $MAINUI_OBJS is: tools/gen-library-tables.sh's own generated
# generated_lib_server_exports.c/generated_lib_client_exports.c
# reference server_stub.c's GetEntityAPI2 by name (an `extern void
# GetEntityAPI2(void);` declaration written into the generated file
# itself), so leaving any one of these three out of the link would be an
# immediate, deliberately-introduced undefined symbol - there is no
# meaningful "measure without the stub" configuration the way there is a
# meaningful "measure without the menu module" one.
# filesystem_stdio: found in X2 W5, booting the dedicated target in xemu
# for the first time - "FS_LoadProgs: can't load filesystem library
# filesystem_stdio", looping the boot animation forever (Sys_Error ->
# host shutdown -> BIOS reboot -> same failure). This comment's own
# "besides engine, filesystem and menu" phrasing above already assumed
# filesystem was wired in; it never was - tools/gen-library-tables.sh's
# invocation below only ever passed server/client/menu.
# filesystem/exports.txt (GetFSAPI) already names the real entry point,
# same as menu's GetMenuAPI/GetExtAPI - filesystem/*.c's own GetFSAPI is
# already part of $ENGINE_OBJS (this build links engine and filesystem
# into one binary), so this is a real fix, not a new stub, and required
# unconditionally: unlike menu, FS_Init runs on every boot regardless of
# dedicated/client.
STUB_GAME_OBJS=""
FS_EXPORTS_C="$SCRATCH/gen/generated_lib_filesystem_stdio_exports.c"
if [ ! -f "$FS_EXPORTS_C" ]; then
	echo "xbox-link-check: $FS_EXPORTS_C does not exist." >&2
	echo "  Run tools/xbox-sweep.sh (or any tools/xbox-cc.sh compile) first -" >&2
	echo "  it generates $SCRATCH/gen/generated_lib_*_exports.c as a side effect." >&2
	exit 1
fi
FS_EXPORTS_OBJ="$SCRATCH/gen/$(echo "$FS_EXPORTS_C" | tr '/' '_' | sed 's/\.c$/.obj/')"
if ! tools/xbox-cc.sh "$FS_EXPORTS_C" -o "$FS_EXPORTS_OBJ" 2>"$SCRATCH/gen/stub-lasterr.txt"; then
	echo "xbox-link-check: generated_lib_filesystem_stdio_exports.c failed to compile:" >&2
	cat "$SCRATCH/gen/stub-lasterr.txt" >&2
	exit 2
fi
STUB_GAME_OBJS="$FS_EXPORTS_OBJ"
echo "xbox-link-check: filesystem_stdio registered in the static library table (real GetFSAPI)." >&2

# ref_null: X5 bring-up. R_Init()'s own renderer-selection loop
# (engine/client/dll_int/ref_common.c) only auto-tries names gated by a
# compile-time XASH_REF_*_ENABLED define, none of which exist for Xbox
# yet (no vid_xbox.c/ref_soft port written) - without SOME renderer
# registered, VID_Init() cannot succeed at all, blocking client boot
# before it reaches the main loop. ref/null (upstream's own "run full
# client in hazardous^Wheadless environments" module, ref/null/r_context.c's
# own comment) is a real, complete ref_interface_t of no-op stubs -
# genuinely useful on its own merits here, not a placeholder invented for
# this project: it lets the REST of the client boot path (SV_Init,
# CL_Init, menu init, the frame loop) be verified independently of the
# harder problem of porting a real rasterizer to the NV2A. Selected at
# runtime via a `r_refdll "null"` line in a disc's video.cfg (executed by
# R_Init() itself, upstream's own designated renderer-choice mechanism -
# no engine code changes needed for selection), not compiled in as the
# default.
REF_NULL_EXPORTS_C="$SCRATCH/gen/generated_lib_ref_null_exports.c"
if [ ! -f "$REF_NULL_EXPORTS_C" ]; then
	echo "xbox-link-check: $REF_NULL_EXPORTS_C does not exist." >&2
	echo "  Run tools/xbox-sweep.sh (or any tools/xbox-cc.sh compile) first -" >&2
	echo "  it generates $SCRATCH/gen/generated_lib_*_exports.c as a side effect." >&2
	exit 1
fi
REF_NULL_EXPORTS_OBJ="$SCRATCH/gen/$(echo "$REF_NULL_EXPORTS_C" | tr '/' '_' | sed 's/\.c$/.obj/')"
if ! tools/xbox-cc.sh "$REF_NULL_EXPORTS_C" -o "$REF_NULL_EXPORTS_OBJ" 2>"$SCRATCH/gen/stub-lasterr.txt"; then
	echo "xbox-link-check: generated_lib_ref_null_exports.c failed to compile:" >&2
	cat "$SCRATCH/gen/stub-lasterr.txt" >&2
	exit 2
fi
REF_NULL_OBJ="$SCRATCH/ref_null_r_context.obj"
if ! tools/xbox-cc.sh ref/null/r_context.c -Iref/common -o "$REF_NULL_OBJ" 2>"$SCRATCH/gen/stub-lasterr.txt"; then
	echo "xbox-link-check: ref/null/r_context.c failed to compile:" >&2
	cat "$SCRATCH/gen/stub-lasterr.txt" >&2
	exit 2
fi
STUB_GAME_OBJS="$STUB_GAME_OBJS $REF_NULL_EXPORTS_OBJ $REF_NULL_OBJ"
echo "xbox-link-check: ref_null registered in the static library table (real GetRefAPI)." >&2

# ref_soft: X5 proper. upstream's own software rasterizer (ref/soft, 20
# files) plus the ref/common sources every ref module links (ref/soft/
# wscript: `use = ['ref_common', ...]`), each compiled through
# tools/xbox-cc.sh's ref/ rule (REF_DLL=1, ref/soft + ref/common ahead of
# the engine's include set - see the comment there for why the order is
# not optional). Registered under the name R_GetRendererName() derives
# from "soft" ("ref_%s", ref_common.c:622 under XASH_STATIC_LIBS), and
# auto-tried by R_Init()'s own renderer loop because tools/xbox-cc.sh now
# compiles the engine with XASH_REF_SOFT_ENABLED=1 - so a disc's retail
# video.cfg with `r_refdll ""` selects it without any config edit, and
# ref_null stays what it always was: the last resort after every real
# option, never the default.
REF_SOFT_EXPORTS_C="$SCRATCH/gen/generated_lib_ref_soft_exports.c"
if [ ! -f "$REF_SOFT_EXPORTS_C" ]; then
	echo "xbox-link-check: $REF_SOFT_EXPORTS_C does not exist." >&2
	echo "  Run tools/xbox-sweep.sh (or any tools/xbox-cc.sh compile) first -" >&2
	echo "  it generates $SCRATCH/gen/generated_lib_*_exports.c as a side effect." >&2
	exit 1
fi
REF_SOFT_EXPORTS_OBJ="$SCRATCH/gen/$(echo "$REF_SOFT_EXPORTS_C" | tr '/' '_' | sed 's/\.c$/.obj/')"
if ! tools/xbox-cc.sh "$REF_SOFT_EXPORTS_C" -o "$REF_SOFT_EXPORTS_OBJ" 2>"$SCRATCH/gen/stub-lasterr.txt"; then
	echo "xbox-link-check: generated_lib_ref_soft_exports.c failed to compile:" >&2
	cat "$SCRATCH/gen/stub-lasterr.txt" >&2
	exit 2
fi
# Isolated, not linked raw: measured on the first attempt, ref/soft's own
# objects next to the engine and ref_null produce 20 duplicate symbols
# (R_Init/R_Shutdown, the Tri* API, _Mem_Alloc/_Mem_Free/_Mem_Realloc,
# six convar_t definitions the engine also owns, and GetRefAPI itself) -
# tools/xbox-ref-isolate.sh's header has the list and the reasoning. It
# compiles the module fresh and renames every defined symbol soft_*;
# ref/soft/exports.txt's second column ("_soft_GetRefAPI") is what makes
# the generated table's "GetRefAPI" row resolve to this module's copy.
if ! REF_SOFT_ISO_DIR="$(bash tools/xbox-ref-isolate.sh soft)"; then
	echo "xbox-link-check: tools/xbox-ref-isolate.sh soft failed - see above." >&2
	exit 2
fi
REF_SOFT_OBJS="$REF_SOFT_EXPORTS_OBJ"
for o in "$REF_SOFT_ISO_DIR"/*.obj; do
	[ -f "$o" ] && REF_SOFT_OBJS="$REF_SOFT_OBJS $o"
done
STUB_GAME_OBJS="$STUB_GAME_OBJS $REF_SOFT_OBJS"
echo "xbox-link-check: ref_soft registered in the static library table (real, isolated GetRefAPI)." >&2

# ref_nv2a: X9 bring-up. A minimal ref_interface_t - everything but
# R_Init/R_Shutdown/R_EndFrame is a no-op stub copied from ref/null/
# r_context.c - whose only real job is proving pbkit can drive the NV2A
# through the actual engine ref-module path: R_Init brings up pbkit
# (reusing tools/x4-triangle's own already-proven, linear-texture-only
# init sequence), and R_EndFrame draws one fixed textured triangle every
# frame and presents it. No MPAK-derived geometry, no register combiners
# beyond the single stage X4 already proved, no swizzled textures (X4's
# own open defect stays untouched) - see docs/r3d/fork-plan.md's X9
# section and docs/r3d/divergences.md for the rest of what's deliberately
# out of scope. Registered the same way ref_soft was: compiled and
# isolated by tools/xbox-ref-isolate.sh, auto-tried by R_Init()'s own
# renderer loop because tools/xbox-cc.sh compiles the engine with
# XASH_REF_NV2A_ENABLED=1 - but selected explicitly via `r_refdll "nv2a"`
# in a disc's video.cfg for this bring-up, so it never silently displaces
# the already-working ref_soft in the auto-try order.
REF_NV2A_EXPORTS_C="$SCRATCH/gen/generated_lib_ref_nv2a_exports.c"
if [ ! -f "$REF_NV2A_EXPORTS_C" ]; then
	echo "xbox-link-check: $REF_NV2A_EXPORTS_C does not exist." >&2
	echo "  Run tools/xbox-sweep.sh (or any tools/xbox-cc.sh compile) first -" >&2
	echo "  it generates $SCRATCH/gen/generated_lib_*_exports.c as a side effect." >&2
	exit 1
fi
REF_NV2A_EXPORTS_OBJ="$SCRATCH/gen/$(echo "$REF_NV2A_EXPORTS_C" | tr '/' '_' | sed 's/\.c$/.obj/')"
if ! tools/xbox-cc.sh "$REF_NV2A_EXPORTS_C" -o "$REF_NV2A_EXPORTS_OBJ" 2>"$SCRATCH/gen/stub-lasterr.txt"; then
	echo "xbox-link-check: generated_lib_ref_nv2a_exports.c failed to compile:" >&2
	cat "$SCRATCH/gen/stub-lasterr.txt" >&2
	exit 2
fi
if ! REF_NV2A_ISO_DIR="$(bash tools/xbox-ref-isolate.sh nv2a)"; then
	echo "xbox-link-check: tools/xbox-ref-isolate.sh nv2a failed - see above." >&2
	exit 2
fi
REF_NV2A_OBJS="$REF_NV2A_EXPORTS_OBJ"
for o in "$REF_NV2A_ISO_DIR"/*.obj; do
	[ -f "$o" ] && REF_NV2A_OBJS="$REF_NV2A_OBJS $o"
done
STUB_GAME_OBJS="$STUB_GAME_OBJS $REF_NV2A_OBJS"
echo "xbox-link-check: ref_nv2a registered in the static library table (real, isolated GetRefAPI)." >&2

for f in stub_game/server/server_stub.c "$SCRATCH/gen/generated_lib_server_exports.c" "$SCRATCH/gen/generated_lib_client_exports.c"; do
	if [ ! -f "$f" ]; then
		echo "xbox-link-check: $f does not exist." >&2
		echo "  Run tools/xbox-sweep.sh (or any tools/xbox-cc.sh compile) first -" >&2
		echo "  it generates $SCRATCH/gen/generated_lib_*_exports.c as a side effect." >&2
		exit 1
	fi
	obj="$SCRATCH/gen/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.obj/')"
	if ! tools/xbox-cc.sh "$f" -o "$obj" 2>"$SCRATCH/gen/stub-lasterr.txt"; then
		echo "xbox-link-check: STUB GAME MODULE FILE THAT WAS SUPPOSED TO COMPILE DID NOT: $f" >&2
		cat "$SCRATCH/gen/stub-lasterr.txt" >&2
		exit 2
	fi
	STUB_GAME_OBJS="$STUB_GAME_OBJS $obj"
done
echo "xbox-link-check: stub game module included (server_stub.c + generated export tables)." >&2

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
	# libc++.lib: needed only when $MAINUI_OBJS is non-empty (the menu
	# module is the only C++ code in this link), but harmless to always
	# include - a real, prebuilt library (divergence discussion, X1's
	# "a C++ toolchain" section), not a stub that silently degrades a
	# C-only link.
	"$NXDK_DIR/lib/libc++.lib"
)

# Response file, not a raw command line: found in review, X1, 2026-09-09,
# adding the menu module's 82 objects to the already-large engine +
# third-party set pushed the full nxdk-link invocation past the OS
# argument-length limit ("Argument list too long" - the underlying lld
# invocation nxdk-link's own wrapper script builds, not this script's own
# argv handling). lld's `-flavor link` mode (what nxdk-link uses) accepts
# the standard MS link.exe `@file` convention - one quoted path per line -
# same mechanism, sidesteps the OS limit entirely regardless of how many
# objects a future sweep adds.
#
# cygpath -m on every path, not the raw MSYS2 form: found compiling this
# fix, same run - MSYS2 bash auto-translates POSIX-style paths (/g/...)
# to a real Windows path ONLY when they are actual argv entries passed
# directly to a native exe's exec call. A path written as TEXT inside a
# file the program reads back itself gets no such translation - lld read
# "/g/Proyectos/.../foo.obj" literally and reported "could not open" for
# an object that genuinely exists, because Windows has no such path.
# -m (not -w) keeps forward slashes, which lld accepts fine and avoids
# a second layer of backslash-escaping inside the response file.
RESPONSE="$SCRATCH/link-check-response.txt"
: > "$RESPONSE"
# shellcheck disable=SC2086
for f in $ENGINE_OBJS $THIRDPARTY_OBJS $MAINUI_OBJS $STUB_GAME_OBJS "${LIBS[@]}"; do
	echo "\"$(cygpath -m "$f")\"" >> "$RESPONSE"
done

# Divergence #54: compat/xbox/xbox_printf.c overrides pdclib's buffer-writing
# printf family (pdclib's prints nothing for %f/%e/%g) by being a defined
# direct input ahead of libpdclib.lib - lld extracts an archive member only
# for a symbol still undefined at that point. An ordering mistake fails
# loudly as a duplicate symbol, which this script already gates to zero;
# the ONLY silent way back to pdclib's is the object being absent from the
# link (a widened filter above, a renamed file). Assert presence and order
# here, so that path fails loudly too. Found in the toolchain review of #54,
# which proved the ordering behaviour by experiment.
PRINTF_OBJ_LINE=$(grep -n 'compat_xbox_xbox_printf\.obj' "$RESPONSE" | head -n 1 | cut -d: -f1)
PDCLIB_LINE=$(grep -n 'libpdclib\.lib' "$RESPONSE" | head -n 1 | cut -d: -f1)
if [ -z "$PRINTF_OBJ_LINE" ] || [ -z "$PDCLIB_LINE" ] || [ "$PRINTF_OBJ_LINE" -ge "$PDCLIB_LINE" ]; then
	echo "xbox-link-check: compat_xbox_xbox_printf.obj must be in the response file and precede libpdclib.lib (divergence #54)." >&2
	echo "  printf object at line ${PRINTF_OBJ_LINE:-<absent>}, libpdclib.lib at line ${PDCLIB_LINE:-<absent>} of $RESPONSE" >&2
	exit 1
fi
echo "xbox-link-check: compat printf override present (line $PRINTF_OBJ_LINE) and ahead of libpdclib.lib (line $PDCLIB_LINE)." >&2

LOG="$SCRATCH/link-check.txt"
# 1 MiB main-thread stack, overriding nxdk-link's own -stack:65536 default -
# same value and full reasoning as tools/xbox-game-link-check.sh's
# XBOX_STACK_SIZE (a 64 KiB stack cannot hold NET_QueuePacket's own
# 65535-byte frame; found live, X7.4 step 2). Kept in step here so an
# engine-only --xbe boot test measures the same binary shape.
XBOX_STACK_SIZE=1048576
deps/nxdk/bin/nxdk-link -errorlimit:0 -include:_automount_d_drive \
	-stack:"$XBOX_STACK_SIZE" \
	-out:"$SCRATCH/link-check.exe" "@$RESPONSE" > "$LOG" 2>&1
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

# The -stack: override is measured on the actual output, not trusted from
# the flag - see tools/xbox-check-stack.sh and divergence #47. nxdk-link
# emits no output while undefined symbols remain, so only an output that
# exists is checked.
if [ -f "$SCRATCH/link-check.exe" ] && ! tools/xbox-check-stack.sh "$SCRATCH/link-check.exe" "$XBOX_STACK_SIZE"; then
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

# Found in review, 2026-09-09 (second pass): the symbol-identity check
# below was originally only reached inside the `--xbe` block - meaning
# the script's own documented, ordinary invocation (`--expect-undefined N`
# alone, no `--xbe`, exactly the form the usage comment at the top of this
# file shows) still only ever compared a COUNT, the exact false-clean
# measurement class this whole check exists to close. A same-count swap
# (one parked networking symbol accidentally getting defined somewhere
# while an entirely different, genuinely new symbol goes undefined for an
# unrelated reason) would still have passed as a clean "matches
# --expect-undefined" result under ordinary use - it only got caught when
# also requesting an XBE. Moved here, unconditional on `--xbe`, so every
# successful --expect-undefined match is symbol-checked, not just the
# ones that also happen to build an XBE. docs/r3d/
# parked-undefined-symbols.txt is the real, checked-in reference - every
# undefined symbol name this tree currently has, not just how many.
if [ "$UNDEF" -gt 0 ]; then
	PARKED_LIST="$REPO/docs/r3d/parked-undefined-symbols.txt"
	if [ ! -f "$PARKED_LIST" ]; then
		echo "link-check: $PARKED_LIST does not exist - nothing to compare the" >&2
		echo "  real undefined symbol set against. Refusing to report a clean result." >&2
		exit 1
	fi
	ACTUAL_UNDEF_LIST="$SCRATCH/xbe-actual-undefined.txt"
	grep 'undefined symbol' "$LOG" | sed 's/.*undefined symbol: //' | sort -u > "$ACTUAL_UNDEF_LIST"
	EXPECTED_UNDEF_LIST="$SCRATCH/xbe-expected-undefined.txt"
	sort -u "$PARKED_LIST" > "$EXPECTED_UNDEF_LIST"
	if ! diff -u "$EXPECTED_UNDEF_LIST" "$ACTUAL_UNDEF_LIST" > "$SCRATCH/xbe-undefined-diff.txt"; then
		echo "link-check: REGRESSION - the actual undefined symbol set does not" >&2
		echo "  match docs/r3d/parked-undefined-symbols.txt, even though the COUNT" >&2
		echo "  may have matched --expect-undefined. Refusing to report a clean" >&2
		echo "  result over an unexpected symbol set - see the diff:" >&2
		cat "$SCRATCH/xbe-undefined-diff.txt" >&2
		exit 1
	fi
	echo "link-check: actual undefined symbols match docs/r3d/parked-undefined-symbols.txt exactly."
fi

# --xbe: X1's own exit line names an actual XBE, not just a clean symbol
# count. Deliberately a SEPARATE link, not a change to the measurement
# link above: the measurement's own error-vs-warning accounting (TOTAL_ERR
# counting only `lld: error:` lines) depends on undefined symbols staying
# real link ERRORS, and -force:unresolved (lld's `-flavor link` emulation
# of MSVC link.exe's real /force:unresolved flag - verified via `lld
# -flavor link /?`, not guessed) turns them into warnings instead, which
# would silently change what TOTAL_ERR means. Only reached after a
# successful --expect-undefined match AND symbol-identity match above, so
# this never runs on an unexplained, regressed, or substituted link.
#
# This is a genuine, deliberate use of /force:unresolved, not a way to
# hide a real problem: every one of the (now symbol-verified) parked
# symbols is the tracked, deliberately-parked networking group - forcing
# them is exactly what "parked, not forgotten" means at link time. A
# forced-unresolved import is not a silent no-op either: real MSVC/lld
# semantics leave it resolving to address 0, so anything that ever
# actually called one of these symbols would crash loudly, not pretend to
# succeed - matching this project's own "fail honestly" posture elsewhere
# (compat/xbox's chdir/getcwd stubs, stub_game's own honesty about
# implementing nothing).
if [ -n "$XBE_OUT" ]; then
	XBE_EXE="$SCRATCH/xbe-build.exe"
	# shellcheck disable=SC2086
	deps/nxdk/bin/nxdk-link -errorlimit:0 -include:_automount_d_drive -force:unresolved \
		-stack:"$XBOX_STACK_SIZE" \
		-out:"$XBE_EXE" "@$RESPONSE" > "$SCRATCH/xbe-link.txt" 2>&1
	XBE_LINK_STATUS=$?
	if [ "$XBE_LINK_STATUS" -ne 0 ] || [ ! -f "$XBE_EXE" ]; then
		echo "link-check: XBE build link failed (exit $XBE_LINK_STATUS) even with -force:unresolved -" >&2
		echo "  this is not the same 63 (or N) parked symbols; something else broke. See:" >&2
		echo "  $SCRATCH/xbe-link.txt" >&2
		tail -n 20 "$SCRATCH/xbe-link.txt" >&2
		exit 1
	fi

	# Measured stack check on the PE that is about to become the XBE -
	# see tools/xbox-check-stack.sh and divergence #47.
	if ! tools/xbox-check-stack.sh "$XBE_EXE" "$XBOX_STACK_SIZE"; then
		exit 1
	fi

	CXBE="deps/nxdk/tools/cxbe/cxbe"
	if [ ! -x "$CXBE" ] && [ ! -f "${CXBE}.exe" ]; then
		echo "link-check: $CXBE not found or not built." >&2
		echo "  Run: make -C deps/nxdk/tools/cxbe" >&2
		exit 1
	fi
	if ! "$CXBE" -OUT:"$XBE_OUT" -TITLE:"Resonance3D" "$XBE_EXE" > "$SCRATCH/xbe-cxbe.txt" 2>&1; then
		echo "link-check: cxbe conversion failed. See $SCRATCH/xbe-cxbe.txt" >&2
		tail -n 20 "$SCRATCH/xbe-cxbe.txt" >&2
		exit 1
	fi
	if [ ! -f "$XBE_OUT" ]; then
		echo "link-check: cxbe reported success but $XBE_OUT does not exist." >&2
		exit 1
	fi
	# And on the XBE header itself (SizeOfStackCommit, the value nxdk's
	# CreateThread(..., 0, ...) hands every thread) - divergence #47.
	if ! tools/xbox-check-stack.sh "$XBE_OUT" "$XBOX_STACK_SIZE"; then
		exit 1
	fi
	# XBEH, the real magic header every valid .xbe starts with - checked,
	# not assumed from cxbe's own exit code alone.
	MAGIC=$(head -c 4 "$XBE_OUT")
	if [ "$MAGIC" != "XBEH" ]; then
		echo "link-check: $XBE_OUT does not start with the XBEH magic header -" >&2
		echo "  cxbe reported success but the output does not look like a real XBE." >&2
		exit 1
	fi
	echo "link-check: $XBE_OUT is a real, valid XBE ($(wc -c < "$XBE_OUT") bytes)."
fi
