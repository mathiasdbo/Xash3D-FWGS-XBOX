#!/usr/bin/env bash
# Attempt a real Xbox link of a title's isolated server module against the
# real engine + third-party + menu object set tools/xbox-link-check.sh
# already proves clean - X7.4 step 1's real exit gate ("0 duplicate
# symbols, and the undefined symbol set exactly matches the checked-in
# baseline"), the first time this project has actually attempted it
# against the real hl SDK rather than a stub or a 2-object prototype.
#
# This was a MEASUREMENT-only tool while the tracked server baseline
# (docs/r3d/parked-server-undefined-symbols.txt) was non-empty:
# `nxdk-link -errorlimit:0` reports every undefined symbol but produces
# no `.exe` at all when any remain (see docs/r3d/fork-plan.md, "A linker
# cannot emit a loadable binary with unresolved symbols") - the same
# real limitation tools/xbox-link-check.sh's own `--xbe` flag works
# around with a SEPARATE `-force:unresolved` link. As of divergence #37
# (the RTTI-free downcast fix), that baseline is empty and this script's
# own `nxdk-link` call now produces a real, valid $OUT_EXE every time -
# still true generically, though: if the tracked baseline ever goes
# non-empty again (a real regression, or a newly deferred gap), this
# reverts to measurement-only automatically, no script change needed.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-game-link-check.sh <hl> [--expect-undefined N] [--xbe <path>] [--include-client] [--include-freevgui]
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
# yet). Server-only by default, matching X7.4 step 1's own already-proven,
# already-boot-verified milestone exactly (the CLIENT_MAIN launcher, the
# real server module, still-stubbed client - see divergence #38's real
# breakpoint-verified boot). `--include-client`, added once the real
# client SDK itself reached 85/85 zero-failure and clean isolation
# (divergence #40), additionally swaps the still-empty stub client module
# (`client/exports.txt` is deliberately empty upstream - no
# `stub_game/client/*.c` file exists at all, so there is only ONE client
# object to remove, `generated_lib_client_exports.obj`, unlike the
# server's two) for the real, isolated `hl` client module - X7.4 step 2's
# own real exit gate.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

# A checkout may carry its nxdk location in .sweep/nxdk-dir (the agentic
# loop's setup stage writes it into each worktree). docs/r3d/lessons.md's
# entry on /c/msys64/usr/bin/bash shows an exported variable can be dropped
# at that shell boundary with no error, so the checkout itself answers
# when the environment does not. Unset and no file: the default below.
[ -n "${NXDK_DIR:-}" ] || NXDK_DIR="$(cat "$REPO/.sweep/nxdk-dir" 2>/dev/null || true)"

MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-game-link-check: $MSYS2_BIN not found." >&2
	echo "  This project requires MSYS2 installed at C:\\msys64 - see tools/xbox-env.sh." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

EXPECT_UNDEFINED=""
XBE_OUT=""
INCLUDE_CLIENT=""
INCLUDE_FREEVGUI=""
POSITIONAL=()
while [ $# -gt 0 ]; do
	case "$1" in
	--expect-undefined)
		EXPECT_UNDEFINED="${2:?--expect-undefined needs a number}"
		shift 2
		;;
	--xbe)
		XBE_OUT="${2:?--xbe needs a path}"
		shift 2
		;;
	--include-client)
		INCLUDE_CLIENT=1
		shift
		;;
	--include-freevgui)
		INCLUDE_FREEVGUI=1
		shift
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
	echo "usage: xbox-game-link-check.sh <hl> [--expect-undefined N] [--xbe <path>] [--include-client] [--include-freevgui]" >&2
	exit 1
fi
if [ -n "$INCLUDE_FREEVGUI" ] && [ -z "$INCLUDE_CLIENT" ]; then
	echo "xbox-game-link-check: --include-freevgui requires --include-client -" >&2
	echo "  freevgui is a client-side dependency only, meaningless without the" >&2
	echo "  real client module also being linked." >&2
	exit 1
fi
TITLE="${POSITIONAL[0]}"
if [ "$TITLE" != "hl" ]; then
	echo "xbox-game-link-check: only 'hl' is supported (X7.1's manifest tool" >&2
	echo "  does not derive op4/bshift yet - see docs/r3d/fork-plan.md's X7.6)." >&2
	exit 1
fi

SCRATCH="${SCRATCH:-$REPO/.sweep}"
if [ -n "$INCLUDE_CLIENT" ] && [ -n "$INCLUDE_FREEVGUI" ]; then
	MODULE_LABEL="server+client+freevgui"
elif [ -n "$INCLUDE_CLIENT" ]; then
	MODULE_LABEL="server+client"
else
	MODULE_LABEL="server"
fi

# Step 1: a fresh, verified-clean baseline link (engine + third-party +
# menu + filesystem_stdio + ref_null + the STILL-stubbed client and
# server) - never trust a stale response file. --expect-undefined 0: the
# real, current, measured baseline (tools/xbox-link-check.sh's own
# parked-undefined-symbols.txt tracking is updated in the same change
# that adds this script - see its own commit for why the old "32" was
# stale, not a regression). If this doesn't pass, building the real
# server module on top of it would tell us nothing about OUR change.
echo "xbox-game-link-check: verifying the baseline link is clean first..." >&2
if ! env SCRATCH="$SCRATCH" bash tools/xbox-link-check.sh --client-main --xbe "$SCRATCH/game-$TITLE-baseline.xbe" --expect-undefined 0; then
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
if ! env SCRATCH="$SCRATCH" bash tools/xbox-game-isolate.sh "$TITLE" server --expect-fail 0 --expect-total 107; then
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

# --include-client: the SAME isolation treatment as the server above, one
# module later - X7.4 step 2's own real exit gate. Kept as a clearly
# separate block (not folded into a loop over "server client") so the
# server-only default path above stays exactly what X7.4 step 1's own
# real, boot-verified milestone already ran - this block only ever adds
# to that, never changes it.
CLIENT_ISO_OBJS=()
CLIENT_EXPORTS_TXT=""
if [ -n "$INCLUDE_CLIENT" ]; then
	echo "xbox-game-link-check: isolating the $TITLE client module..." >&2
	if ! env SCRATCH="$SCRATCH" bash tools/xbox-game-isolate.sh "$TITLE" client --expect-fail 0 --expect-total 85; then
		echo "xbox-game-link-check: $TITLE client isolation did not certify a" >&2
		echo "  complete object set - refusing to attempt a real link against a" >&2
		echo "  set already known to be partial or regressed." >&2
		exit 1
	fi

	CLIENT_ISO_DIR="$SCRATCH/game-$TITLE/client-isolated"
	CLIENT_ISO_MANIFEST="$CLIENT_ISO_DIR/manifest.txt"
	if [ ! -f "$CLIENT_ISO_MANIFEST" ]; then
		echo "xbox-game-link-check: $CLIENT_ISO_MANIFEST missing after a certified" >&2
		echo "  isolation - impossible." >&2
		exit 1
	fi
	CLIENT_ISO_COMPLETE=""
	CLIENT_ISO_CONTENT="$(cat "$CLIENT_ISO_MANIFEST" 2>/dev/null)"
	if [[ "$CLIENT_ISO_CONTENT" =~ COMPLETE\ ([a-z]+) ]]; then
		CLIENT_ISO_COMPLETE="${BASH_REMATCH[1]}"
	fi
	if [ "$CLIENT_ISO_COMPLETE" != "yes" ]; then
		echo "xbox-game-link-check: $CLIENT_ISO_MANIFEST reports COMPLETE" >&2
		echo "  $CLIENT_ISO_COMPLETE, not yes - a partial client object set is not" >&2
		echo "  link-ready. Refusing." >&2
		exit 1
	fi

	for o in "$CLIENT_ISO_DIR"/*.obj; do
		[ -f "$o" ] && CLIENT_ISO_OBJS+=("$o")
	done
	if [ "${#CLIENT_ISO_OBJS[@]}" -eq 0 ]; then
		echo "xbox-game-link-check: zero .obj files in $CLIENT_ISO_DIR despite a" >&2
		echo "  COMPLETE-yes manifest - impossible." >&2
		exit 1
	fi
	echo "xbox-game-link-check: ${#CLIENT_ISO_OBJS[@]} isolated client object(s)." >&2

	CLIENT_EXPORTS_TXT="$CLIENT_ISO_DIR/exports.txt"
	[ -f "$CLIENT_EXPORTS_TXT" ] || { echo "xbox-game-link-check: $CLIENT_EXPORTS_TXT missing." >&2; exit 1; }
fi

# --include-freevgui: freevgui (X7.3, divergence-tracked) provides the
# real, buildable-from-source VGUI1 the isolated client module above
# needs and does not itself provide (deps/hlsdk/utils/vgui ships no
# source at all, headers plus one prebuilt win32 .lib). Its objects need
# NO isolation treatment, unlike the server/client modules above: they
# are not a second copy of anything already in this link (nothing else
# defines vgui::Panel/Scheme/etc.), so there is no same-name collision
# for X7.1b's isolation to guard against - they are simply added to the
# response file as-is, the real, public library the isolated client
# module's own (isolated, cl_-prefixed) code calls into by its real,
# unrenamed, Valve-mangled symbol names.
FREEVGUI_OBJS=()
if [ -n "$INCLUDE_FREEVGUI" ]; then
	echo "xbox-game-link-check: sweeping freevgui..." >&2
	if ! env SCRATCH="$SCRATCH" bash tools/xbox-freevgui-sweep.sh --expect-fail 0 --expect-total 39; then
		echo "xbox-game-link-check: freevgui did not sweep clean - refusing to" >&2
		echo "  link against a source set already known to be broken." >&2
		exit 1
	fi
	FREEVGUI_OUT_DIR="$SCRATCH/freevgui"
	for o in "$FREEVGUI_OUT_DIR"/*.obj; do
		[ -f "$o" ] && FREEVGUI_OBJS+=("$o")
	done
	if [ "${#FREEVGUI_OBJS[@]}" -eq 0 ]; then
		echo "xbox-game-link-check: zero .obj files in $FREEVGUI_OUT_DIR despite a" >&2
		echo "  clean sweep - impossible." >&2
		exit 1
	fi
	echo "xbox-game-link-check: ${#FREEVGUI_OBJS[@]} freevgui object(s)." >&2
fi

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

# --include-client: the SAME fresh generated_lib_client_exports.c
# treatment, one module later.
CLIENT_EXPORTS_OBJ=""
if [ -n "$INCLUDE_CLIENT" ]; then
	# --include-freevgui: freevgui's real InitVGUISupportAPI (compiled into
	# one of the freevgui objects added to the response file above, not
	# any of the isolated client's own 85) is NOT in the isolated client's
	# own exports.txt (X7.1b's isolation never sees freevgui at all - it
	# runs entirely on the client's OWN 85 objects). Found in Codex review:
	# without this, the symbol links (it is a real, defined function
	# somewhere in this response file) but is UNDISCOVERABLE at runtime -
	# on a static build, COM_GetProcAddress only ever searches the
	# GENERATED export table (engine/platform/misc/lib_static.c), and
	# engine/client/vgui/vgui_draw.c's own VGui_LoadProgs looks this name
	# up through exactly that mechanism, by name, same as every other
	# required export in this project. A clean link-only measurement
	# cannot see this gap at all - it is a real, defined, correctly-linked
	# symbol, just not reachable by the one lookup path that matters.
	# Fixed the same way this project registers every other statically-
	# linked module's entry point (ref_null's GetRefAPI, menu's
	# GetMenuAPI, filesystem_stdio's GetFSAPI): add the name to the
	# generated table's own input list. Which .obj actually DEFINES a
	# name is irrelevant to gen-library-tables.sh's own output (it emits
	# a plain `extern void NAME(void)` + a `{"NAME", (void*)NAME}` table
	# row, resolved at link time regardless of source object), so
	# appending to a COPY of the client's own real, isolated exports.txt
	# is correct and does not touch X7.1b's own isolation output.
	CLIENT_EXPORTS_LIST="$CLIENT_EXPORTS_TXT"
	if [ -n "$INCLUDE_FREEVGUI" ]; then
		CLIENT_EXPORTS_LIST="$GAME_GEN_DIR/client-exports-with-freevgui.txt"
		cp "$CLIENT_EXPORTS_TXT" "$CLIENT_EXPORTS_LIST"
		echo "InitVGUISupportAPI" >> "$CLIENT_EXPORTS_LIST"
	fi
	if ! bash tools/gen-library-tables.sh "$GAME_GEN_DIR" "client:$CLIENT_EXPORTS_LIST" > /dev/null; then
		echo "xbox-game-link-check: tools/gen-library-tables.sh failed generating" >&2
		echo "  the real client export table." >&2
		exit 1
	fi
	CLIENT_EXPORTS_C="$GAME_GEN_DIR/generated_lib_client_exports.c"
	[ -f "$CLIENT_EXPORTS_C" ] || { echo "xbox-game-link-check: $CLIENT_EXPORTS_C was not generated." >&2; exit 1; }
	CLIENT_EXPORTS_OBJ="$GAME_GEN_DIR/generated_lib_client_exports.obj"
	if ! tools/xbox-cc.sh "$CLIENT_EXPORTS_C" -o "$CLIENT_EXPORTS_OBJ" 2>"$GAME_GEN_DIR/lasterr-client.txt"; then
		echo "xbox-game-link-check: $CLIENT_EXPORTS_C failed to compile:" >&2
		cat "$GAME_GEN_DIR/lasterr-client.txt" >&2
		exit 1
	fi
	echo "xbox-game-link-check: real client export table generated and compiled" >&2
	echo "  (${CLIENT_EXPORTS_TXT#"$REPO/"})." >&2
fi

# Step 4: build this attempt's own response file - the real, just-verified
# baseline response file, with the stub server's two objects removed
# (matched by their distinctive, stable filename suffixes - the exact
# generated path prefix is long and implementation-detail-shaped, see
# tools/xbox-link-check.sh's own naming; only the suffix is stable to
# match against) and the isolated server objects + the freshly-generated
# export table object added. generated_lib_client_exports.obj is a
# DIFFERENT suffix and must survive this filter UNLESS --include-client is
# set, in which case it is removed too (the stub client's own only
# object - no stub_game/client/*.c source exists at all, so this is ONE
# object, not two) and replaced with the real isolated client module the
# same way.
NEW_RESPONSE="$SCRATCH/game-$TITLE/link-check-response.txt"
mkdir -p "$(dirname "$NEW_RESPONSE")"
if [ -n "$INCLUDE_CLIENT" ]; then
	grep -v -F 'server_stub.obj' "$RESPONSE" \
		| grep -v -F 'generated_lib_server_exports.obj' \
		| grep -v -F 'generated_lib_client_exports.obj' \
		> "$NEW_RESPONSE" || true
	EXPECTED_REMOVED=3
else
	grep -v -F 'server_stub.obj' "$RESPONSE" \
		| grep -v -F 'generated_lib_server_exports.obj' \
		> "$NEW_RESPONSE" || true
	EXPECTED_REMOVED=2
fi
REMOVED=$(( $(wc -l < "$RESPONSE") - $(wc -l < "$NEW_RESPONSE") ))
if [ "$REMOVED" -ne "$EXPECTED_REMOVED" ]; then
	echo "xbox-game-link-check: expected to remove exactly $EXPECTED_REMOVED" >&2
	echo "  stub-module object(s) from $RESPONSE, actually removed $REMOVED -" >&2
	echo "  the stub game module's naming may have changed; fix this script's" >&2
	echo "  filter rather than trusting a mismatched count." >&2
	exit 1
fi
for o in "${ISO_OBJS[@]}" "$SERVER_EXPORTS_OBJ"; do
	echo "\"$(cygpath -m "$o")\"" >> "$NEW_RESPONSE"
done
echo "xbox-game-link-check: response file built - removed 2 stub-server" >&2
echo "  object(s), added ${#ISO_OBJS[@]} isolated + 1 generated export table" >&2
echo "  object." >&2
if [ -n "$INCLUDE_CLIENT" ]; then
	for o in "${CLIENT_ISO_OBJS[@]}" "$CLIENT_EXPORTS_OBJ"; do
		echo "\"$(cygpath -m "$o")\"" >> "$NEW_RESPONSE"
	done
	echo "xbox-game-link-check: removed 1 stub-client object, added" >&2
	echo "  ${#CLIENT_ISO_OBJS[@]} isolated + 1 generated export table object." >&2
fi
if [ -n "$INCLUDE_FREEVGUI" ]; then
	for o in "${FREEVGUI_OBJS[@]}"; do
		echo "\"$(cygpath -m "$o")\"" >> "$NEW_RESPONSE"
	done
	echo "xbox-game-link-check: added ${#FREEVGUI_OBJS[@]} freevgui object(s)." >&2
fi

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
# Found in Codex review round 2, 2026-09-12: without this, a stale .exe
# from an earlier run (e.g. once the RTTI gap is fixed and this baseline
# goes to 0 undefined, producing a real .exe) would still be sitting at
# this same fixed path if a LATER run regressed back to an accepted
# nonzero undefined count - the existence check below would then report
# that stale file as this run's output. Remove it first and fail loudly
# if removal doesn't leave it actually gone, so "the file exists" only
# ever means "this run's own link produced it."
rm -f "$OUT_EXE"
if [ -e "$OUT_EXE" ]; then
	echo "xbox-game-link-check: could not remove stale $OUT_EXE before" >&2
	echo "  linking - refusing to risk reporting a stale artifact." >&2
	exit 1
fi
# Main-thread stack size, in bytes. nxdk-link's own default is -stack:65536
# (64 KiB), and that XBE header value is what every CreateThread(..., 0, ...)
# inherits - including the thread crt0's thrd_create() runs main() on, so
# the whole engine. Upstream assumes far more: Windows' 1 MiB default, and
# its own ports with small native defaults raise it explicitly (DOS:
# `stack=512k`, Emscripten: 16 MiB, scripts/waifulib/compiler_optimizations.py
# and engine/wscript). Found live, X7.4 step 2: the very first Host_Frame's
# NET_QueuePacket (engine/common/net_ws.c) puts byte buf[NET_MAX_FRAGMENT]
# (65535 bytes) on the stack - a 0x10100-byte frame that can never fit in
# 64 KiB at any depth - and nxdk's __chkstk -> _xlibc_check_stack raises
# EXCEPTION_STACK_OVERFLOW, NONCONTINUABLE, which the kernel turns into
# KeBugCheckEx(0x1E, 0xC00000FD). 1 MiB matches Windows, the platform
# XASH_XBOX is a sub-platform of (divergence #7), and also covers the
# larger frames already known further in (MAX_INIT_MSG = 196608-byte
# buffers in CL_ProcessFile/CL_BatchResourceRequest/CL_SendResourceList,
# Cmd_TokenizeString's 32 KiB). Cost: every thread created with size 0
# gets this much, so keep the thread count in mind at the memory-ceiling
# milestone. lld applies the last -stack: given, so this overrides
# nxdk-link's own default cleanly.
XBOX_STACK_SIZE=1048576

"${NXDK_DIR:-deps/nxdk}/bin/nxdk-link" -errorlimit:0 -include:_automount_d_drive \
	-stack:"$XBOX_STACK_SIZE" \
	-out:"$OUT_EXE" "@$NEW_RESPONSE" > "$LOG" 2>&1
LINK_STATUS=$?

DUP=$(grep -c 'duplicate symbol' "$LOG")
UNDEF=$(grep -c 'undefined symbol' "$LOG")

echo
echo "xbox-game-link-check: $TITLE $MODULE_LABEL - $DUP duplicate, $UNDEF undefined   (full log: $LOG)"
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

# The -stack: override is measured on the actual output, not trusted from
# the flag (found in review, divergence #47): lld takes the LAST -stack:
# given, and nothing else here would notice if that ordering, or the
# option itself, ever stopped overriding nxdk-link's own -stack:65536 -
# the binary would still link and still pass every other gate while
# restoring the first-frame stack overflow. nxdk-link emits no output at
# all when undefined symbols remain (see the --xbe path's -force:unresolved
# for why), so only an output that exists is checked here.
if [ -f "$OUT_EXE" ] && ! tools/xbox-check-stack.sh "$OUT_EXE" "$XBOX_STACK_SIZE"; then
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

# Found in Codex review round 1, 2026-09-12: a same-count substitution
# (___RTDynamicCast resolving while an unrelated symbol goes undefined
# for an unrelated reason) would still pass the count-only check above -
# exactly the false-clean measurement class tools/xbox-link-check.sh's
# own parked-undefined-symbols.txt identity check already exists to
# close, but that check was not carried into this tool. Mirrors it here
# with a module-specific baseline (the real undefined set for server-only
# vs. server+client vs. server+client+freevgui is not the same in any two
# of these, so each combination uses its own separately-tracked file
# rather than conflating them).
if [ "$UNDEF" -gt 0 ]; then
	if [ -n "$INCLUDE_CLIENT" ] && [ -n "$INCLUDE_FREEVGUI" ]; then
		PARKED_LIST="$REPO/docs/r3d/parked-game-freevgui-undefined-symbols.txt"
	elif [ -n "$INCLUDE_CLIENT" ]; then
		PARKED_LIST="$REPO/docs/r3d/parked-game-undefined-symbols.txt"
	else
		PARKED_LIST="$REPO/docs/r3d/parked-server-undefined-symbols.txt"
	fi
	if [ ! -f "$PARKED_LIST" ]; then
		echo "xbox-game-link-check: $PARKED_LIST does not exist - nothing to" >&2
		echo "  compare the real undefined symbol set against. Refusing to" >&2
		echo "  report a clean result." >&2
		exit 1
	fi
	ACTUAL_UNDEF_LIST="$SCRATCH/game-$TITLE/actual-undefined.txt"
	grep 'undefined symbol' "$LOG" | sed 's/.*undefined symbol: //' | sort -u > "$ACTUAL_UNDEF_LIST"
	EXPECTED_UNDEF_LIST="$SCRATCH/game-$TITLE/expected-undefined.txt"
	sort -u "$PARKED_LIST" > "$EXPECTED_UNDEF_LIST"
	if ! diff -u "$EXPECTED_UNDEF_LIST" "$ACTUAL_UNDEF_LIST" > "$SCRATCH/game-$TITLE/undefined-diff.txt"; then
		echo "xbox-game-link-check: REGRESSION - the actual undefined symbol set" >&2
		echo "  does not match ${PARKED_LIST#"$REPO/"}, even" >&2
		echo "  though the COUNT may have matched --expect-undefined. Refusing to" >&2
		echo "  report a clean result over an unexpected symbol set - see the diff:" >&2
		cat "$SCRATCH/game-$TITLE/undefined-diff.txt" >&2
		exit 1
	fi
	echo "xbox-game-link-check: actual undefined symbols match" >&2
	echo "  ${PARKED_LIST#"$REPO/"} exactly." >&2
fi

# --xbe: same mechanism as tools/xbox-link-check.sh's own --xbe block
# (see that script's comment for the full reasoning on why this is a
# genuine, deliberate use of -force:unresolved, not a way to hide a real
# problem) - a SEPARATE link against THIS script's own $NEW_RESPONSE
# (the real server module swapped in), only reached after a successful
# --expect-undefined match AND symbol-identity match above, so this
# never runs on an unexplained, regressed, or substituted link.
if [ -n "$XBE_OUT" ] && [ "$UNDEF" -gt 0 ]; then
	echo "xbox-game-link-check: refusing --xbe with $UNDEF undefined" >&2
	echo "  symbol(s) present, even though the count/identity match a" >&2
	echo "  tracked baseline. -force:unresolved (below) would still" >&2
	echo "  produce a file that passes the existence/magic checks this" >&2
	echo "  script's own --xbe path runs - but a real, callable branch" >&2
	echo "  through a genuinely undefined symbol (found in Codex review," >&2
	echo "  2026-09-13: real dynamic_cast<> call sites in 3rdparty/freevgui" >&2
	echo "  reach the parked ___RTDynamicCast gap) is not the same thing as" >&2
	echo "  a real, valid XBE. --xbe is for a clean (0 duplicate, 0" >&2
	echo "  undefined) link only; use this script without --xbe to measure" >&2
	echo "  a tracked, nonzero undefined baseline." >&2
	exit 1
fi
if [ -n "$XBE_OUT" ]; then
	XBE_EXE="$SCRATCH/game-$TITLE/xbe-build.exe"
	# Found in Codex review round 1, 2026-09-12: without this, a stale
	# xbe-build.exe or $XBE_OUT from an earlier run could survive a
	# nxdk-link/cxbe invocation that returns success without actually
	# writing fresh output, and the existence/magic checks below would
	# then certify that stale artifact as this run's - same class of bug
	# already fixed for the ordinary link's $OUT_EXE (Codex round 2,
	# see that fix's comment above). Remove both first and fail loudly
	# if removal doesn't leave them actually gone.
	rm -f "$XBE_EXE" "$XBE_OUT"
	if [ -e "$XBE_EXE" ] || [ -e "$XBE_OUT" ]; then
		echo "xbox-game-link-check: could not remove stale XBE build" >&2
		echo "  artifact(s) before building - refusing to risk reporting a" >&2
		echo "  stale artifact." >&2
		exit 1
	fi
	# shellcheck disable=SC2086
	# -map: from THIS link, not a second one: lld writes the same timestamp
	# into the map's header and the PE's COFF header, which is how
	# tools/xemu-debug.py binds a map to the executable a guest is running
	# (Codex review, X5: a map from a separate link of the same inputs has
	# identical addresses but no identity a tool can check; section
	# extents are not identity). The map lands next to the XBE below.
	XBE_MAP="$SCRATCH/game-$TITLE/xbe-build.map"
	"${NXDK_DIR:-deps/nxdk}/bin/nxdk-link" -errorlimit:0 -include:_automount_d_drive -force:unresolved \
		-stack:"$XBOX_STACK_SIZE" -map:"$XBE_MAP" \
		-out:"$XBE_EXE" "@$NEW_RESPONSE" > "$SCRATCH/game-$TITLE/xbe-link.txt" 2>&1
	XBE_LINK_STATUS=$?
	if [ "$XBE_LINK_STATUS" -ne 0 ] || [ ! -f "$XBE_EXE" ]; then
		echo "xbox-game-link-check: XBE build link failed (exit $XBE_LINK_STATUS) even" >&2
		echo "  with -force:unresolved - this is not the same tracked undefined" >&2
		echo "  symbol set; something else broke. See:" >&2
		echo "  $SCRATCH/game-$TITLE/xbe-link.txt" >&2
		tail -n 20 "$SCRATCH/game-$TITLE/xbe-link.txt" >&2
		exit 1
	fi

	# Same measured stack check as the ordinary link above, on the PE that
	# is about to become the XBE.
	if ! tools/xbox-check-stack.sh "$XBE_EXE" "$XBOX_STACK_SIZE"; then
		exit 1
	fi

	# cxbe is a built, untracked nxdk artifact: resolve it through a caller's
	# NXDK_DIR when set (a worktree pointed at the main checkout's built
	# nxdk - see tools/xbox-link-check.sh's own NXDK_DIR line), else here.
	CXBE="${NXDK_DIR:-deps/nxdk}/tools/cxbe/cxbe"
	if [ ! -x "$CXBE" ] && [ ! -f "${CXBE}.exe" ]; then
		echo "xbox-game-link-check: $CXBE not found or not built." >&2
		echo "  Point NXDK_DIR at a built nxdk (tools/xbox-env.sh), or build it there." >&2
		exit 1
	fi
	if ! "$CXBE" -OUT:"$XBE_OUT" -TITLE:"Resonance3D" "$XBE_EXE" > "$SCRATCH/game-$TITLE/xbe-cxbe.txt" 2>&1; then
		echo "xbox-game-link-check: cxbe conversion failed. See $SCRATCH/game-$TITLE/xbe-cxbe.txt" >&2
		tail -n 20 "$SCRATCH/game-$TITLE/xbe-cxbe.txt" >&2
		exit 1
	fi
	if [ ! -f "$XBE_OUT" ]; then
		echo "xbox-game-link-check: cxbe reported success but $XBE_OUT does not exist." >&2
		exit 1
	fi
	# XBEH, the real magic header every valid .xbe starts with - checked,
	# not assumed from cxbe's own exit code alone.
	# The XBE's own map, beside it, named for it: what tools/xemu-debug.py's
	# --map/--exe pair binds against (xbe-build.exe stays the --exe).
	if [ ! -f "$XBE_MAP" ]; then
		echo "xbox-game-link-check: the XBE link produced no map at $XBE_MAP." >&2
		exit 1
	fi
	cp "$XBE_MAP" "${XBE_OUT%.xbe}.map" || exit 1
	# Identity sidecar for tools/xemu-debug.py's pools/fps (Codex review, X5
	# round 2: the link TIMESTAMP this used to bind on is wall-clock
	# SECONDS - two links finishing in the same second collide, and with
	# equal section extents too - an ordinary equal-size substitution - a
	# stale map would still have bound). SHA-256 of the exact bytes of
	# xbe-build.exe (the --exe those tools take) and of its own map,
	# written next to xbe-build.exe as <exe>.identity: content hashes
	# cannot collide the way a timestamp can, and they bind the map's
	# actual content, not just a header field.
	printf 'sha256_exe=%s\nsha256_map=%s\n' \
		"$(sha256sum "$XBE_EXE" | cut -d' ' -f1)" \
		"$(sha256sum "$XBE_MAP" | cut -d' ' -f1)" \
		> "$XBE_EXE.identity" || exit 1
	MAGIC=$(head -c 4 "$XBE_OUT")
	if [ "$MAGIC" != "XBEH" ]; then
		echo "xbox-game-link-check: $XBE_OUT does not start with the XBEH magic" >&2
		echo "  header - cxbe reported success but the output does not look like" >&2
		echo "  a real XBE." >&2
		exit 1
	fi
	# And on the XBE itself: cxbe's propagation of the PE field into the
	# XBE header's SizeOfStackCommit is the value the target actually runs
	# on (nxdk's CreateThread(..., 0, ...) reads it for every thread).
	if ! tools/xbox-check-stack.sh "$XBE_OUT" "$XBOX_STACK_SIZE"; then
		exit 1
	fi
	echo "xbox-game-link-check: $XBE_OUT is a real, valid XBE ($(wc -c < "$XBE_OUT") bytes)." >&2
	# Title/completeness sidecar for tools/xbox-xiso.sh (X7.5, Codex review
	# round 1): nothing about --xbe's own bytes says which title's game
	# logic it links, or whether it is the real client or the still-stubbed
	# one - a stale build, a dedicated build, or another title's XBE handed
	# to xiso packaging under the wrong name would look identical from the
	# file alone. $XBE_OUT.identity records what THIS script actually
	# built: the title argument, the module set actually linked
	# ($MODULE_LABEL - a caller wanting a real bootable disc needs
	# "server+client+freevgui", not a stub), and the exact bytes' hash so a
	# later, different link under the same path cannot be mistaken for this
	# one. Same sidecar shape and reasoning as $XBE_EXE.identity above, one
	# level up the pipeline (that one binds xbe-build.exe to its own map;
	# this one binds the packaged --xbe to what was actually linked into it).
	# Checked, not a bare command substitution inside the printf (Codex
	# review round 2 on the xiso side of this same sidecar): without
	# pipefail, a missing or failing sha256sum here would silently write
	# sha256_xbe= empty, and an equally-broken reader comparing against an
	# equally-empty value would treat that as a match - defeating the gate
	# this sidecar exists for. XBE_SHA_OUT is required to look like a real
	# digest before anything is written.
	XBE_SHA_OUT="$(sha256sum "$XBE_OUT" 2>/dev/null | cut -d' ' -f1)"
	if ! [[ "$XBE_SHA_OUT" =~ ^[0-9a-f]{64}$ ]]; then
		echo "xbox-game-link-check: could not compute a valid sha256 of $XBE_OUT - refusing to write a sidecar tools/xbox-xiso.sh would trust." >&2
		exit 1
	fi
	# mainui= (X9-X step 5 memory investigation): the menu module is linked
	# whenever $SCRATCH/mainui holds a clean tools/xbox-mainui-sweep.sh
	# output (tools/xbox-link-check.sh), which no command-line flag - and
	# so nothing in modules= - records. Two XBEs both saying
	# modules=server+client+freevgui, one with the menu and one without,
	# were compared as main vs a branch: the menu's own fonts and
	# background (4.6 MiB contiguous) and code read as a regression in the
	# branch. Read from this link's own map, not inferred from the
	# directory, and kept out of modules= so tools/xbox-xiso.sh's exact
	# match on that line is unchanged.
	if grep -q '3rdparty_mainui_' "$XBE_MAP"; then XBE_MAINUI=1; else XBE_MAINUI=0; fi
	printf 'title=%s\nmodules=%s\nmainui=%s\nsha256_xbe=%s\n' \
		"$TITLE" "$MODULE_LABEL" "$XBE_MAINUI" "$XBE_SHA_OUT" \
		> "$XBE_OUT.identity" || exit 1
	echo "xbox-game-link-check: menu module (mainui) linked: $XBE_MAINUI (recorded in $XBE_OUT.identity)" >&2
fi

# Found in Codex review round 1, 2026-09-12: this script was printing
# $OUT_EXE as its successful output unconditionally, but nxdk-link
# produces no .exe at all whenever any undefined symbol remains (see the
# header comment) - as long as the tracked baseline above is non-empty,
# that file genuinely does not exist, and reporting its path as "output"
# would be a fabricated result. Report a real path only when one was
# actually produced.
if [ -f "$OUT_EXE" ]; then
	echo "xbox-game-link-check: $TITLE $MODULE_LABEL module(s) link-checked. Output: $OUT_EXE" >&2
	echo "$OUT_EXE"
else
	echo "xbox-game-link-check: $TITLE $MODULE_LABEL module(s) link-checked - $UNDEF" >&2
	echo "  undefined symbol(s) match the tracked baseline, so no .exe was" >&2
	echo "  produced (nxdk-link never emits output with unresolved symbols" >&2
	echo "  present). This is a measurement pass, not an executable build." >&2
fi
