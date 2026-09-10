#!/usr/bin/env bash
# X2 W5 - link the real Xbox DEDICATED-SERVER target (tools/xbox-dedicated-
# sweep.sh's object set) plus the stub game module (server side only - a
# dedicated build never loads the client/menu libraries, but
# platform/misc/lib_static.c's generated dispatch table always references
# all three by name, same as tools/xbox-link-check.sh's own mainui-optional
# case), and produce a real .xbe.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-dedicated-sweep.sh
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-dedicated-link-check.sh --xbe .sweep-dedicated/resonance3d-dedicated.xbe
#
# Deliberately separate from tools/xbox-link-check.sh rather than a shared
# flag: that script's whole object set (mainui, the broader engine sweep)
# answers "does the eventual full client+menu target link", a materially
# different question with its own tracked baseline (divergences #20-23).
# This one answers "does a real, booting, dedicated server link" - W5's
# actual exit criterion (docs/r3d/mpak-loading-design.md section 8: "same
# as W4, on hardware/xemu").
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-dedicated-link-check: $MSYS2_BIN not found - see tools/xbox-env.sh." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

XBE_OUT=""
while [ $# -gt 0 ]; do
	case "$1" in
		--xbe) XBE_OUT="$2"; shift 2 ;;
		*) echo "xbox-dedicated-link-check: unrecognised argument: $1" >&2; exit 1 ;;
	esac
done

SCRATCH="${SCRATCH:-$REPO/.sweep-dedicated}"

# Found in review (Codex, round 2): a plain `ls *.obj` trusts whatever is
# sitting in $SCRATCH, which tools/xbox-dedicated-sweep.sh only ever ADDS
# to per file compiled - a source later renamed, excluded, or deleted
# left its last-successful object behind, and this script would still
# find and link it, reporting a clean link over a source set that no
# longer matches the checkout. The sweep now clears $SCRATCH before every
# run and writes manifest.txt only after a clean, non-empty pass (same
# fix as tools/xbox-mainui-sweep.sh's own manifest); this script now
# requires that manifest, requires it clean, and cross-checks the object
# count on disk against it - not just the manifest's own say-so, the same
# double-check tools/xbox-link-check.sh already does for the mainui case.
MANIFEST="$SCRATCH/manifest.txt"
if [ ! -f "$MANIFEST" ]; then
	echo "xbox-dedicated-link-check: $MANIFEST does not exist." >&2
	echo "  Run tools/xbox-dedicated-sweep.sh first - it only writes this" >&2
	echo "  file after a clean, non-empty sweep." >&2
	exit 1
fi
MANIFEST_OK=$(sed -n 's/OK=\([0-9]*\).*/\1/p' "$MANIFEST")
MANIFEST_FAIL=$(sed -n 's/.*FAIL=\([0-9]*\).*/\1/p' "$MANIFEST")
MANIFEST_TOTAL=$(sed -n 's/.*TOTAL=\([0-9]*\).*/\1/p' "$MANIFEST")
if [ -z "$MANIFEST_OK" ] || [ -z "$MANIFEST_FAIL" ] || [ "$MANIFEST_FAIL" -gt 0 ] \
	|| [ -z "$MANIFEST_TOTAL" ] || [ "$MANIFEST_TOTAL" -eq 0 ]; then
	echo "xbox-dedicated-link-check: $MANIFEST is not clean - ($(cat "$MANIFEST" 2>/dev/null))." >&2
	echo "  Fix the failures and re-run tools/xbox-dedicated-sweep.sh." >&2
	exit 1
fi

ENGINE_OBJS=$(ls "$SCRATCH"/*.obj 2>/dev/null)
ENGINE_OBJ_COUNT=$(echo $ENGINE_OBJS | wc -w)
if [ -z "$ENGINE_OBJS" ] || [ "$ENGINE_OBJ_COUNT" -ne "$MANIFEST_OK" ]; then
	echo "xbox-dedicated-link-check: $ENGINE_OBJ_COUNT object(s) on disk in $SCRATCH" >&2
	echo "  do not match manifest.txt's OK=$MANIFEST_OK - re-run" >&2
	echo "  tools/xbox-dedicated-sweep.sh to refresh both together." >&2
	exit 1
fi
echo "xbox-dedicated-link-check: $ENGINE_OBJ_COUNT engine objects match manifest.txt (OK=$MANIFEST_OK, clean sweep)." >&2

# generated_library_tables.h + its three per-module exports.c files: a side
# effect of any tools/xbox-cc.sh compile (see that script's own GEN_DIR
# preflight). Reused, not regenerated - same reasoning as
# tools/xbox-link-check.sh's own stub_game section.
GEN_DIR="$REPO/.sweep/gen"
if [ ! -f "$GEN_DIR/generated_library_tables.h" ]; then
	echo "xbox-dedicated-link-check: $GEN_DIR/generated_library_tables.h missing." >&2
	echo "  Run any tools/xbox-cc.sh compile first (tools/xbox-sweep.sh, or" >&2
	echo "  tools/xbox-dedicated-sweep.sh itself already triggers it)." >&2
	exit 1
fi

# filesystem_stdio: found booting this exact target in xemu for the first
# time - "FS_LoadProgs: can't load filesystem library filesystem_stdio",
# looping the BIOS animation (Sys_Error -> host shutdown -> reboot -> same
# failure). Real, not a stub: filesystem/*.c's own GetFSAPI is already
# part of ENGINE_OBJS above (this target links engine and filesystem into
# one binary), so this table entry is the only missing piece - required,
# unlike menu below, since FS_Init runs unconditionally on every boot.
GEN_FS_C="$GEN_DIR/generated_lib_filesystem_stdio_exports.c"
if [ ! -f "$GEN_FS_C" ]; then
	echo "xbox-dedicated-link-check: $GEN_FS_C does not exist." >&2
	echo "  Run any tools/xbox-cc.sh compile first - it regenerates this" >&2
	echo "  alongside generated_library_tables.h now that filesystem_stdio" >&2
	echo "  is part of that table." >&2
	exit 1
fi
GEN_FS_OBJ="$SCRATCH/gen_$(echo "$GEN_FS_C" | tr '/' '_' | sed 's/\.c$/.obj/')"
if ! tools/xbox-cc.sh "$GEN_FS_C" -o "$GEN_FS_OBJ" 2>"$SCRATCH/stub-lasterr.txt"; then
	echo "xbox-dedicated-link-check: generated_lib_filesystem_stdio_exports.c failed to compile:" >&2
	cat "$SCRATCH/stub-lasterr.txt" >&2
	exit 2
fi
echo "xbox-dedicated-link-check: filesystem_stdio registered in the static library table (real GetFSAPI)." >&2

STUB_GAME_OBJS="$GEN_FS_OBJ"
for f in stub_game/server/server_stub.c "$GEN_DIR/generated_lib_server_exports.c" "$GEN_DIR/generated_lib_client_exports.c"; do
	if [ ! -f "$f" ]; then
		echo "xbox-dedicated-link-check: $f does not exist." >&2
		exit 1
	fi
	obj="$SCRATCH/gen_$(echo "$f" | tr '/' '_' | sed 's/\.c$/.obj/')"
	mkdir -p "$SCRATCH"
	if ! tools/xbox-cc.sh "$f" -o "$obj" 2>"$SCRATCH/stub-lasterr.txt"; then
		echo "xbox-dedicated-link-check: STUB GAME MODULE FILE THAT WAS SUPPOSED TO COMPILE DID NOT: $f" >&2
		cat "$SCRATCH/stub-lasterr.txt" >&2
		exit 2
	fi
	STUB_GAME_OBJS="$STUB_GAME_OBJS $obj"
done
echo "xbox-dedicated-link-check: stub game module included (server side; SV_LoadProgs's real, only caller here)." >&2

# Menu is never reached under XASH_DEDICATED (Host_IsDedicated() is the
# compile-time constant true, common.h:99), but lib_static.c's generated
# dispatch table declares lib_menu_exports[] unconditionally - an empty,
# real table, same fallback tools/xbox-link-check.sh uses when mainui was
# not swept.
MENU_STUB_C="$SCRATCH/gen_menu_exports_empty.c"
MENU_STUB_OBJ="$SCRATCH/gen_menu_exports_empty.obj"
{
	echo "typedef struct {const char *name;void *func;} table_t;"
	echo "table_t lib_menu_exports[] = { {0,0} };"
} > "$MENU_STUB_C"
if ! tools/xbox-cc.sh "$MENU_STUB_C" -o "$MENU_STUB_OBJ" 2>"$SCRATCH/stub-lasterr.txt"; then
	echo "xbox-dedicated-link-check: empty lib_menu_exports failed to compile:" >&2
	cat "$SCRATCH/stub-lasterr.txt" >&2
	exit 2
fi

NXDK_DIR="deps/nxdk"
LIBS=(
	"$NXDK_DIR/lib/xboxkrnl/libxboxkrnl.lib" "$NXDK_DIR/lib/libpdclib.lib"
	"$NXDK_DIR/lib/libwinapi.lib" "$NXDK_DIR/lib/libnxdk_hal.lib"
	"$NXDK_DIR/lib/libnxdk.lib" "$NXDK_DIR/lib/libnxdk_automount_d.lib"
	"$NXDK_DIR/lib/libpbkit.lib" "$NXDK_DIR/lib/nxdk_usb.lib"
	"$NXDK_DIR/lib/libxboxrt.lib" "$NXDK_DIR/lib/libzlib.lib"
	"$NXDK_DIR/lib/libpng.lib" "$NXDK_DIR/lib/libjpeg.lib"
	"$NXDK_DIR/lib/libnxdk_net.lib" "$NXDK_DIR/lib/libSDL2.lib"
	"$NXDK_DIR/lib/winmm.lib"
	# filesystem/VFileSystem009.cpp is real C++ (a COM-style interface
	# class, virtual dtors, one try/catch) even in the dedicated build -
	# not only the menu module, which is where tools/xbox-link-check.sh's
	# own copy of this comment first noted the need for it.
	"$NXDK_DIR/lib/libc++.lib"
)

RESPONSE="$SCRATCH/link-response.txt"
: > "$RESPONSE"
# shellcheck disable=SC2086
for f in $ENGINE_OBJS $STUB_GAME_OBJS "$MENU_STUB_OBJ" "${LIBS[@]}"; do
	echo "\"$(cygpath -m "$f")\"" >> "$RESPONSE"
done

LOG="$SCRATCH/link.txt"
LINK_OUT="${XBE_OUT:-$SCRATCH/resonance3d-dedicated.exe}"
deps/nxdk/bin/nxdk-link -errorlimit:0 -include:_automount_d_drive \
	-out:"$LINK_OUT" "@$RESPONSE" > "$LOG" 2>&1
LINK_STATUS=$?

DUP=$(grep -c 'duplicate symbol' "$LOG")
UNDEF=$(grep -c 'undefined symbol' "$LOG")

echo
echo "xbox-dedicated-link-check: $DUP duplicate, $UNDEF undefined   (full log: $LOG)"
if [ "$UNDEF" -gt 0 ]; then
	echo "xbox-dedicated-link-check: undefined symbols:" >&2
	grep 'undefined symbol' "$LOG" | sed -E 's/^>>> //' | sort -u >&2
fi
echo

if [ "$DUP" -gt 0 ] || [ "$UNDEF" -gt 0 ] || [ "$LINK_STATUS" -ne 0 ]; then
	echo "xbox-dedicated-link-check: link did not produce a clean, complete binary (exit $LINK_STATUS)." >&2
	exit 1
fi

echo "xbox-dedicated-link-check: clean link, 0 duplicate, 0 undefined."

# $LINK_OUT above is a raw PE .exe, not an XBE - nxdk-link's own output
# format. A real .xbe needs cxbe's separate conversion pass (same
# two-step tools/xbox-link-check.sh already uses); unlike that script this
# link is already fully resolved (0 undefined), so no -force:unresolved
# re-link is needed first - $LINK_OUT itself is what gets converted.
if [ -n "$XBE_OUT" ]; then
	CXBE="deps/nxdk/tools/cxbe/cxbe"
	if [ ! -x "$CXBE" ] && [ ! -f "${CXBE}.exe" ]; then
		echo "xbox-dedicated-link-check: $CXBE not found or not built." >&2
		echo "  Run: make -C deps/nxdk/tools/cxbe" >&2
		exit 1
	fi
	if ! "$CXBE" -OUT:"$XBE_OUT" -TITLE:"Resonance3D Dedicated" "$LINK_OUT" > "$SCRATCH/cxbe.txt" 2>&1; then
		echo "xbox-dedicated-link-check: cxbe conversion failed. See $SCRATCH/cxbe.txt" >&2
		tail -n 20 "$SCRATCH/cxbe.txt" >&2
		exit 1
	fi
	if [ ! -f "$XBE_OUT" ]; then
		echo "xbox-dedicated-link-check: cxbe reported success but $XBE_OUT does not exist." >&2
		exit 1
	fi
	# cmp against a literal 4-byte file, not a command-substitution capture
	# of the header text: this environment's `python3` can resolve to a
	# Windows Store install-stub on first invocation, which prints
	# "Downloading:/Extracting:" progress to stdout and corrupted an
	# earlier version of this check that captured `python3 -c 'print(...)'`
	# into a shell variable; `head -c4 | od` has the same
	# capture-into-a-variable problem one layer down (od's own multi-byte
	# escaping is not a stable string to compare against). cmp's exit code
	# is the only thing this needs.
	printf 'XBEH' > "$SCRATCH/xbeh-magic.bin"
	SIZE=$(wc -c < "$XBE_OUT")
	if cmp -s -n 4 "$SCRATCH/xbeh-magic.bin" "$XBE_OUT"; then
		MAGIC="XBEH"
	else
		MAGIC="(not XBEH)"
	fi
	if [ "$MAGIC" = "XBEH" ]; then
		echo "xbox-dedicated-link-check: $XBE_OUT is a real, valid XBE ($SIZE bytes)."
	else
		echo "xbox-dedicated-link-check: $XBE_OUT exists but does not start with XBEH magic (got: $MAGIC)." >&2
		exit 1
	fi
fi
exit 0
