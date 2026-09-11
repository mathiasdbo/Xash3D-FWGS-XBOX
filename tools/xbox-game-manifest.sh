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
# Include directories are derived the same way and for the same reason as
# defines: target_include_directories() from the target's own CMakeLists.txt,
# plus set_common_properties()'s own `target_include_directories(TARGET_NAME
# PRIVATE ${CMAKE_BINARY_DIR})`. ${CMAKE_CURRENT_SOURCE_DIR} (every target's
# own directory) resolves deterministically to $SDK_DIR/$SUBDIR - not a
# generator expression, an ordinary CMake variable with one fixed value per
# CMakeLists.txt. ${CMAKE_BINARY_DIR} is different: checked (`grep
# configure_file`) before deciding how to handle it, not assumed - nothing in
# deps/hlsdk's CMakeLists.txt ever configure_file()s a real header into any
# binary dir that a .cpp #includes (the two real configure_file() calls are
# an install script and cl_dll's Linux-only vgui/.so copies) - so unlike a
# real generated-header directory, adding a scratch path here would be
# inventing a value nothing needs rather than deriving a real one. Omitted,
# with this comment as the record of why, not a silent drop.
#
# Output: $SCRATCH/game-<title>/<target>.manifest (SCRATCH defaults to
# .sweep, matching tools/xbox-link-check.sh's own default), one directive
# per line, title/target recorded so a downstream consumer can refuse a
# mismatch outright rather than silently building the wrong thing (the
# XBOX_TITLE env-var lesson X7.1 already cites):
#   TITLE <title>
#   TARGET <server|client>
#   DEFINE <name>
#   INCLUDE <path, relative to the repo root>
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

# Resolve the output path and invalidate any PREVIOUS manifest immediately
# - before any parsing/validation below has even started, not after.
# Found in Codex review: every check in this script runs (and can exit 1)
# before the output write at the very end, and the old version never
# touched $OUT until that final step - so a run that fails midway (an
# unrecognized generator expression, a source/include that no longer
# exists, anything) left a PRIOR successful run's manifest sitting at the
# fixed path this project's own design (fork-plan.md's X7.1) has a
# downstream sweep read from, exactly the false-clean failure mode this
# project keeps getting burned by: the sweep would compile stale inputs,
# unaware regeneration had actually failed. Removing it up front makes
# "no manifest at $OUT" mean what the header comment and every negative
# test already claim it means - regeneration failed, full stop - for
# every failure path in this script, not just the ones written with that
# in mind.
OUT_DIR="$SCRATCH/game-$TITLE"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/$TARGET.manifest"
rm -f "$OUT"

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

# try_extract_block FILE MARKER: like extract_block, but prints nothing
# (does not error) when MARKER is absent - for calls that are genuinely
# optional in this codebase (target_link_libraries: dlls/CMakeLists.txt
# has none at all, cl_dll/CMakeLists.txt has one), unlike
# target_sources()/target_compile_definitions()/target_include_directories(),
# which are confirmed present in both per-target files and always required.
try_extract_block() {
	local file="$1" marker="$2"
	if ! grep -qF -- "$marker" "$file"; then
		return 0
	fi
	extract_block "$file" "$marker"
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

# resolve_include TOKEN: prints zero or one repo-relative directory for one
# CMake list item from a target_include_directories() call.
# ${CMAKE_CURRENT_SOURCE_DIR} resolves to this target's own directory
# ($SDK_DIR/$SUBDIR). ${CMAKE_BINARY_DIR} resolves to nothing (see the
# header comment - verified unused, not guessed). A `../x` entry resolves
# to $SDK_DIR/x; a bare entry resolves to $SDK_DIR/$SUBDIR/x. Any OTHER
# `${...}` CMake variable reference is a hard error, same posture as
# resolve_define's unrecognized-genex path - this script only claims to
# understand the two variable references actually present in the files it
# reads.
resolve_include() {
	local tok="$1"
	case "$tok" in
	'${CMAKE_CURRENT_SOURCE_DIR}')
		printf '%s\n' "$SDK_DIR/$SUBDIR"
		return
		;;
	'${CMAKE_CURRENT_SOURCE_DIR}/'*)
		# Found in Codex review: vgui/SDL2's own INTERFACE include dirs
		# (cl_dll/CMakeLists.txt:14-15) use this as a PATH PREFIX
		# (${CMAKE_CURRENT_SOURCE_DIR}/../utils/vgui/include), not as the
		# whole token the case above matches - substitute the prefix and
		# keep the rest. Resolves to $SDK_DIR/$SUBDIR because every real
		# occurrence of this shape lives inside a per-target CMakeLists.txt
		# (dlls/ or cl_dll/'s own); find_interface_includes guards the one
		# place this assumption could silently be wrong (a match inside
		# the ROOT CMakeLists.txt, where CMAKE_CURRENT_SOURCE_DIR would
		# actually be $SDK_DIR, not $SDK_DIR/$SUBDIR) with a hard error
		# rather than resolving this quietly wrong.
		printf '%s\n' "$SDK_DIR/$SUBDIR/${tok#'${CMAKE_CURRENT_SOURCE_DIR}/'}"
		return
		;;
	'${CMAKE_BINARY_DIR}')
		return
		;;
	'${'*)
		echo "xbox-game-manifest: unrecognized CMake variable in a" >&2
		echo "  target_include_directories() block: '$tok' - this script only" >&2
		echo "  resolves \${CMAKE_CURRENT_SOURCE_DIR} and \${CMAKE_BINARY_DIR}," >&2
		echo "  the two actually present in these files; fix resolve_include()" >&2
		echo "  rather than guessing this one's value." >&2
		exit 1
		;;
	../*)
		printf '%s\n' "$SDK_DIR/${tok#../}"
		return
		;;
	*)
		printf '%s\n' "$SDK_DIR/$SUBDIR/$tok"
		return
		;;
	esac
}

# find_interface_includes NAME: appends to the (already-declared, global)
# INCLUDES array every directory a `target_include_directories(NAME
# INTERFACE ...)` call declares for the locally-defined CMake target NAME,
# searching both the per-target and common (comment-stripped) files - a
# consumer of NAME via target_link_libraries() inherits these
# automatically under real CMake, and this script's own derivation would
# otherwise silently miss them the same way it silently missed
# set_common_properties()'s defines before Codex review round 1 caught
# that. Found in Codex review, this same round: `client` links `vgui` and
# `SDL2` (both `add_library(... IMPORTED)` targets cl_dll/CMakeLists.txt
# defines itself, each with its own INTERFACE include dir -
# `../utils/vgui/include` and `../external` respectively) - `../external`
# is NEVER listed in `client`'s own `target_include_directories()` call,
# only inherited this way, and `cdll_int.cpp` unconditionally `#include`s
# `<SDL2/SDL_messagebox.h>` from it. A name with no matching
# `target_include_directories(NAME INTERFACE ...)` anywhere (a raw system
# library like `${CMAKE_DL_LIBS}`, or a Windows-only import library like
# `delayimp`/`wsock32`) contributes nothing - not an error, since most
# link-library names legitimately have no such block.
find_interface_includes() {
	local name="$1"
	local marker="target_include_directories(${name} INTERFACE"
	local file
	for file in "$WORK/stripped.txt" "$WORK/common_stripped.txt"; do
		if ! grep -qF -- "$marker" "$file"; then
			continue
		fi
		local block body
		block="$(extract_block "$file" "$marker")"
		body="${block#"$marker"}"
		body="$(printf '%s' "$body" | sed 's/)[[:space:]]*$//')"
		if [ "$file" = "$WORK/common_stripped.txt" ] && printf '%s' "$body" | grep -qF 'CMAKE_CURRENT_SOURCE_DIR'; then
			# resolve_include's ${CMAKE_CURRENT_SOURCE_DIR} resolution
			# assumes the token came from a per-target CMakeLists.txt
			# (dlls/ or cl_dll/'s own, where it means $SDK_DIR/$SUBDIR) -
			# true for every real case today, but wrong for the ROOT
			# CMakeLists.txt (where it would mean $SDK_DIR itself). No
			# real case hits this yet; fail loud instead of resolving it
			# quietly wrong if one ever does.
			echo "xbox-game-manifest: $marker in $file uses" >&2
			echo "  \${CMAKE_CURRENT_SOURCE_DIR} - resolve_include()'s resolution" >&2
			echo "  for it assumes a per-target file, not the root CMakeLists.txt;" >&2
			echo "  fix resolve_include() before trusting this case." >&2
			exit 1
		fi
		while IFS= read -r tok; do
			[ -z "$tok" ] && continue
			resolved_output="$(resolve_include "$tok")" || exit 1
			[ -n "$resolved_output" ] && INCLUDES+=("$resolved_output")
		done <<< "$(printf '%s' "$body" | split_cmake_args)"
	done
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
	# Found in Codex review: resolve_define runs inside a command
	# substitution here, so its own `exit 1` (an unrecognized genex shape)
	# only kills THAT subshell - under bash, a failing command
	# substitution feeding a here-string does not, on its own, fail the
	# surrounding `while`/`<<<`, so the loop would silently see zero
	# resolved lines and continue as if this token had legitimately
	# resolved to nothing. Capture first, check explicitly, THEN iterate -
	# a resolver failure now aborts the whole script instead of quietly
	# reducing the define set.
	resolved_output="$(resolve_define "$tok")" || exit 1
	while IFS= read -r resolved; do
		[ -z "$resolved" ] && continue
		DEFINES+=("$resolved")
	done <<< "$resolved_output"
done <<< "$(printf '%s' "$COMMON_DEF_BODY" | split_cmake_args)"

if [ "${#DEFINES[@]}" -eq 0 ]; then
	echo "xbox-game-manifest: zero defines parsed from $CMAKELISTS + $COMMON_CMAKELISTS -" >&2
	echo "  refusing to write an empty manifest (VALVE_DLL/CLIENT_DLL/WIN32/" >&2
	echo "  CLIENT_WEAPONS are always expected; a title that genuinely needs" >&2
	echo "  none yet doesn't exist)." >&2
	exit 1
fi

# Includes: target_include_directories(<cmake-target> PRIVATE <dirs...>)
# from the target's own CMakeLists.txt...
INC_MARKER="target_include_directories(${CMAKE_TARGET} PRIVATE"
INC_BLOCK="$(extract_block "$WORK/stripped.txt" "$INC_MARKER")"
INC_BODY="${INC_BLOCK#"$INC_MARKER"}"
INC_BODY="$(printf '%s' "$INC_BODY" | sed 's/)[[:space:]]*$//')"

# ...and from the shared set_common_properties(TARGET_NAME) function - see
# the header comment on ${CMAKE_BINARY_DIR}.
COMMON_INC_MARKER='target_include_directories(${TARGET_NAME} PRIVATE'
COMMON_INC_BLOCK="$(extract_block "$WORK/common_stripped.txt" "$COMMON_INC_MARKER")"
COMMON_INC_BODY="${COMMON_INC_BLOCK#"$COMMON_INC_MARKER"}"
COMMON_INC_BODY="$(printf '%s' "$COMMON_INC_BODY" | sed 's/)[[:space:]]*$//')"

INCLUDES=()
while IFS= read -r tok; do
	[ -z "$tok" ] && continue
	resolved_output="$(resolve_include "$tok")" || exit 1
	[ -n "$resolved_output" ] && INCLUDES+=("$resolved_output")
done <<< "$(printf '%s' "$INC_BODY" | split_cmake_args)"

while IFS= read -r tok; do
	[ -z "$tok" ] && continue
	resolved_output="$(resolve_include "$tok")" || exit 1
	[ -n "$resolved_output" ] && INCLUDES+=("$resolved_output")
done <<< "$(printf '%s' "$COMMON_INC_BODY" | split_cmake_args)"

# Transitive INTERFACE include directories from target_link_libraries() -
# see find_interface_includes's own comment for why this is not optional
# (SDL2/../external, cdll_int.cpp's <SDL2/SDL_messagebox.h>, found in
# Codex review). target_link_libraries() itself is optional in this
# codebase (dlls/CMakeLists.txt has none at all) - try_extract_block
# tolerates its absence rather than erroring.
LINK_MARKER="target_link_libraries(${CMAKE_TARGET} PRIVATE"
LINK_BLOCK="$(try_extract_block "$WORK/stripped.txt" "$LINK_MARKER")"
if [ -n "$LINK_BLOCK" ]; then
	LINK_BODY="${LINK_BLOCK#"$LINK_MARKER"}"
	LINK_BODY="$(printf '%s' "$LINK_BODY" | sed 's/)[[:space:]]*$//')"
	while IFS= read -r tok; do
		[ -z "$tok" ] && continue
		case "$tok" in
		'$<'*)
			# Generator-expression-gated link-library names
			# ($<$<PLATFORM_ID:Windows>:delayimp>,
			# $<$<PLATFORM_ID:Windows>:wsock32> - the only two present)
			# never have a target_include_directories(NAME INTERFACE ...)
			# block anywhere in this SDK regardless of whether the genex
			# itself would resolve true here - verified, not assumed -
			# so there is nothing for find_interface_includes to find
			# either way; skip resolving the genex itself.
			continue
			;;
		esac
		find_interface_includes "$tok"
	done <<< "$(printf '%s' "$LINK_BODY" | split_cmake_args)"
fi

COMMON_LINK_MARKER='target_link_libraries(${TARGET_NAME} PRIVATE'
COMMON_LINK_BLOCK="$(try_extract_block "$WORK/common_stripped.txt" "$COMMON_LINK_MARKER")"
if [ -n "$COMMON_LINK_BLOCK" ]; then
	COMMON_LINK_BODY="${COMMON_LINK_BLOCK#"$COMMON_LINK_MARKER"}"
	COMMON_LINK_BODY="$(printf '%s' "$COMMON_LINK_BODY" | sed 's/)[[:space:]]*$//')"
	while IFS= read -r tok; do
		[ -z "$tok" ] && continue
		case "$tok" in
		'$<'*) continue ;;
		esac
		find_interface_includes "$tok"
	done <<< "$(printf '%s' "$COMMON_LINK_BODY" | split_cmake_args)"
fi

if [ "${#INCLUDES[@]}" -eq 0 ]; then
	echo "xbox-game-manifest: zero include directories parsed from $CMAKELISTS +" >&2
	echo "  $COMMON_CMAKELISTS - refusing to write an empty manifest." >&2
	exit 1
fi

# Normalize before dedup: vgui's INTERFACE include resolves via
# ${CMAKE_CURRENT_SOURCE_DIR}/../utils/vgui/include
# (deps/hlsdk/cl_dll/../utils/vgui/include, un-simplified) while client's
# own target_include_directories() lists the same real directory as
# ../utils/vgui/include (deps/hlsdk/utils/vgui/include) - the same
# directory on disk, spelled two different ways, which a plain string
# dedup would not catch. realpath -m (works on a path that may not
# resolve through a real target yet, though every one here already does)
# collapses both to one canonical spelling before counting/deduping.
NORMALIZED=()
for d in "${INCLUDES[@]}"; do
	NORMALIZED+=("$(realpath -m --relative-to=. "$d")")
done
INCLUDES=("${NORMALIZED[@]}")

# Dedup: the normalization above is exactly what turns vgui's two
# spellings into one - without it, this manifest would report more
# distinct include roots than it actually found.
mapfile -t INCLUDES < <(printf '%s\n' "${INCLUDES[@]}" | awk '!seen[$0]++')

for d in "${INCLUDES[@]}"; do
	[ -d "$d" ] || { echo "xbox-game-manifest: derived include directory does not exist: $d" >&2; exit 1; }
done

# Atomic publish: write to a temp file in the SAME directory (so the final
# rename is a same-filesystem, single-step replace, never a partial write
# a concurrent reader could observe) and rename into place only once every
# line has been written successfully. $OUT was already removed up front
# (see above), so between here and the rename there is genuinely no
# manifest at $OUT - not a stale one, not a truncated one.
OUT_TMP="$(mktemp "$OUT_DIR/.manifest.XXXXXX")"
trap 'rm -rf "$WORK" "$OUT_TMP"' EXIT

{
	echo "TITLE $TITLE"
	echo "TARGET $TARGET"
	for d in "${DEFINES[@]}"; do
		echo "DEFINE $d"
	done
	for i in "${INCLUDES[@]}"; do
		echo "INCLUDE $i"
	done
	for s in "${SOURCES[@]}"; do
		echo "SOURCE $s"
	done
} > "$OUT_TMP"

mv -f "$OUT_TMP" "$OUT"

echo "xbox-game-manifest: wrote ${#SOURCES[@]} source(s), ${#DEFINES[@]} define(s)," >&2
echo "  ${#INCLUDES[@]} include dir(s) to $OUT" >&2
echo "$OUT"
