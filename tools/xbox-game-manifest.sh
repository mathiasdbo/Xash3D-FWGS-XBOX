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
# (headers are ignored - they need no compile step) from the target's own
# CMakeLists.txt (deps/hlsdk/dlls or deps/hlsdk/cl_dll), and
# target_compile_definitions() from TWO places, both real and both
# effective on the built target - the target's own CMakeLists.txt, and
# the shared `set_common_properties(TARGET_NAME)` function
# (deps/hlsdk/CMakeLists.txt) that both dlls/CMakeLists.txt and
# cl_dll/CMakeLists.txt call on their target. Found in Codex review: an
# earlier version of this script read only the per-target block, silently
# omitting `_CRT_SECURE_NO_WARNINGS`, `CLIENT_WEAPONS` and
# `UNIFIED_SDK_CONFIG` - `CLIENT_WEAPONS` is not cosmetic (it changes a
# weapon save-field's type, dlls/weapons.cpp:378-386, and gates client-side
# weapon prediction, cl_dll/hl/hl_weapons.cpp:910-915), so a manifest
# missing it would compile and link a functioning-looking but materially
# wrong build - the "clean sweep of the wrong configuration" failure mode
# this project has been burned by before. This project's own
# tools/xbox-cxx.sh compile route reads the resulting manifest rather than
# invoking any CMakeLists.txt through CMake itself, so all of this must be
# derived here, not left to CMake to resolve at configure time.
#
# `set_common_properties`'s defines block contains real CMake generator
# expressions (`$<...>`), which this script resolves against a single,
# fixed, explicitly-chosen target configuration rather than actually
# running CMake (X7.1's whole point is skipping CMake as a build step) -
# see RESOLVED_CONFIG/RESOLVED_PLATFORM below. Every genex SHAPE this
# script knows how to resolve is verified present, and every other shape
# is a hard parse error (never silently passed through or dropped) -
# checked by reading deps/hlsdk/CMakeLists.txt in full before writing
# this, not assumed.
#
# Also found, NOT fixed here (out of this script's scope - a project-wide
# compiler-flag concern, not a per-title one): `set_common_properties`
# also sets `target_compile_options` (`-fpermissive -fno-strict-aliasing
# -Wno-invalid-offsetof`, with its own comment already noting nxdk-cc
# needs them too) that `tools/xbox-cxx.sh` does not currently pass at all.
# This manifest format carries DEFINE/SOURCE only, by design (X7.1) - flag
# derivation, if it needs to happen per-title rather than as a fixed
# addition to xbox-cxx.sh itself, is a separate follow-up.
#
# Both CMakeLists.txt calls this script parses are single, un-nested
# function calls in this codebase (no nested calls; generator expressions
# use only `<`/`>`, never `(`/`)`, verified by reading all three files in
# full before writing this) - comments are stripped first (CMake's `#` is
# a real end-of-line comment, same as this script's own), then a block is
# isolated by counting parens on what remains, so a comment containing a
# stray "(" or ")" (this file's own header comments included) can never
# desync the count. Within an isolated block, individual CMake list items
# are split on whitespace at angle-bracket depth 0 only, so a genex like
# `$<$<PLATFORM_ID:Linux, Darwin>:POSIX _POSIX LINUX _LINUX GNUC>` (one
# CMake list item, despite its internal spaces - real, legal CMake syntax:
# whitespace inside a balanced `$<...>` does not end an unquoted argument)
# is never torn apart by a naive word-split.
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

# This project's own resolution of the two generator-expression axes
# actually used in deps/hlsdk/CMakeLists.txt's set_common_properties():
# CONFIG and PLATFORM_ID. Xbox via nxdk's toolchain-nxdk.cmake sets
# CMAKE_SYSTEM_NAME to "Generic" - deliberately not "Linux" or "Darwin",
# the only two PLATFORM_ID values this SDK's genexps ever test, so
# RESOLVED_PLATFORM's exact spelling only needs to be neither of those.
# There is no CMake configure step in this project's Xbox compile route
# (X7.1) and therefore no real Debug/Release distinction to inherit -
# RESOLVED_CONFIG picks Release, matching how a shipped title would
# actually be built (no debug asserts/extra logging), an explicit
# decision recorded here rather than left implicit.
RESOLVED_CONFIG="Release"
RESOLVED_PLATFORM="Xbox"

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
COMMON_CMAKELISTS="$SDK_DIR/CMakeLists.txt"
[ -f "$CMAKELISTS" ] || { echo "xbox-game-manifest: $CMAKELISTS does not exist" >&2; exit 1; }
[ -f "$COMMON_CMAKELISTS" ] || { echo "xbox-game-manifest: $COMMON_CMAKELISTS does not exist" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

sed 's/#.*//' "$CMAKELISTS" > "$WORK/stripped.txt"
sed 's/#.*//' "$COMMON_CMAKELISTS" > "$WORK/common_stripped.txt"

# extract_block FILE MARKER: prints the text of the single-call block
# starting at the first line of (comment-stripped) FILE containing MARKER
# (MARKER includes the call's own opening paren, e.g.
# "target_sources(hl PRIVATE"), up to and including its matching close
# paren. Errors out - rather than printing nothing - if MARKER is never
# found, so a CMakeLists.txt that stopped matching this project's assumed
# shape is a hard failure, not a silently empty manifest.
extract_block() {
	local file="$1" marker="$2"
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
	' "$file")"
	if [ -z "$out" ]; then
		echo "xbox-game-manifest: marker not found in $file: $marker" >&2
		exit 1
	fi
	printf '%s\n' "$out"
}

# split_cmake_args: reads block body text on stdin, emits one logical
# CMake list item per output line, splitting on whitespace only when
# angle-bracket ('<'/'>') depth is 0 - see the header comment on why a
# naive word-split is wrong here. None of the text this script ever feeds
# through this function contains '<'/'>' outside of `$<...>` generator
# expressions (verified by reading the source files), so plain
# angle-bracket counting is exact, not an approximation.
split_cmake_args() {
	awk '
		{ text = text $0 " " }
		END {
			depth = 0
			cur = ""
			n = length(text)
			for (i = 1; i <= n; i++) {
				c = substr(text, i, 1)
				if (c == "<") depth++
				else if (c == ">") depth--
				if ((c == " " || c == "\t") && depth == 0) {
					if (cur != "") { print cur; cur = "" }
				} else {
					cur = cur c
				}
			}
			if (cur != "") print cur
		}
	'
}

# resolve_define TOKEN: prints zero or more actual -D-shaped names for one
# CMake list item from a target_compile_definitions() call. A plain
# identifier (optionally NAME=value) passes through unchanged. A whole-item
# generator-expression gate ($<$<CONFIG:X>:...> or
# $<$<PLATFORM_ID:A,B,...>:...>) is resolved against RESOLVED_CONFIG/
# RESOLVED_PLATFORM: true emits its (whitespace-split) payload, false
# emits nothing. A literal `$<CONFIG>` substring inside an otherwise plain
# token (UNIFIED_SDK_CONFIG="$<CONFIG>") is substituted with
# RESOLVED_CONFIG. Any OTHER generator-expression shape is a hard error:
# this script only ever claims to understand the shapes it was actually
# measured against (deps/hlsdk/CMakeLists.txt's set_common_properties,
# read in full before writing this), never a guessed general CMake genex
# evaluator.
resolve_define() {
	local tok="$1"
	case "$tok" in
	'$<$<CONFIG:'*'>:'*'>')
		local cfg="${tok#\$<\$<CONFIG:}"
		cfg="${cfg%%>:*}"
		local payload="${tok#*>:}"
		payload="${payload%>}"
		if [ "$cfg" = "$RESOLVED_CONFIG" ] || { [ "$cfg" = "DEBUG" ] && [ "$RESOLVED_CONFIG" = "Debug" ]; }; then
			# CMake's CONFIG genex compares case-insensitively; this SDK
			# only ever spells it DEBUG/Debug, both handled explicitly
			# rather than doing a general case-fold for a single case.
			for p in $payload; do printf '%s\n' "$p"; done
		fi
		return
		;;
	esac
	case "$tok" in
	'$<$<PLATFORM_ID:'*'>:'*'>')
		local rest="${tok#\$<\$<PLATFORM_ID:}"
		local platforms="${rest%%>:*}"
		local payload="${rest#*>:}"
		payload="${payload%>}"
		local matched=0
		local old_ifs="$IFS"
		IFS=','
		for p in $platforms; do
			IFS="$old_ifs"
			p="$(printf '%s' "$p" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
			[ "$p" = "$RESOLVED_PLATFORM" ] && matched=1
			IFS=','
		done
		IFS="$old_ifs"
		if [ "$matched" -eq 1 ]; then
			for p in $payload; do printf '%s\n' "$p"; done
		fi
		return
		;;
	esac
	case "$tok" in
	*'$<CONFIG>'*)
		# Checked BEFORE the generic "contains $<...>" catch-all below:
		# that broader pattern also matches this shape
		# (UNIFIED_SDK_CONFIG="$<CONFIG>"), and case picks the first
		# matching branch - ordering it last would send every value
		# substitution straight into the "unrecognized" hard error.
		printf '%s\n' "${tok//\$<CONFIG>/$RESOLVED_CONFIG}"
		return
		;;
	esac
	case "$tok" in
	*'$<'*'>'*)
		echo "xbox-game-manifest: unrecognized generator expression in" >&2
		echo "  $COMMON_CMAKELISTS's target_compile_definitions(): '$tok' - this" >&2
		echo "  script only resolves the specific genex shapes it was measured" >&2
		echo "  against; fix resolve_define() rather than guessing its value." >&2
		exit 1
		;;
	*)
		printf '%s\n' "$tok"
		return
		;;
	esac
}

# Sources: target_sources(<cmake-target> PRIVATE <files...>)
SRC_MARKER="target_sources(${CMAKE_TARGET} PRIVATE"
SRC_BLOCK="$(extract_block "$WORK/stripped.txt" "$SRC_MARKER")"
SRC_BODY="${SRC_BLOCK#"$SRC_MARKER"}"
# Strip exactly the one trailing ')' that closes this call - it is always
# attached to the last token with no space before it (verified by reading
# both CMakeLists.txt files: every target_sources()/
# target_compile_definitions() call in this codebase closes this way).
SRC_BODY="$(printf '%s' "$SRC_BODY" | sed 's/)[[:space:]]*$//')"

SOURCES=()
while IFS= read -r tok; do
	[ -z "$tok" ] && continue
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
done <<< "$(printf '%s' "$SRC_BODY" | split_cmake_args)"

if [ "${#SOURCES[@]}" -eq 0 ]; then
	echo "xbox-game-manifest: zero .cpp sources parsed from $CMAKELISTS - refusing" >&2
	echo "  to write an empty manifest." >&2
	exit 1
fi

for f in "${SOURCES[@]}"; do
	[ -f "$f" ] || { echo "xbox-game-manifest: derived source does not exist: $f" >&2; exit 1; }
done

# Defines: target_compile_definitions(<cmake-target> PRIVATE <names...>)
# from the target's own CMakeLists.txt...
DEF_MARKER="target_compile_definitions(${CMAKE_TARGET} PRIVATE"
DEF_BLOCK="$(extract_block "$WORK/stripped.txt" "$DEF_MARKER")"
DEF_BODY="${DEF_BLOCK#"$DEF_MARKER"}"
DEF_BODY="$(printf '%s' "$DEF_BODY" | sed 's/)[[:space:]]*$//')"

# ...and from the shared set_common_properties(TARGET_NAME) function both
# dlls/CMakeLists.txt and cl_dll/CMakeLists.txt call on their target
# (deps/hlsdk/CMakeLists.txt) - see the header comment on why this is not
# optional (CLIENT_WEAPONS).
COMMON_DEF_MARKER='target_compile_definitions(${TARGET_NAME} PRIVATE'
COMMON_DEF_BLOCK="$(extract_block "$WORK/common_stripped.txt" "$COMMON_DEF_MARKER")"
COMMON_DEF_BODY="${COMMON_DEF_BLOCK#"$COMMON_DEF_MARKER"}"
COMMON_DEF_BODY="$(printf '%s' "$COMMON_DEF_BODY" | sed 's/)[[:space:]]*$//')"

DEFINES=()
while IFS= read -r tok; do
	[ -z "$tok" ] && continue
	DEFINES+=("$tok")
done <<< "$(printf '%s' "$DEF_BODY" | split_cmake_args)"

while IFS= read -r tok; do
	[ -z "$tok" ] && continue
	while IFS= read -r resolved; do
		[ -z "$resolved" ] && continue
		DEFINES+=("$resolved")
	done <<< "$(resolve_define "$tok")"
done <<< "$(printf '%s' "$COMMON_DEF_BODY" | split_cmake_args)"

if [ "${#DEFINES[@]}" -eq 0 ]; then
	echo "xbox-game-manifest: zero defines parsed from $CMAKELISTS + $COMMON_CMAKELISTS -" >&2
	echo "  refusing to write an empty manifest (VALVE_DLL/CLIENT_DLL/WIN32/" >&2
	echo "  CLIENT_WEAPONS are always expected; a title that genuinely needs" >&2
	echo "  none yet doesn't exist)." >&2
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
