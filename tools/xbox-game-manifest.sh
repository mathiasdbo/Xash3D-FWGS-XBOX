#!/usr/bin/env bash
# Derive a title's per-target translation-unit list and compile defines
# directly from its own HL SDK fork's CMakeLists.txt - never a hand-typed
# glob or list (project directive: no hardcoded list where the real
# source has a mechanism). docs/r3d/fork-plan.md's X7.1.
#
# Usage:
#   xbox-game-manifest.sh <hl> <server|client>
#
# Currently only `hl` (deps/hlsdk) is supported. deps/hlsdk-opfor and
# deps/hlsdk-bshift have NO CMakeLists.txt at all - measured 2026-09-11,
# not assumed (`find deps/hlsdk-opfor deps/hlsdk-bshift -iname
# CMakeLists.txt` finds nothing in either tree; they ship Visual Studio
# .vcxproj/.sln and Linux Makefile project files only). X7.1's plan text
# originally assumed "op4 and bshift add their own [target_sources/
# target_compile_definitions]" - that was wrong, corrected here: there is
# no per-target source list to read for them yet, and inventing one by
# guessing a file glob is exactly the kind of hardcoded list project
# directive #6 forbids. Their own manifest mechanism is X7.6 work.
#
# Reads target_sources(<cmake-target> PRIVATE ...) for the .cpp entries
# (headers are ignored - they need no compile step) and
# target_compile_definitions(<cmake-target> PRIVATE ...) for the defines,
# from the target's own CMakeLists.txt, verbatim. This project's
# tools/xbox-cxx.sh compile route reads the resulting manifest rather
# than invoking the CMakeLists.txt through CMake itself, so the two must
# never silently drift - and re-deriving after an SDK-side source-list
# edit (X7.3's VGUI removal, for instance) needs NO change to this
# script: it reflects whatever CMakeLists.txt says at derivation time,
# nothing hand-maintained in between.
#
# Both CMakeLists.txt calls this script parses are single, un-nested
# function calls in this codebase (no generator expressions, no nested
# calls, verified by reading both files in full before writing this) -
# comments are stripped first (CMake's `#` is a real end-of-line comment,
# same as this script's own), then the block is isolated by counting
# parens on what remains, so a comment containing a stray "(" or ")"
# (this file's own header comments included) can never desync the count.
#
# Output: $SCRATCH/game-<title>/<target>.manifest (SCRATCH defaults to
# .sweep, matching tools/xbox-link-check.sh's own default), one directive
# per line, title/target recorded so a downstream consumer can refuse a
# mismatch outright rather than silently building the wrong thing (the
# XBOX_TITLE env-var lesson X7.1 already cites):
#   TITLE <title>
#   TARGET <server|client>
#   DEFINE <name>
#   SOURCE <path, relative to the repo root>
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

SCRATCH="${SCRATCH:-.sweep}"

usage() {
	echo "usage: xbox-game-manifest.sh <hl> <server|client>" >&2
	exit 1
}

[ $# -eq 2 ] || usage
TITLE="$1"
TARGET="$2"

case "$TITLE" in
hl)
	SDK_DIR="deps/hlsdk"
	;;
op4 | bshift)
	echo "xbox-game-manifest: $TITLE not yet supported - deps/hlsdk-opfor and" >&2
	echo "  deps/hlsdk-bshift have no CMakeLists.txt (measured 2026-09-11), so" >&2
	echo "  there is no target_sources()/target_compile_definitions() to read" >&2
	echo "  from yet. See docs/r3d/fork-plan.md's X7.6." >&2
	exit 1
	;;
*)
	echo "xbox-game-manifest: unknown title '$TITLE' (expected hl, op4 or bshift)" >&2
	exit 1
	;;
esac

case "$TARGET" in
server)
	CMAKE_TARGET="hl"
	SUBDIR="dlls"
	;;
client)
	CMAKE_TARGET="client"
	SUBDIR="cl_dll"
	;;
*)
	echo "xbox-game-manifest: unknown target '$TARGET' (expected server or client)" >&2
	exit 1
	;;
esac

CMAKELISTS="$SDK_DIR/$SUBDIR/CMakeLists.txt"
[ -f "$CMAKELISTS" ] || { echo "xbox-game-manifest: $CMAKELISTS does not exist" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

sed 's/#.*//' "$CMAKELISTS" > "$WORK/stripped.txt"

# extract_block MARKER: prints the text of the single-call block starting
# at the first line containing MARKER (MARKER includes the call's own
# opening paren, e.g. "target_sources(hl PRIVATE"), up to and including
# its matching close paren, comments already gone. Errors out - rather
# than printing nothing - if MARKER is never found, so a CMakeLists.txt
# that stopped matching this project's assumed shape is a hard failure,
# not a silently empty manifest.
extract_block() {
	local marker="$1"
	local out
	out="$(awk -v marker="$marker" '
		BEGIN { depth = 0; active = 0 }
		{
			line = $0
			if (!active) {
				idx = index(line, marker)
				if (idx == 0) next
				active = 1
				line = substr(line, idx)
			}
			for (i = 1; i <= length(line); i++) {
				c = substr(line, i, 1)
				if (c == "(") depth++
				else if (c == ")") depth--
			}
			print line
			if (active && depth == 0) exit
		}
	' "$WORK/stripped.txt")"
	if [ -z "$out" ]; then
		echo "xbox-game-manifest: marker not found in $CMAKELISTS: $marker" >&2
		exit 1
	fi
	printf '%s\n' "$out"
}

# Sources: target_sources(<cmake-target> PRIVATE <files...>)
SRC_MARKER="target_sources(${CMAKE_TARGET} PRIVATE"
SRC_BLOCK="$(extract_block "$SRC_MARKER")"
SRC_BODY="${SRC_BLOCK#"$SRC_MARKER"}"
# Strip exactly the one trailing ')' that closes this call - it is always
# attached to the last token with no space before it (verified by reading
# both CMakeLists.txt files: every target_sources()/
# target_compile_definitions() call in this codebase closes this way).
SRC_BODY="$(printf '%s' "$SRC_BODY" | sed 's/)[[:space:]]*$//')"

SOURCES=()
for tok in $SRC_BODY; do
	case "$tok" in
	*.cpp)
		case "$tok" in
		../*)
			SOURCES+=("$SDK_DIR/${tok#../}")
			;;
		*)
			SOURCES+=("$SDK_DIR/$SUBDIR/$tok")
			;;
		esac
		;;
	*.h) ;; # header-only entries need no compile step
	*)
		echo "xbox-game-manifest: unrecognized token in $CMAKELISTS's" >&2
		echo "  target_sources() block: '$tok' (neither .cpp nor .h) - this" >&2
		echo "  script's parsing assumptions no longer match the file, fix" >&2
		echo "  the parser rather than silently dropping it." >&2
		exit 1
		;;
	esac
done

if [ "${#SOURCES[@]}" -eq 0 ]; then
	echo "xbox-game-manifest: zero .cpp sources parsed from $CMAKELISTS - refusing" >&2
	echo "  to write an empty manifest." >&2
	exit 1
fi

for f in "${SOURCES[@]}"; do
	[ -f "$f" ] || { echo "xbox-game-manifest: derived source does not exist: $f" >&2; exit 1; }
done

# Defines: target_compile_definitions(<cmake-target> PRIVATE <names...>)
DEF_MARKER="target_compile_definitions(${CMAKE_TARGET} PRIVATE"
DEF_BLOCK="$(extract_block "$DEF_MARKER")"
DEF_BODY="${DEF_BLOCK#"$DEF_MARKER"}"
DEF_BODY="$(printf '%s' "$DEF_BODY" | sed 's/)[[:space:]]*$//')"

DEFINES=()
for tok in $DEF_BODY; do
	DEFINES+=("$tok")
done

if [ "${#DEFINES[@]}" -eq 0 ]; then
	echo "xbox-game-manifest: zero defines parsed from $CMAKELISTS - refusing to" >&2
	echo "  write an empty manifest (VALVE_DLL/CLIENT_DLL/WIN32 are always" >&2
	echo "  expected; a title that genuinely needs none yet doesn't exist)." >&2
	exit 1
fi

OUT_DIR="$SCRATCH/game-$TITLE"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/$TARGET.manifest"

{
	echo "TITLE $TITLE"
	echo "TARGET $TARGET"
	for d in "${DEFINES[@]}"; do
		echo "DEFINE $d"
	done
	for s in "${SOURCES[@]}"; do
		echo "SOURCE $s"
	done
} > "$OUT"

echo "xbox-game-manifest: wrote ${#SOURCES[@]} source(s), ${#DEFINES[@]} define(s) to $OUT" >&2
echo "$OUT"
