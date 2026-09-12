#!/usr/bin/env bash
# Attempt a real Xbox link of a title's isolated server module against the
# real engine + third-party + menu object set tools/xbox-link-check.sh
# already proves clean - X7.4 step 1's real exit gate ("0 duplicate, 0
# undefined, a real XBE"), the first time this project has actually
# attempted it against the real hl SDK rather than a stub or a 2-object
# prototype.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-game-link-check.sh <hl> [--expect-undefined N]
#
# Reuses, rather than re-derives, three already-built, already-reviewed
# pieces: tools/xbox-link-check.sh's own response file (the real,
# currently-clean engine+third-party+menu+filesystem_stdio+ref_null set,
# plus the stub game module this script replaces the server half of),
# tools/xbox-game-isolate.sh (X7.1b - the renamed, strip-and-rename-ready
# server object set), and tools/gen-library-tables.sh (X7.2/upstream's own
# xshlib.py mechanism - the generated_lib_server_exports.c this script
# builds fresh from the isolated exports.txt).
#
# Only `hl` is meaningful here (X7.1's own manifest tool only supports it
# yet), and only the SERVER half - the client isolation currently refuses
# outright (X7.4's own tracked, pre-existing SDL2-header/clang blocker),
# so the stub client stays in this link exactly as tools/xbox-link-check.sh
# already has it.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-game-link-check: $MSYS2_BIN not found." >&2
	echo "  This project requires MSYS2 installed at C:\\msys64 - see tools/xbox-env.sh." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

EXPECT_UNDEFINED=""
POSITIONAL=()
while [ $# -gt 0 ]; do
	case "$1" in
	--expect-undefined)
		EXPECT_UNDEFINED="${2:?--expect-undefined needs a number}"
		shift 2
		;;
	-*)
		echo "xbox-game-link-check: unrecognised argument: $1" >&2
		exit 1
		;;
	*)
		POSITIONAL+=("$1")
		shift
		;;
	esac
done

if [ "${#POSITIONAL[@]}" -ne 1 ]; then
	echo "usage: xbox-game-link-check.sh <hl> [--expect-undefined N]" >&2
	exit 1
fi
TITLE="${POSITIONAL[0]}"
if [ "$TITLE" != "hl" ]; then
	echo "xbox-game-link-check: only 'hl' is supported (X7.1's manifest tool" >&2
	echo "  does not derive op4/bshift yet - see docs/r3d/fork-plan.md's X7.6)." >&2
	exit 1
fi

SCRATCH="${SCRATCH:-$REPO/.sweep}"

# Step 1: a fresh, verified-clean baseline link (engine + third-party +
# menu + filesystem_stdio + ref_null + the STILL-stubbed client and
# server) - never trust a stale response file. --expect-undefined 0: the
# real, current, measured baseline (tools/xbox-link-check.sh's own
# parked-undefined-symbols.txt tracking is updated in the same change
# that adds this script - see its own commit for why the old "32" was
# stale, not a regression). If this doesn't pass, building the real
# server module on top of it would tell us nothing about OUR change.
echo "xbox-game-link-check: verifying the baseline link is clean first..." >&2
if ! bash tools/xbox-link-check.sh --client-main --xbe "$SCRATCH/game-$TITLE-baseline.xbe" --expect-undefined 0; then
	echo "xbox-game-link-check: the baseline (stub game module) link is not" >&2
	echo "  clean - refusing to build the real server module on top of a" >&2
	echo "  foundation that isn't itself known-good." >&2
	exit 1
fi

RESPONSE="$SCRATCH/link-check-response.txt"
if [ ! -f "$RESPONSE" ]; then
	echo "xbox-game-link-check: $RESPONSE does not exist after a clean baseline" >&2
	echo "  link - this should be impossible." >&2
	exit 1
fi

# Step 2: a fresh, COMPLETE server isolation. --expect-fail 0 --expect-total
# 107: hl's own real, current, tracked baseline (X7.4 - see
# docs/r3d/fork-plan.md). A partial isolation (COMPLETE no) is refused
# below regardless, but requiring a clean sweep here too means a real
# compile regression is caught at THIS step, with a clear cause, not
# discovered later as a confusing pile of undefined symbols.
echo "xbox-game-link-check: isolating the $TITLE server module..." >&2
if ! bash tools/xbox-game-isolate.sh "$TITLE" server --expect-fail 0 --expect-total 107; then
	echo "xbox-game-link-check: $TITLE server isolation did not certify a" >&2
	echo "  complete object set - refusing to attempt a real link against a" >&2
	echo "  set already known to be partial or regressed." >&2
	exit 1
fi

ISO_DIR="$SCRATCH/game-$TITLE/server-isolated"
ISO_MANIFEST="$ISO_DIR/manifest.txt"
if [ ! -f "$ISO_MANIFEST" ]; then
	echo "xbox-game-link-check: $ISO_MANIFEST missing after a certified isolation -" >&2
	echo "  impossible." >&2
	exit 1
fi
ISO_COMPLETE=""
ISO_CONTENT="$(cat "$ISO_MANIFEST" 2>/dev/null)"
if [[ "$ISO_CONTENT" =~ COMPLETE\ ([a-z]+) ]]; then
	ISO_COMPLETE="${BASH_REMATCH[1]}"
fi
if [ "$ISO_COMPLETE" != "yes" ]; then
	echo "xbox-game-link-check: $ISO_MANIFEST reports COMPLETE $ISO_COMPLETE, not" >&2
	echo "  yes - a partial server object set is not link-ready (see" >&2
	echo "  tools/xbox-game-isolate.sh's own manifest.txt fields). Refusing." >&2
	exit 1
fi

ISO_OBJS=()
for o in "$ISO_DIR"/*.obj; do
	[ -f "$o" ] && ISO_OBJS+=("$o")
done
if [ "${#ISO_OBJS[@]}" -eq 0 ]; then
	echo "xbox-game-link-check: zero .obj files in $ISO_DIR despite a" >&2
	echo "  COMPLETE-yes manifest - impossible." >&2
	exit 1
fi
echo "xbox-game-link-check: ${#ISO_OBJS[@]} isolated server object(s)." >&2

EXPORTS_TXT="$ISO_DIR/exports.txt"
[ -f "$EXPORTS_TXT" ] || { echo "xbox-game-link-check: $EXPORTS_TXT missing." >&2; exit 1; }

# Step 3: a fresh generated_lib_server_exports.c from the ISOLATED
# exports.txt (not stub_game/server/exports.txt) - the real
# GetEntityAPI/GetEntityAPI2/GiveFnptrsToDll (asm-overridden, X7.2) this
# module actually defines, in its own scratch dir so tools/xbox-cc.sh's
# own shared $SCRATCH/gen staleness cache (which every other sweep/
# link-check invocation relies on) is never touched by this script.
GAME_GEN_DIR="$SCRATCH/game-$TITLE/gen"
mkdir -p "$GAME_GEN_DIR" || { echo "xbox-game-link-check: cannot create $GAME_GEN_DIR" >&2; exit 1; }
if ! bash tools/gen-library-tables.sh "$GAME_GEN_DIR" "server:$EXPORTS_TXT" > /dev/null; then
	echo "xbox-game-link-check: tools/gen-library-tables.sh failed generating" >&2
	echo "  the real server export table." >&2
	exit 1
fi
SERVER_EXPORTS_C="$GAME_GEN_DIR/generated_lib_server_exports.c"
[ -f "$SERVER_EXPORTS_C" ] || { echo "xbox-game-link-check: $SERVER_EXPORTS_C was not generated." >&2; exit 1; }
SERVER_EXPORTS_OBJ="$GAME_GEN_DIR/generated_lib_server_exports.obj"
if ! tools/xbox-cc.sh "$SERVER_EXPORTS_C" -o "$SERVER_EXPORTS_OBJ" 2>"$GAME_GEN_DIR/lasterr.txt"; then
	echo "xbox-game-link-check: $SERVER_EXPORTS_C failed to compile:" >&2
	cat "$GAME_GEN_DIR/lasterr.txt" >&2
	exit 1
fi
echo "xbox-game-link-check: real server export table generated and compiled" >&2
echo "  (${EXPORTS_TXT#"$REPO/"})." >&2

# Step 4: build this attempt's own response file - the real, just-verified
# baseline response file, with the stub server's two objects removed
# (matched by their distinctive, stable filename suffixes - the exact
# generated path prefix is long and implementation-detail-shaped, see
# tools/xbox-link-check.sh's own naming; only the suffix is stable to
# match against) and the isolated server objects + the freshly-generated
# export table object added. generated_lib_client_exports.obj is a
# DIFFERENT suffix and must survive this filter - the client stays
# stubbed.
NEW_RESPONSE="$SCRATCH/game-$TITLE/link-check-response.txt"
mkdir -p "$(dirname "$NEW_RESPONSE")"
grep -v -F 'server_stub.obj' "$RESPONSE" \
	| grep -v -F 'generated_lib_server_exports.obj' \
	> "$NEW_RESPONSE" || true
REMOVED=$(( $(wc -l < "$RESPONSE") - $(wc -l < "$NEW_RESPONSE") ))
if [ "$REMOVED" -ne 2 ]; then
	echo "xbox-game-link-check: expected to remove exactly 2 stub-server" >&2
	echo "  object(s) from $RESPONSE, actually removed $REMOVED - the stub" >&2
	echo "  game module's naming may have changed; fix this script's filter" >&2
	echo "  rather than trusting a mismatched count." >&2
	exit 1
fi
for o in "${ISO_OBJS[@]}" "$SERVER_EXPORTS_OBJ"; do
	echo "\"$(cygpath -m "$o")\"" >> "$NEW_RESPONSE"
done
echo "xbox-game-link-check: response file built - removed 2 stub-server" >&2
echo "  object(s), added ${#ISO_OBJS[@]} isolated + 1 generated export table" >&2
echo "  object." >&2

# Step 5: the real link. Deliberately NOT `eval "$(deps/nxdk/bin/activate
# -s)"` - measured directly that calling it here REPLACES PATH rather
# than extending it, with a bare `/mingw64/bin` (not `/c/msys64/mingw64/bin`)
# that does not resolve on this Git-Bash/MSYS2 setup: `lld` was found
# before that call and "command not found" immediately after it, in the
# exact same shell, nothing else changed. tools/xbox-link-check.sh's own
# nxdk-link calls already prove the plain MSYS2_BIN PATH export at the
# top of this script (mingw64/bin then usr/bin) is sufficient on its
# own - that script never calls activate either.
LOG="$SCRATCH/game-$TITLE/link-check.txt"
OUT_EXE="$SCRATCH/game-$TITLE/link-check.exe"
deps/nxdk/bin/nxdk-link -errorlimit:0 -include:_automount_d_drive \
	-out:"$OUT_EXE" "@$NEW_RESPONSE" > "$LOG" 2>&1
LINK_STATUS=$?

DUP=$(grep -c 'duplicate symbol' "$LOG")
UNDEF=$(grep -c 'undefined symbol' "$LOG")

echo
echo "xbox-game-link-check: $TITLE server - $DUP duplicate, $UNDEF undefined   (full log: $LOG)"
echo

# Same robustness as tools/xbox-link-check.sh's own gate - see that
# script's comment for the full reasoning (a fatal lld failure that is
# neither a duplicate nor an undefined symbol must never read as "0
# duplicate, 0 undefined").
TOTAL_ERR=$(grep -c '^lld: error:' "$LOG")
OTHER_ERR=$(( TOTAL_ERR - DUP - UNDEF ))
if [ "$OTHER_ERR" -gt 0 ] || { [ "$LINK_STATUS" -ne 0 ] && [ "$TOTAL_ERR" -eq 0 ]; }; then
	echo "xbox-game-link-check: REGRESSION - nxdk-link failed (exit $LINK_STATUS)" >&2
	echo "  for a reason other than duplicate/undefined symbols." >&2
	echo "  Full log: $LOG" >&2
	tail -n 20 "$LOG" >&2
	exit 1
fi

if [ "$DUP" -gt 0 ]; then
	echo "xbox-game-link-check: REGRESSION - $DUP duplicate symbol(s), expected 0." >&2
	echo "  This is exactly what X7.1b's symbol isolation exists to prevent -" >&2
	echo "  a leaked, unrenamed collision would be a real bug in that tool," >&2
	echo "  not an SDK problem." >&2
	grep 'duplicate symbol' "$LOG" | sed 's/.*duplicate symbol: //' | sort -u >&2
	exit 1
fi

if [ "$UNDEF" -gt 0 ] && [ -z "$EXPECT_UNDEFINED" ]; then
	echo "xbox-game-link-check: $UNDEF undefined symbol(s), no --expect-undefined" >&2
	echo "  given. This is likely the FIRST real measurement of what the hl" >&2
	echo "  server module actually needs from the engine beyond the stub -" >&2
	echo "  pass --expect-undefined N (see the log above) once the real" >&2
	echo "  count is known and reviewed, or fix them." >&2
	exit 1
fi

if [ -n "$EXPECT_UNDEFINED" ]; then
	if [ "$UNDEF" -eq "$EXPECT_UNDEFINED" ]; then
		echo "xbox-game-link-check: $UNDEF undefined matches --expect-undefined $EXPECT_UNDEFINED."
	elif [ "$UNDEF" -gt "$EXPECT_UNDEFINED" ]; then
		echo "xbox-game-link-check: REGRESSION - $UNDEF undefined, expected $EXPECT_UNDEFINED." >&2
		exit 1
	else
		echo "xbox-game-link-check: IMPROVEMENT - $UNDEF undefined, expected $EXPECT_UNDEFINED." >&2
		echo "  Update the tracked count and whichever docs cite it." >&2
		exit 1
	fi
fi

echo "xbox-game-link-check: $TITLE server module link-checked. Output: $OUT_EXE" >&2
echo "$OUT_EXE"
