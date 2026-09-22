#!/usr/bin/env bash
# xbox-ref-isolate.sh - compile one upstream renderer module (ref/<module> +
# ref/common) for Xbox and rename EVERY symbol it defines with a
# <module>_ prefix, so it can be statically linked next to the engine and
# next to other renderer modules.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-ref-isolate.sh soft
#
# Why (X5, measured): linking ref/soft's 24 objects as-is next to the engine
# and ref_null gave 20 duplicate symbols - R_Init/R_Shutdown (the engine's
# ref_common.c has its own), the Tri* triangle API (engine/client has its
# own), _Mem_Alloc/_Mem_Free/_Mem_Realloc (ref/common/ref_common.h's
# wrappers vs engine/common/zone.c), the gl_vsync/gl_clear/r_decals/
# r_showtextures/r_showhull/host_allow_materials convar_t definitions the
# engine also owns, and GetRefAPI itself (ref_null's). Upstream never sees
# any of this: each ref is its own shared library, and its own static-lib
# path (scripts/waifulib/xshlib.py:36, `objcopy -G lib_${NAME}_exports`)
# localises everything but the export table for the same reason.
#
# Same mechanism as tools/xbox-game-isolate.sh (X7.1b), minus the allowlist:
# a renderer module has exactly one entry point the engine resolves by name
# (GetRefAPI, through the generated static library table), and that one is
# renamed too - the module's ref/<module>/exports.txt names the renamed
# linker symbol in its second column ("GetRefAPI _<module>_GetRefAPI"), the
# asm-name form tools/gen-library-tables.sh already supports for
# GiveFnptrsToDll@8 - so the table's "GetRefAPI" row points at THIS
# module's copy and ref_null's stays untouched. Nothing else in a ref module
# is referenced by name from outside: every engine service comes in through
# the ref_api_t function table, every ref service goes out through
# ref_interface_t, and llvm-objcopy --redefine-syms renames references and
# definitions alike, so intra-module calls stay consistent. Undefined
# references to the C runtime (memcpy, sin, ...) are not in the module's
# defined set and are left alone.
#
# Output: $SCRATCH/ref-<module>-isolated/*.obj, rename-map.txt, manifest.txt.
# The generated export table for the module is NOT built here - tools/
# xbox-cc.sh generates generated_lib_ref_<module>_exports.c from the
# exports.txt on any compile, and tools/xbox-link-check.sh compiles and
# links it next to these objects.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

MODULE="${1:?usage: xbox-ref-isolate.sh <module>   (ref/<module>/ must exist)}"
if [ ! -d "ref/$MODULE" ] || [ ! -f "ref/$MODULE/exports.txt" ]; then
	echo "xbox-ref-isolate: ref/$MODULE/ with an exports.txt does not exist" >&2
	exit 1
fi
PREFIX="$MODULE"

MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-ref-isolate: $MSYS2_BIN not found - see tools/xbox-env.sh." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
NM="$MSYS2_BIN/llvm-nm.exe"
OBJCOPY="$MSYS2_BIN/llvm-objcopy.exe"
for t in "$NM" "$OBJCOPY"; do
	[ -x "$t" ] || { echo "xbox-ref-isolate: $t not found or not executable." >&2; exit 1; }
done

SCRATCH="${SCRATCH:-$REPO/.sweep}"
ISO_DIR="$SCRATCH/ref-$MODULE-isolated"
mkdir -p "$ISO_DIR" "$SCRATCH/tmp" || exit 1
if ! rm -f "$ISO_DIR"/*.obj "$ISO_DIR/rename-map.txt" "$ISO_DIR/manifest.txt"; then
	echo "xbox-ref-isolate: could not clear stale output in $ISO_DIR" >&2
	exit 1
fi
WORK="$(mktemp -d -p "$SCRATCH/tmp")"
trap 'rm -rf "$WORK"' EXIT

# Step 1: compile, fresh, through tools/xbox-cc.sh's own ref/ rule
# (REF_DLL=1, ref/<module> + ref/common ahead of the engine include set).
SRCS=()
for f in ref/common/*.c "ref/$MODULE"/*.c; do
	[ -f "$f" ] && SRCS+=("$f")
done
OK=0
FAIL=0
: > "$WORK/errors.txt"
for f in "${SRCS[@]}"; do
	obj="$ISO_DIR/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.obj/')"
	if tools/xbox-cc.sh "$f" -o "$obj" 2>> "$WORK/errors.txt"; then
		OK=$((OK + 1))
	else
		FAIL=$((FAIL + 1))
		echo "  FAIL $f" >&2
	fi
done
echo "xbox-ref-isolate: ref/$MODULE + ref/common: $OK ok, $FAIL failing of ${#SRCS[@]}" >&2
if [ "$FAIL" -ne 0 ]; then
	cat "$WORK/errors.txt" >&2
	echo "xbox-ref-isolate: refusing to isolate a module that did not compile clean." >&2
	exit 2
fi

# Step 2: strip .drectve before renaming (X7.1b's measured Gap 2: a stale
# /EXPORT: directive on a renamed-away symbol corrupts the export table
# instead of erroring). Objects with no such section are the normal case.
for o in "$ISO_DIR"/*.obj; do
	if ! "$OBJCOPY" --remove-section=.drectve "$o" 2> "$WORK/strip_err.txt"; then
		if ! grep -qi "does not have section\|no such section\|not found" "$WORK/strip_err.txt"; then
			echo "xbox-ref-isolate: llvm-objcopy --remove-section failed on $o:" >&2
			cat "$WORK/strip_err.txt" >&2
			exit 1
		fi
	fi
done

# Step 3: rename map - every defined extern symbol, no allowlist. One
# persistent descriptor for the accumulation (tools/xbox-game-isolate.sh's
# own measured reason: `>>` reopened per object lost entries on this host).
: > "$WORK/all_defined.txt"
exec 4>> "$WORK/all_defined.txt"
for o in "$ISO_DIR"/*.obj; do
	if ! "$NM" --defined-only --extern-only "$o" > "$WORK/nm_out.txt" 2> "$WORK/nm_err.txt"; then
		echo "xbox-ref-isolate: llvm-nm failed on $o:" >&2
		cat "$WORK/nm_err.txt" >&2
		exit 1
	fi
	awk '{print $NF}' "$WORK/nm_out.txt" >&4
done
exec 4>&-
sort -u "$WORK/all_defined.txt" > "$WORK/all_defined_sorted.txt"
if [ ! -s "$WORK/all_defined_sorted.txt" ]; then
	echo "xbox-ref-isolate: zero defined symbols across the module - impossible." >&2
	exit 1
fi
# ??_C@... are COFF COMDAT string-literal symbols (clang's MSVC-style
# mangling for string constants, selectany): identical literals across
# every object in the link are meant to fold into one copy, and a literal
# cannot collide with anything by name in the sense that matters here.
# Left alone, in the map and in the leak check below.
RENAME_MAP="$ISO_DIR/rename-map.txt"
# The prefix goes AFTER the i386 leading underscore ("_GetRefAPI" ->
# "_soft_GetRefAPI"), so the renamed symbol is still the ordinary
# decoration of a C identifier (soft_GetRefAPI) and exports.txt's asm
# column reads naturally; a raw name with no underscore keeps a plain
# "soft_" prefix.
awk -v prefix="$PREFIX" '
	/^\?\?_C@/ { next }
	/^_/       { print $0 " _" prefix "_" substr($0, 2); next }
	           { print $0 " " prefix "_" $0 }
' "$WORK/all_defined_sorted.txt" > "$RENAME_MAP"
TOTAL=$(wc -l < "$RENAME_MAP" | tr -d ' ')

for o in "$ISO_DIR"/*.obj; do
	if ! "$OBJCOPY" --redefine-syms="$RENAME_MAP" "$o" 2> "$WORK/redefine_err.txt"; then
		echo "xbox-ref-isolate: llvm-objcopy --redefine-syms failed on $o:" >&2
		cat "$WORK/redefine_err.txt" >&2
		exit 1
	fi
done

# Step 4: self-check - every symbol still defined must carry the prefix,
# and the one name the engine will look up (exports.txt's second column)
# must actually exist now.
#
# Real bug, found and root-caused here, not assumed: this whole isolate
# step (both `soft` and `nv2a`) reported "N symbol(s) leaked" every time
# it ran, with the printed list already showing the CORRECT prefix on
# every entry (e.g. "_soft_GetRefAPI" listed as failing a check for
# "starts with soft_"). Chased through three wrong hypotheses first (a
# stale re-read of a just-written text file; a stale re-read of the
# just-`objcopy`'d .obj files themselves, "fixed" by inserting `sync`;
# neither changed the result) before isolating the real cause with a
# byte-level, side-by-side test: `_\?` (the BRE "optional preceding
# character" extension, GNU-only, not POSIX) is silently treated as a
# LITERAL backslash-question-mark by /c/msys64/usr/bin/grep.exe - the
# grep this script's own PATH export resolves to - even though it
# self-reports the identical "GNU grep 3.0" version string this
# project's other interactive shells use, where `\?` DOES work. `echo
# _soft_GetRefAPI | grep "^_\?soft_"` (msys64's own grep.exe, by full
# path) matches nothing; the same test through `grep -E "^_?soft_"`
# (extended regex, portable, no backslash escape needed) matches
# correctly on both grep builds. The same class of trap this project's
# own lessons.md already has an entry for (tools/xbox-check-nxdk-cc-
# drift.sh: "PATH, not the pattern, first" - the exact grep/sed pipeline
# giving silently different results depending on which binary the
# invoking shell's PATH resolves to) - a second, independent instance of
# it, in a different script, not yet fixed here. Every symbol this
# module's own object files genuinely carry HAS always had the correct
# prefix; only this check's own pattern was wrong, on this one grep
# build, the entire time. Extended regex (grep -E) with a plain,
# unescaped question mark, below, is the portable fix - do not
# reintroduce the backslash-escaped form here.
#
# Persistent fd 4, opened once and closed once, not reopened via `>>`
# once per loop iteration - this project's own lessons.md ("Reopening
# the SAME file for append once per iteration... silently loses data on
# this setup") already names that exact pattern as unreliable here.
# codex review round 2, real finding: an earlier version of this loop
# silenced llvm-nm's own stderr (`2>/dev/null`) and never checked its
# exit status - the exact "a measuring tool that cannot report its own
# failure will eventually lie" shape this project's own lessons.md
# already warns about. A failing nm on one rewritten object would have
# silently dropped that object's own symbols from post_defined.txt,
# letting a genuinely leaked symbol in THAT object go uncounted while
# the script still reported LEAKED=0. Checked the same way Step 3's own
# nm loop already is (:117-124) - capture stdout/stderr separately,
# abort loudly on any nonzero exit, never swallow the error.
LEAKED=0
: > "$WORK/post_defined.txt"
exec 4>> "$WORK/post_defined.txt"
for o in "$ISO_DIR"/*.obj; do
	if ! "$NM" --defined-only --extern-only "$o" > "$WORK/nm_out2.txt" 2> "$WORK/nm_err2.txt"; then
		echo "xbox-ref-isolate: llvm-nm failed on $o (post-rewrite scan):" >&2
		cat "$WORK/nm_err2.txt" >&2
		exit 1
	fi
	awk '{print $NF}' "$WORK/nm_out2.txt" >&4
done
exec 4>&-
if [ ! -s "$WORK/post_defined.txt" ]; then
	echo "xbox-ref-isolate: zero defined symbols after rewrite - impossible." >&2
	exit 1
fi
LEAKED=$(grep -v "^??_C@" "$WORK/post_defined.txt" | grep -Evc "^_?${PREFIX}_" || true)
if [ "$LEAKED" -ne 0 ]; then
	echo "xbox-ref-isolate: $LEAKED symbol(s) leaked without the ${PREFIX}_ prefix:" >&2
	grep -v "^??_C@" "$WORK/post_defined.txt" | grep -Ev "^_?${PREFIX}_" | head -20 >&2
	exit 1
fi
MISSING=0
while IFS=' ' read -r fn asm_sym || [ -n "$fn" ]; do
	[ -z "$fn" ] && continue
	if [ -z "${asm_sym:-}" ]; then
		echo "xbox-ref-isolate: ref/$MODULE/exports.txt row '$fn' has no asm-symbol column -" >&2
		echo "  after isolation the real symbol is _${PREFIX}_${fn}; the row must say so." >&2
		MISSING=$((MISSING + 1))
	elif ! grep -qx "$asm_sym" "$WORK/post_defined.txt"; then
		echo "xbox-ref-isolate: exports.txt names $asm_sym for $fn, but no isolated object defines it." >&2
		MISSING=$((MISSING + 1))
	fi
done < "ref/$MODULE/exports.txt"
[ "$MISSING" -eq 0 ] || exit 1

printf 'MODULE=%s OBJECTS=%s RENAMED=%s LEAKED=0\n' "$MODULE" "$OK" "$TOTAL" > "$ISO_DIR/manifest.txt"
echo "xbox-ref-isolate: ref_$MODULE isolated cleanly - $OK object(s), $TOTAL symbol(s)" >&2
echo "  renamed with the ${PREFIX}_ prefix, 0 leaked. Output: $ISO_DIR" >&2
echo "$ISO_DIR"
