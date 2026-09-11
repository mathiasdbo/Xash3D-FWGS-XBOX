#!/usr/bin/env bash
# Generate an exports.txt (tools/gen-library-tables.sh's input format) from
# a module's own COMPILED objects, not from a hand-written list.
#
# docs/r3d/fork-plan.md's X7.2: a title's server exports.txt is ~260 lines
# (every LINK_ENTITY_TO_CLASS entity, deps/hlsdk/dlls/util.h:100-102) that
# must never be typed by hand (project directive #6: no hardcoded classname
# lists where the real source has a mechanism) - this reads it back out of
# the linker's own view of the object, every build.
#
# Two independently real problems, both measured on real SDK objects before
# this script was written (X7.1b/X7.2's prototype, PR #36):
#
#   1. Not every real export shows up as a `/EXPORT:` COFF directive.
#      DLLEXPORT is `__declspec(dllexport)` almost everywhere in this SDK
#      (common/Platform.h:66), which clang does record as `/EXPORT:name`
#      (llvm-readobj --coff-directives) - but deps/hlsdk/dlls/h_export.cpp
#      locally redefines DLLEXPORT as `__stdcall` on WIN32 for
#      GiveFnptrsToDll, which is a hard-required server export
#      (engine/server/sv_game.c:5245-5255) and emits NO directive at all.
#      Its real linker symbol is stdcall-decorated (`_GiveFnptrsToDll@8`,
#      measured), not the plain cdecl name a naive `extern void name(void);`
#      would look for - so this script also scans the RAW symbol table
#      (llvm-nm) for stdcall-decorated forms of anything a caller marks
#      --require, and writes gen-library-tables.sh's optional second
#      column (`name asm_symbol`) so the generated extern binds to the
#      exact real symbol via an asm-label override, not a guessed
#      convention.
#
#   2. A C++-mangled `/EXPORT:` entry is normal, not an error. Ordinary
#      EXPORT-marked C++ methods (e.g. deps/hlsdk/dlls/crossbow.cpp's
#      CCrossbow::PrimaryAttack) produce mangled directives too - real SDK
#      behaviour, not a missing extern "C". This script silently drops
#      every mangled directive (anything starting with `?` after the i386
#      leading `_` is stripped) rather than treating its presence as a
#      failure; only a REQUIRED (--require) name actually missing fails
#      the build.
#
# Usage:
#   xbox-gen-exports.sh --all    --require <req-file> <out.txt> <obj>...
#   xbox-gen-exports.sh --allow <allow-file> --require <req-file> <out.txt> <obj>...
#
# --all     every unmangled real export found goes into <out.txt> (the
#           server case: ~260 entity classnames plus the API exports, none
#           of it known in advance - X7.2).
# --allow   only names present in <allow-file> (one per line) are written,
#           even if the objects export other real, unmangled names too
#           (the client case: cl_dll also compiles the shared dlls/*.cpp
#           weapon sources, whose entity classnames the engine never looks
#           up on the client - X7.1b's own allowlist, reused here so the
#           two tools can never disagree about what "the client's public
#           surface" means).
# --require <req-file>: names that MUST be found (in --all mode, on top of
#           whatever --all already writes; in --allow mode, checked against
#           what --allow already wrote as a completeness gate). One
#           requirement per line; a line may list alternatives separated by
#           `|` (`GetEntityAPI|GetEntityAPI2` - engine/server/sv_game.c:5235
#           only fails if BOTH are absent, not one). Found in Codex review:
#           the documented client invocation had originally omitted this,
#           which would let a wrong or incomplete client object set produce
#           an empty (or partial) table and still exit 0 - `client/exports.txt`
#           MUST be generated with `--require tools/xbox-client-required-exports.txt`
#           (the 37 `cdll_exports` names `cl_game.c`'s own load path treats
#           as fatal if any is missing, `cl_game.c:4059-4084`) in real use;
#           `--require` staying technically optional here is for this
#           script's own generality, not licence to skip it for a real
#           module.
# --allow-empty: without this, an <out.txt> with zero entries is an ERROR
#           (exit 1), not just a warning - found in Codex review: a silent
#           empty table passing the build is exactly the false-clean
#           measurement this project's own tools have shipped before
#           (docs/r3d/lessons.md). Pass this only for a module that is
#           genuinely, deliberately empty (gen-library-tables.sh's own
#           header comment already treats that as a real, intentional
#           state, not a placeholder) - never as a way to silence this
#           check.
#
# Requires llvm-readobj and llvm-nm (present at
# /c/msys64/mingw64/bin/{llvm-readobj,llvm-nm}.exe, same toolchain
# tools/xbox-cxx.sh already depends on). A tool failing on any ONE object
# (a truncated file, an object this build of llvm-readobj/-nm cannot parse)
# is fatal, not silently skipped - found in Codex review: naive
# `... 2>/dev/null | grep ... || true` swallows the INSPECTOR's own exit
# code (the pipeline's exit code is grep's), so a crashed llvm-readobj on
# one object looked identical to "this object legitimately exports
# nothing" and the export list would just quietly lose that object's
# names while the script still exited 0.
set -eu

READOBJ="/c/msys64/mingw64/bin/llvm-readobj.exe"
NM="/c/msys64/mingw64/bin/llvm-nm.exe"
for t in "$READOBJ" "$NM"; do
	if [ ! -x "$t" ]; then
		echo "xbox-gen-exports: $t not found or not executable." >&2
		exit 1
	fi
done

usage() {
	echo "usage: xbox-gen-exports.sh --all|--allow <file> [--require <file>] [--allow-empty] <out.txt> <obj>..." >&2
	exit 1
}

MODE=""
ALLOW_FILE=""
REQUIRE_FILE=""
ALLOW_EMPTY=0
OUT=""
OBJS=()

while [ $# -gt 0 ]; do
	case "$1" in
	--all)
		[ -n "$MODE" ] && { echo "xbox-gen-exports: --all/--allow given twice" >&2; exit 1; }
		MODE="all"
		shift
		;;
	--allow)
		[ -n "$MODE" ] && { echo "xbox-gen-exports: --all/--allow given twice" >&2; exit 1; }
		MODE="allow"
		ALLOW_FILE="${2:?--allow needs a file}"
		shift 2
		;;
	--require)
		REQUIRE_FILE="${2:?--require needs a file}"
		shift 2
		;;
	--allow-empty)
		ALLOW_EMPTY=1
		shift
		;;
	--)
		shift
		break
		;;
	-*)
		echo "xbox-gen-exports: unknown flag $1" >&2
		usage
		;;
	*)
		break
		;;
	esac
done

[ -z "$MODE" ] && usage
[ $# -ge 1 ] || usage
OUT="$1"
shift
[ $# -ge 1 ] || { echo "xbox-gen-exports: no object files given" >&2; exit 1; }
OBJS=("$@")

for o in "${OBJS[@]}"; do
	[ -f "$o" ] || { echo "xbox-gen-exports: object file does not exist: $o" >&2; exit 1; }
done
if [ "$MODE" = "allow" ] && [ ! -f "$ALLOW_FILE" ]; then
	echo "xbox-gen-exports: --allow file does not exist: $ALLOW_FILE" >&2
	exit 1
fi
if [ -n "$REQUIRE_FILE" ] && [ ! -f "$REQUIRE_FILE" ]; then
	echo "xbox-gen-exports: --require file does not exist: $REQUIRE_FILE" >&2
	exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- 1. Every unmangled /EXPORT: name across all objects, deduplicated ---
# i386 mangled (C++) names start with '?' once the leading '_' clang always
# adds is stripped - MSVC-ABI mangling, unrelated to the leading underscore
# every i386 cdecl/stdcall symbol also gets. A real cdecl C export here is
# always plain ASCII after that strip (LINK_ENTITY_TO_CLASS's `extern "C"`,
# common/Platform.h:66's `__declspec(dllexport)` path taken with WIN32
# defined - the caller is responsible for passing -DWIN32, same as
# tools/xbox-cxx.sh's other SDK callers already must, per the h_export.cpp
# _rotr trap documented in fork-plan.md's X7.2).
: > "$WORK/directive_raw.txt"
for o in "${OBJS[@]}"; do
	if ! "$READOBJ" --coff-directives "$o" > "$WORK/readobj_out.txt" 2> "$WORK/readobj_err.txt"; then
		echo "xbox-gen-exports: llvm-readobj failed on $o:" >&2
		cat "$WORK/readobj_err.txt" >&2
		exit 1
	fi
	cat "$WORK/readobj_out.txt" >> "$WORK/directive_raw.txt"
done
# A valid object with zero /EXPORT: directives is a legitimate, non-fatal
# result (grep exits 1 on no match) - distinct from the loop above, where a
# nonzero llvm-readobj exit already aborted the script for that object.
grep -o '/EXPORT:"[^"]*"\|/EXPORT:[^" ]*' "$WORK/directive_raw.txt" \
	| sed 's#^/EXPORT:##; s/^"//; s/"$//; s/^_//' \
	| grep -v '^?' | sort -u > "$WORK/directive_names.txt" \
	|| : > "$WORK/directive_names.txt"

# --- 2. Full raw defined+extern symbol table across all objects (for the
#        stdcall-fallback search only - never used to widen --all's output,
#        since an arbitrary internal symbol is not "exported" by any
#        definition this project uses). A failed llvm-nm on any one object
#        is fatal (see the header comment on tool-failure swallowing), not
#        silently treated as "no symbols in this object". ---
: > "$WORK/nm_raw.txt"
for o in "${OBJS[@]}"; do
	if ! "$NM" --defined-only --extern-only "$o" > "$WORK/nm_out.txt" 2> "$WORK/nm_err.txt"; then
		echo "xbox-gen-exports: llvm-nm failed on $o:" >&2
		cat "$WORK/nm_err.txt" >&2
		exit 1
	fi
	cat "$WORK/nm_out.txt" >> "$WORK/nm_raw.txt"
done
awk '{print $NF}' "$WORK/nm_raw.txt" | sort -u > "$WORK/all_syms.txt"

# find_symbol_for NAME: prints, on success, either NAME (plain cdecl form
# present) or "NAME asm_symbol" (only a stdcall-decorated form is present -
# `_NAME@<n>` for some digit sequence <n>, the byte count of the real
# signature's arguments, which this script does not need to know in
# advance: it is read directly off the symbol that already exists). Empty
# output + nonzero exit means neither form was found anywhere.
find_symbol_for() {
	local name="$1" hit
	if grep -qx "_${name}" "$WORK/all_syms.txt" 2>/dev/null; then
		echo "$name"
		return 0
	fi
	hit=$(grep -x "_${name}@[0-9]\+" "$WORK/all_syms.txt" 2>/dev/null | head -1) || true
	if [ -n "$hit" ]; then
		echo "$name $hit"
		return 0
	fi
	return 1
}

# --- 3. Write the base export list per mode ---
: > "$WORK/out_lines.txt"
if [ "$MODE" = "all" ]; then
	while IFS= read -r n; do
		[ -z "$n" ] && continue
		echo "$n" >> "$WORK/out_lines.txt"
	done < "$WORK/directive_names.txt"
else
	while IFS= read -r n; do
		[ -z "$n" ] && continue
		if line=$(find_symbol_for "$n"); then
			echo "$line" >> "$WORK/out_lines.txt"
		fi
	done < "$ALLOW_FILE"
fi

# --- 4. Completeness gate: every --require line (OR-group on '|') must
#        resolve to something already written, or be added via the
#        stdcall fallback (--all mode only - --allow mode's whole point is
#        that ONLY the allow-list's names are ever written, so a required
#        name has to already be on that list to pass; this script does not
#        silently widen an --allow module's public surface to satisfy a
#        completeness check). ---
MISSING=()
if [ -n "$REQUIRE_FILE" ]; then
	while IFS= read -r group; do
		[ -z "$group" ] && continue
		found=0
		IFS='|' read -ra ALTS <<< "$group"
		for alt in "${ALTS[@]}"; do
			if grep -qx "$alt" "$WORK/directive_names.txt" 2>/dev/null \
				|| grep -qE "^${alt}( |$)" "$WORK/out_lines.txt" 2>/dev/null; then
				found=1
				break
			fi
			if [ "$MODE" = "all" ] && line=$(find_symbol_for "$alt"); then
				echo "$line" >> "$WORK/out_lines.txt"
				found=1
				break
			fi
		done
		[ "$found" -eq 0 ] && MISSING+=("$group")
	done < "$REQUIRE_FILE"
fi

if [ "${#MISSING[@]}" -gt 0 ]; then
	echo "xbox-gen-exports: missing required export(s), not writing $OUT:" >&2
	for m in "${MISSING[@]}"; do
		echo "  $m" >&2
	done
	echo "  Searched ${#OBJS[@]} object(s); a name here means neither a" >&2
	echo "  plain nor a stdcall-decorated symbol for it was found anywhere" >&2
	echo "  in them." >&2
	exit 1
fi

sort -u "$WORK/out_lines.txt" > "$WORK/final.txt"
COUNT=$(wc -l < "$WORK/final.txt" | tr -d ' ')
if [ "$COUNT" -eq 0 ] && [ "$ALLOW_EMPTY" -ne 1 ]; then
	echo "xbox-gen-exports: ERROR - refusing to write $OUT with zero entries" >&2
	echo "  (no unmangled exports found in any of the ${#OBJS[@]} object(s) given)." >&2
	echo "  Pass --allow-empty if this module's public surface is genuinely," >&2
	echo "  deliberately empty; otherwise this means the wrong object set was" >&2
	echo "  given, or the module's real exports were lost some other way." >&2
	exit 1
fi
cp "$WORK/final.txt" "$OUT"
echo "xbox-gen-exports: wrote $COUNT export(s) to $OUT" >&2
echo "$OUT"
