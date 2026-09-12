#!/usr/bin/env bash
# Module symbol isolation: strip and rename a compiled title module's own
# object set so its internals cannot collide with the other side's in the
# link - the real, bulk implementation of the mechanism X7.1b prototyped
# by hand on two objects (crossbow.cpp, server side and client side) and
# proved against the real 384-object client response file.
# docs/r3d/fork-plan.md's X7.1b.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-game-isolate.sh <hl> <server|client> [--expect-fail N] [--expect-total N]
#
# Order, fixed by X7.1b's own measurement, not incidental:
#   1. compile (this script runs tools/xbox-game-sweep.sh itself first,
#      never trusting objects an earlier, possibly-stale invocation left
#      behind - --expect-fail/--expect-total are forwarded to it verbatim,
#      since sweep.sh itself now refuses a bare run with unacknowledged
#      failures).
#   2. tools/xbox-gen-exports.sh reads each object's /EXPORT: directives -
#      the LAST point they are ever read.
#   3. llvm-objcopy --remove-section=.drectve strips them from every
#      object - this project's static-linking mechanism never consumes a
#      PE export table, and a stale /EXPORT: on a symbol the next step
#      renames away corrupts the final image's export table instead of
#      erroring (X7.1b's own measured Gap 2).
#   4. llvm-nm --defined-only --extern-only over the stripped objects
#      produces the module's own defined-symbol set; every name NOT on
#      the module's allowlist is written to a rename map with a sv_/cl_
#      prefix, and llvm-objcopy --redefine-syms applies it to every
#      object of that module.
#
# The allowlist is the set of names the ENGINE resolves for THAT module,
# and nothing else (X7.1b, corrected in Codex review round 2 of that
# plan section):
#   server: exactly the server objects' own /EXPORT: set - the API
#           exports plus every LINK_ENTITY_TO_CLASS entity classname,
#           read from tools/xbox-gen-exports.sh's OWN --all output for
#           THIS object set (there is no fixed file to compare against;
#           entity classnames are discovered, not enumerated).
#   client: cl_game.c's cdll_exports[]+cdll_new_exports[] names and
#           NOTHING from the client objects' own /EXPORT: set beyond
#           those - the FIXED tools/xbox-client-allowed-exports.txt file
#           itself, not the (possibly smaller) --allow-mode-filtered
#           exports.txt this script also generates for X7.2's own
#           purposes: what matters for renaming is which names the
#           engine will ever look up by that exact string, not which of
#           them this particular object set happens to define.
#
# nm/objcopy convention, measured in X7.1b, not guessed: an i386 plain C
# symbol carries a leading underscore in the object's real symbol table
# (`_weapon_crossbow`); an MSVC-mangled C++ symbol does not
# (`?PrimaryAttack@CCrossbow@@...`) - the `?` sigil is the whole
# decoration, nothing is prepended on top of it. Comparing against an
# allowlist file (which stores bare, unprefixed names, matching
# tools/xbox-gen-exports.sh's own convention) strips AT MOST one leading
# underscore first; building the rename map's NEW name applies the
# prefix to the symbol exactly as llvm-nm printed it, underscore
# included if present - `sv_`/`cl_` + `_weapon_crossbow` intentionally
# yields the double-underscore `sv__weapon_crossbow`/`cl__weapon_crossbow`
# X7.1b's own prototype measured and required.
set -eu

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
		echo "xbox-game-isolate: unrecognised argument: $1" >&2
		exit 1
		;;
	*)
		POSITIONAL+=("$1")
		shift
		;;
	esac
done

if [ "${#POSITIONAL[@]}" -ne 2 ]; then
	echo "usage: xbox-game-isolate.sh <hl> <server|client> [--expect-fail N] [--expect-total N]" >&2
	exit 1
fi
TITLE="${POSITIONAL[0]}"
TARGET="${POSITIONAL[1]}"

case "$TARGET" in
server) PREFIX="sv"; REQUIRE_FILE="tools/xbox-server-required-exports.txt" ;;
client) PREFIX="cl"; REQUIRE_FILE="tools/xbox-client-required-exports.txt" ;;
*)
	echo "xbox-game-isolate: unknown target '$TARGET' (expected server or client)" >&2
	exit 1
	;;
esac
[ -f "$REQUIRE_FILE" ] || { echo "xbox-game-isolate: $REQUIRE_FILE does not exist" >&2; exit 1; }

MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-game-isolate: $MSYS2_BIN not found." >&2
	echo "  This project requires MSYS2 installed at C:\\msys64 - see tools/xbox-env.sh." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

READOBJ="$MSYS2_BIN/llvm-readobj.exe"
NM="$MSYS2_BIN/llvm-nm.exe"
OBJCOPY="$MSYS2_BIN/llvm-objcopy.exe"
for t in "$READOBJ" "$NM" "$OBJCOPY"; do
	if [ ! -x "$t" ]; then
		echo "xbox-game-isolate: $t not found or not executable." >&2
		exit 1
	fi
done

SCRATCH="${SCRATCH:-$REPO/.sweep}"

# Step 1: compile, fresh - never trust objects an earlier invocation left
# behind (the same reasoning tools/xbox-game-sweep.sh already applies to
# the manifest it reads).
echo "xbox-game-isolate: running the $TITLE/$TARGET sweep first..." >&2
SWEEP_ARGS=("$TITLE" "$TARGET")
[ -n "$EXPECT_FAIL" ] && SWEEP_ARGS+=(--expect-fail "$EXPECT_FAIL")
[ -n "$EXPECT_TOTAL" ] && SWEEP_ARGS+=(--expect-total "$EXPECT_TOTAL")
if ! bash tools/xbox-game-sweep.sh "${SWEEP_ARGS[@]}"; then
	echo "xbox-game-isolate: tools/xbox-game-sweep.sh did not certify a compiled" >&2
	echo "  object set for $TITLE/$TARGET - refusing to isolate a set the sweep" >&2
	echo "  itself did not accept (pass --expect-fail/--expect-total if the" >&2
	echo "  current baseline includes known, tracked failures)." >&2
	exit 1
fi

OBJ_DIR="$SCRATCH/game-$TITLE/$TARGET-objs"
OBJS=()
for o in "$OBJ_DIR"/*.obj; do
	[ -f "$o" ] && OBJS+=("$o")
done
if [ "${#OBJS[@]}" -eq 0 ]; then
	echo "xbox-game-isolate: no .obj files in $OBJ_DIR after a certified sweep -" >&2
	echo "  this should be impossible (the sweep already refuses zero attempted)." >&2
	exit 1
fi

# Found in Codex review: --expect-fail lets the sweep above CERTIFY a
# known-partial object set (this project's own established pattern for
# tracking real, unfixed compile failures - tools/xbox-mainui-sweep.sh
# and tools/xbox-game-sweep.sh both allow it deliberately), but this
# script was then publishing that partial set's isolation result with no
# machine-readable trace of the gap - "isolated cleanly" and a
# manifest.txt carrying only object/symbol counts, indistinguishable from
# a genuinely complete, link-ready module to any future automated
# consumer. Read the sweep's OWN manifest.txt (OK/FAIL/TOTAL, already
# written by tools/xbox-game-sweep.sh) back here rather than re-deriving
# it, and carry SWEEP_FAIL/SWEEP_TOTAL and an explicit COMPLETE flag
# through to this tool's own manifest.txt and terminal messaging, so a
# partial isolation can never read as equivalent to a complete one -
# Codex's own offered alternative to refusing to publish anything at all
# for a partial run, chosen because proving this tool's mechanism against
# a known, tracked-partial baseline (exactly what this session's own
# server run already did, honestly, in the plan's prose - just not yet
# in the artifact itself) has real value X7.4's own bring-up work relies
# on, and does not need to wait for every compile failure to be fixed
# first.
SWEEP_MANIFEST="$OBJ_DIR/manifest.txt"
[ -f "$SWEEP_MANIFEST" ] || { echo "xbox-game-isolate: $SWEEP_MANIFEST missing after a certified sweep - impossible." >&2; exit 1; }
SWEEP_OK=""
SWEEP_FAIL=""
SWEEP_TOTAL=""
for tok in $(cat "$SWEEP_MANIFEST"); do
	case "$tok" in
	OK=*) SWEEP_OK="${tok#OK=}" ;;
	FAIL=*) SWEEP_FAIL="${tok#FAIL=}" ;;
	TOTAL=*) SWEEP_TOTAL="${tok#TOTAL=}" ;;
	esac
done
if [ -z "$SWEEP_FAIL" ] || [ -z "$SWEEP_TOTAL" ]; then
	echo "xbox-game-isolate: could not parse OK/FAIL/TOTAL from $SWEEP_MANIFEST" >&2
	exit 1
fi
echo "xbox-game-isolate: isolating ${#OBJS[@]} compiled object(s)" >&2

ISO_DIR="$SCRATCH/game-$TITLE/$TARGET-isolated"
mkdir -p "$ISO_DIR" || { echo "xbox-game-isolate: cannot create $ISO_DIR" >&2; exit 1; }
if ! rm -f "$ISO_DIR"/*.obj "$ISO_DIR/exports.txt" "$ISO_DIR/rename-map.txt" "$ISO_DIR/manifest.txt"; then
	echo "xbox-game-isolate: could not clear stale output in $ISO_DIR - refusing" >&2
	echo "  to isolate into a directory that may still hold an earlier run's" >&2
	echo "  objects." >&2
	exit 1
fi
for stale in "$ISO_DIR"/*.obj; do
	if [ -e "$stale" ]; then
		echo "xbox-game-isolate: $stale survived cleanup - refusing to proceed." >&2
		exit 1
	fi
	break
done

# Rooted under $SCRATCH, not the system temp directory (mktemp -d's
# default) - found by running this for real: a file written here early
# (the server allowlist, below) and read back only after step 3's ~200
# external process invocations (cp + llvm-objcopy per object) had run in
# between came back "No such file or directory" - the same class of
# Windows/MSYS file-I/O unreliability under load already found and fixed
# once this session in tools/xbox-game-sweep.sh's log (there: reopening
# one file for append ~100 times; here: a single write followed by a
# long gap before the read). $SCRATCH-rooted paths have been reliable
# throughout this project's own tooling; the system temp directory,
# measured directly in this same session, has not.
mkdir -p "$SCRATCH/tmp"
WORK="$(mktemp -d -p "$SCRATCH/tmp")"
trap 'rm -rf "$WORK"' EXIT

# Step 2: X7.2's generator - the LAST point /EXPORT: directives are read,
# before they are stripped in step 3. Also determines this run's KEEP
# allowlist (see the header comment: server uses this output directly;
# client uses the fixed allow-list file instead, generating this only for
# X7.2's own downstream purposes).
EXPORTS_OUT="$ISO_DIR/exports.txt"
case "$TARGET" in
server)
	bash tools/xbox-gen-exports.sh --all \
		--require tools/xbox-server-required-exports.txt \
		"$EXPORTS_OUT" "${OBJS[@]}" >&2
	ALLOW_FILE="$EXPORTS_OUT"
	;;
client)
	bash tools/xbox-gen-exports.sh --allow tools/xbox-client-allowed-exports.txt \
		--require tools/xbox-client-required-exports.txt \
		"$EXPORTS_OUT" "${OBJS[@]}" >&2
	ALLOW_FILE="$WORK/client_allow_names.txt"
	# tools/xbox-client-allowed-exports.txt's lines are "name" only,
	# exports.txt's own generated lines can carry an optional second
	# field (the GiveFnptrsToDll-style asm-symbol column) - the allowlist
	# comparison below only ever wants the bare name, so both are read
	# the same way (first whitespace-separated field), but the FIXED file
	# is the actual source of truth here per the header comment, not the
	# generated one.
	awk '{print $1}' tools/xbox-client-allowed-exports.txt > "$ALLOW_FILE"
	;;
esac
if [ "$TARGET" = "server" ]; then
	# Found in Codex review: exports.txt's optional second column
	# (GiveFnptrsToDll's stdcall case, name asm_symbol - e.g.
	# "GiveFnptrsToDll _GiveFnptrsToDll@8") is not just a label on column
	# one's name - for a row that HAS one, column one is not a real
	# symbol in the object AT ALL (GiveFnptrsToDll has no plain cdecl
	# form or /EXPORT: directive, only the stdcall-decorated one exists -
	# X7.2's own finding). Keying the allowlist on column one alone kept
	# "GiveFnptrsToDll" - a name nothing actually defines - while the
	# REAL defined symbol's bare form ("GiveFnptrsToDll@8", from
	# "_GiveFnptrsToDll@8" with only the leading underscore stripped)
	# matched nothing, so it silently got renamed to
	# "sv__GiveFnptrsToDll@8", breaking the one export
	# engine/server/sv_game.c hard-requires to load the module at all -
	# confirmed by direct inspection of the (broken) prior run's output
	# before this fix. Fixed: a row with a second column allowlists that
	# column's bare form (what the object really defines); a row without
	# one allowlists column one directly (already bare, no leading
	# underscore, per tools/xbox-gen-exports.sh's own convention).
	awk '{
		if (NF >= 2) {
			name = $2
			sub(/^_/, "", name)
			print name
		} else {
			print $1
		}
	}' "$EXPORTS_OUT" > "$WORK/server_allow_names.txt"
	ALLOW_FILE="$WORK/server_allow_names.txt"
fi

# Step 3: strip .drectve from every object, before any rename - stale
# /EXPORT: directives on a renamed-away symbol corrupt the final image's
# export table instead of erroring (X7.1b's measured Gap 2). Objects with
# no .drectve section at all (a TU that exports nothing) are expected and
# not an error - checked via the tool's own exit status, not assumed.
for o in "${OBJS[@]}"; do
	base="$(basename "$o")"
	cp "$o" "$ISO_DIR/$base"
done
STRIP_ERR="$WORK/strip_err.txt"
for o in "$ISO_DIR"/*.obj; do
	if ! "$OBJCOPY" --remove-section=.drectve "$o" 2> "$STRIP_ERR"; then
		# llvm-objcopy exits nonzero on a genuinely malformed object, but
		# "section .drectve not found" on an object with none is exactly
		# the expected, harmless case for a TU with no /EXPORT: at all -
		# distinguished by message, not swallowed unconditionally.
		if ! grep -qi "does not have section\|no such section\|not found" "$STRIP_ERR"; then
			echo "xbox-game-isolate: llvm-objcopy --remove-section failed on $o:" >&2
			cat "$STRIP_ERR" >&2
			exit 1
		fi
	fi
done

# Step 4: build the rename map. Every name the module DEFINES, across all
# its objects, NOT on the allowlist above, gets ${PREFIX}_<raw-nm-name> -
# see the header comment on the underscore convention. Built with awk set
# operations, not a grep-per-symbol loop - real defined-symbol counts
# here run into the thousands across 100+ objects.
#
# Found running this for real, the same class of bug already found and
# fixed once this session in tools/xbox-game-sweep.sh's own log: `>>` to
# accumulate 100+ objects' symbol lists into one file, reopened once per
# object, silently lost entries under this Windows/MSYS setup (measured:
# a real symbol - CFlockingFlyer::Save - present in the source object but
# absent from the accumulated file, so it was never even considered for
# renaming and leaked through unrenamed). Fixed the same way: one
# persistent file descriptor for the whole loop, opened once.
: > "$WORK/all_defined.txt"
exec 4>> "$WORK/all_defined.txt"
for o in "$ISO_DIR"/*.obj; do
	if ! "$NM" --defined-only --extern-only "$o" > "$WORK/nm_out.txt" 2> "$WORK/nm_err.txt"; then
		echo "xbox-game-isolate: llvm-nm failed on $o:" >&2
		cat "$WORK/nm_err.txt" >&2
		exit 1
	fi
	awk '{print $NF}' "$WORK/nm_out.txt" >&4
done
exec 4>&-
sort -u "$WORK/all_defined.txt" > "$WORK/all_defined_sorted.txt"

if [ ! -s "$WORK/all_defined_sorted.txt" ]; then
	echo "xbox-game-isolate: zero defined symbols found across ${#OBJS[@]} object(s) -" >&2
	echo "  refusing to build an empty rename map from a module that apparently" >&2
	echo "  defines nothing." >&2
	exit 1
fi

awk '{ raw = $0; bare = raw; sub(/^_/, "", bare); print bare "\t" raw }' \
	"$WORK/all_defined_sorted.txt" > "$WORK/bare_raw.tsv"

RENAME_MAP="$ISO_DIR/rename-map.txt"
awk -F'\t' -v prefix="$PREFIX" '
	NR == FNR { allow[$0] = 1; next }
	!($1 in allow) { print $2 " " prefix "_" $2 }
' "$ALLOW_FILE" "$WORK/bare_raw.tsv" > "$RENAME_MAP"

RENAME_COUNT=$(wc -l < "$RENAME_MAP" | tr -d ' ')
TOTAL_DEFINED=$(wc -l < "$WORK/all_defined_sorted.txt" | tr -d ' ')
KEPT_COUNT=$((TOTAL_DEFINED - RENAME_COUNT))

if [ "$RENAME_COUNT" -eq 0 ]; then
	echo "xbox-game-isolate: every one of $TOTAL_DEFINED defined symbol(s) is on" >&2
	echo "  the allowlist - nothing to rename. That is only plausible for a" >&2
	echo "  trivially small module, and never for a real hl $TARGET build;" >&2
	echo "  refusing to proceed as if this were expected." >&2
	exit 1
fi

echo "xbox-game-isolate: $TOTAL_DEFINED defined symbol(s), $KEPT_COUNT kept" >&2
echo "  (allowlisted), $RENAME_COUNT renamed with the ${PREFIX}_ prefix" >&2

for o in "$ISO_DIR"/*.obj; do
	if ! "$OBJCOPY" --redefine-syms="$RENAME_MAP" "$o" 2> "$WORK/redefine_err.txt"; then
		echo "xbox-game-isolate: llvm-objcopy --redefine-syms failed on $o:" >&2
		cat "$WORK/redefine_err.txt" >&2
		exit 1
	fi
done

# Self-check: every symbol the ISOLATED objects now define must be either
# on the allowlist (bare form) or start with the expected prefix - a
# leaked, unrenamed non-allowlisted symbol is exactly the collision this
# whole mechanism exists to prevent, and at 100+-object scale nothing
# short of re-scanning every object catches that the way the 2-object
# X7.1b prototype could by eye.
: > "$WORK/post_defined.txt"
exec 4>> "$WORK/post_defined.txt"
for o in "$ISO_DIR"/*.obj; do
	if ! "$NM" --defined-only --extern-only "$o" > "$WORK/nm_out2.txt" 2> "$WORK/nm_err2.txt"; then
		echo "xbox-game-isolate: llvm-nm failed re-scanning $o after rename:" >&2
		cat "$WORK/nm_err2.txt" >&2
		exit 1
	fi
	awk '{print $NF}' "$WORK/nm_out2.txt" >&4
done
exec 4>&-
sort -u "$WORK/post_defined.txt" > "$WORK/post_defined_sorted.txt"

LEAKED=$(awk -v prefix="$PREFIX" '
	NR == FNR { allow[$0] = 1; next }
	{
		bare = $0
		sub(/^_/, "", bare)
		if (bare in allow) next
		if (index($0, prefix "_") == 1) next
		print
	}
' "$ALLOW_FILE" "$WORK/post_defined_sorted.txt")

if [ -n "$LEAKED" ]; then
	echo "xbox-game-isolate: REGRESSION - post-rename symbol(s) neither" >&2
	echo "  allowlisted nor ${PREFIX}_-prefixed:" >&2
	printf '%s\n' "$LEAKED" | sed 's/^/  /' >&2
	exit 1
fi

# Independent check, added in Codex review: the leak-check above and the
# rename decision itself both derive from the SAME $ALLOW_FILE, so a bug
# in how that file is built (exactly what round 1 found: GiveFnptrsToDll's
# real symbol, _GiveFnptrsToDll@8, was silently absent from it, so the
# leak-check couldn't have caught its own allowlist being wrong) passes
# both checks self-consistently. This one is independent: it re-reads
# tools/xbox-{server,client}-required-exports.txt directly - the source
# of truth for what MUST survive - and searches the real post-rename
# symbol table for either form xbox-gen-exports.sh itself would accept
# (plain cdecl `_name`, or a stdcall-decorated `_name@<n>`), never
# touching $ALLOW_FILE at all.
MISSING_REQUIRED=()
while IFS= read -r group; do
	[ -z "$group" ] && continue
	found=0
	IFS='|' read -ra ALTS <<< "$group"
	for alt in "${ALTS[@]}"; do
		if grep -qxF "_${alt}" "$WORK/post_defined_sorted.txt" 2>/dev/null \
			|| grep -qE "^_${alt}@[0-9]+\$" "$WORK/post_defined_sorted.txt" 2>/dev/null; then
			found=1
			break
		fi
	done
	[ "$found" -eq 0 ] && MISSING_REQUIRED+=("$group")
done < "$REQUIRE_FILE"

if [ "${#MISSING_REQUIRED[@]}" -gt 0 ]; then
	echo "xbox-game-isolate: REGRESSION - required export(s) from $REQUIRE_FILE" >&2
	echo "  are no longer defined (plain or stdcall-decorated) after rename -" >&2
	echo "  renamed away, or never really defined:" >&2
	for m in "${MISSING_REQUIRED[@]}"; do
		echo "  $m" >&2
	done
	exit 1
fi

if [ "$SWEEP_FAIL" -eq 0 ]; then
	COMPLETE="yes"
else
	COMPLETE="no"
fi

echo "TITLE $TITLE" > "$ISO_DIR/manifest.txt"
echo "TARGET $TARGET" >> "$ISO_DIR/manifest.txt"
echo "OBJECTS ${#OBJS[@]}" >> "$ISO_DIR/manifest.txt"
echo "DEFINED $TOTAL_DEFINED" >> "$ISO_DIR/manifest.txt"
echo "KEPT $KEPT_COUNT" >> "$ISO_DIR/manifest.txt"
echo "RENAMED $RENAME_COUNT" >> "$ISO_DIR/manifest.txt"
echo "SWEEP_OK $SWEEP_OK" >> "$ISO_DIR/manifest.txt"
echo "SWEEP_FAIL $SWEEP_FAIL" >> "$ISO_DIR/manifest.txt"
echo "SWEEP_TOTAL $SWEEP_TOTAL" >> "$ISO_DIR/manifest.txt"
echo "COMPLETE $COMPLETE" >> "$ISO_DIR/manifest.txt"

if [ "$COMPLETE" = "yes" ]; then
	echo "xbox-game-isolate: $TITLE/$TARGET isolated cleanly - ${#OBJS[@]} object(s)," >&2
	echo "  $RENAME_COUNT symbol(s) renamed, 0 leaked. Output: $ISO_DIR" >&2
else
	echo "xbox-game-isolate: $TITLE/$TARGET isolated a PARTIAL object set -" >&2
	echo "  ${#OBJS[@]}/$SWEEP_TOTAL object(s) ($SWEEP_FAIL known compile" >&2
	echo "  failure(s) not present at all), $RENAME_COUNT symbol(s) renamed, 0" >&2
	echo "  leaked among what IS here. NOT link-ready - manifest.txt records" >&2
	echo "  COMPLETE no and the real SWEEP_FAIL/SWEEP_TOTAL so nothing mistakes" >&2
	echo "  this for a complete module. Output: $ISO_DIR" >&2
fi
echo "$ISO_DIR"
