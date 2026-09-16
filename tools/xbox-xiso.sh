#!/usr/bin/env bash
# X7.5 - assemble one title's real, bootable Xbox disc: the title's already-
# built default.xbe next to its retail game content, packed as an XISO.
#
# This is packaging, not compilation: the XBE must already exist (build it
# first with tools/xbox-game-link-check.sh --xbe <path>, the same tool and
# real client/server/freevgui link X7.4 verified). This script only copies
# and packs - and requires the sidecar that build wrote (<xbe>.identity:
# title=/modules=/sha256_xbe=) to actually say this XBE was built for the
# requested title with the real client linked in, not a stub. Found in
# Codex review, round 1: nothing about an XBE's own bytes says which
# title's game logic it links or whether the client is real or still
# stubbed - a stale build, a dedicated build, or another title's XBE
# handed to this script under the wrong name would look identical from
# the file alone and would package and "succeed" regardless. There is no
# warning-only fallback for a missing or mismatched sidecar, hl included.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-xiso.sh hl \
#       --steam "/d/SteamLibrary/steamapps/common/Half-Life" \
#       --xbe .sweep/game-hl/default.xbe \
#       --out .sweep-xiso/hl.iso
#
# Never copies retail content into the repo - none of it is ours to
# redistribute. Staging happens under .sweep-xiso/<title>/ (gitignored,
# same convention as every other .sweep* scratch directory), rebuilt clean
# on every run.
#
# gameinfo.txt/liblist.gam need NO generation step here, and this script
# does not attempt one: the real Steam install already ships both, written
# by the modern Xash3D FWGS build Valve now distributes Half-Life with
# (verified directly against a real local install, 2026-09-16 - every
# title's gamedir already carries a gameinfo.txt whose fields match what
# filesystem/gameinfo.c's FS_InitGameInfo/FS_ParseGenericGameInfo would
# derive from that same gamedir's liblist.gam, so "as upstream generates
# it" in this phase's plan means "ship the file upstream already
# generated", not "regenerate it here"). Likewise video.cfg: retail's own
# already sets `r_refdll ""`, which is what selects ref_soft (X5) - the
# numeric fields (resolution, refresh, fullscreen) are read but overridden
# by the Xbox video path's own hardcoded 640x480 (divergence #50), so
# retail's copy needs no Xbox-specific edit. If either fact ever stops
# holding for a title, this is the file to teach how to generate one
# instead of copying it - see filesystem/gameinfo.c for the real algorithm
# to port faithfully, not guess.
#
# Disc-size and load-time work (.mpak, X8) is deliberately NOT here, per
# the plan: a raw, uncompressed retail tree is fine for bring-up and this
# script does not try to be honest about anything it does not do.
set -u

# sha256_of PATH: prints a verified 64-hex-digit digest or fails (Codex
# review round 2: a bare `$(sha256sum ... | cut ...)` swallows both a
# missing/failing sha256sum AND a piped-away failure with `set -u` alone -
# no `pipefail`, no output-shape check - so a broken hasher on either side
# of the identity gate writes/reads an empty string, and empty compares
# equal to empty, silently defeating the whole gate this exists for).
# Every hash used below goes through this, never a raw command substitution.
sha256_of() {
	local h
	h="$(sha256sum "$1" 2>/dev/null | cut -d' ' -f1)" || return 1
	[[ "$h" =~ ^[0-9a-f]{64}$ ]] || return 1
	printf '%s\n' "$h"
}

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

EXTRACT_XISO="$REPO/deps/nxdk/tools/extract-xiso/build/extract-xiso.exe"
if [ ! -f "$EXTRACT_XISO" ]; then
	echo "xbox-xiso: $EXTRACT_XISO not found or not built." >&2
	echo "  Build nxdk's extract-xiso tool first." >&2
	exit 1
fi

# title -> "gamedir basedir startmap". gamedir is the retail folder this
# title's own content lives in (under the Steam install root); basedir is
# the fallback content every title falls back to for anything its own
# gamedir does not carry (retail's own convention, read from each
# gamedir's liblist.gam/gameinfo.txt - hl's basedir is itself). Only "hl"
# has a built XBE and a real boot to test against as this is written
# (X7.4); op4/bshift are X7.6, not started - accepted here so the mapping
# does not need rewriting when they land, but NOT claimed to work: this
# script has never packed or booted either.
title_row() {
	case "$1" in
		hl) echo "valve valve c0a0" ;;
		op4) echo "gearbox valve of0a0" ;;
		bshift) echo "bshift valve ba_tram1" ;;
		*) return 1 ;;
	esac
}

TITLE="${1:-}"
if [ -z "$TITLE" ]; then
	echo "usage: $0 <hl|op4|bshift> --steam <path> --xbe <path> [--out <path>]" >&2
	exit 1
fi
shift
ROW="$(title_row "$TITLE")" || { echo "xbox-xiso: unknown title '$TITLE' (known: hl, op4, bshift)." >&2; exit 1; }
read -r GAMEDIR BASEDIR STARTMAP <<<"$ROW"
if [ "$TITLE" != "hl" ]; then
	echo "xbox-xiso: WARNING - '$TITLE' is X7.6, not started; this script has" >&2
	echo "  never packed or booted it. Proceeding on your say-so." >&2
fi

STEAM=""
XBE=""
OUT=""
while [ $# -gt 0 ]; do
	case "$1" in
		--steam) STEAM="$2"; shift 2 ;;
		--xbe) XBE="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		*) echo "xbox-xiso: unrecognised argument: $1" >&2; exit 1 ;;
	esac
done
[ -n "$STEAM" ] || { echo "xbox-xiso: --steam <path to the Half-Life Steam install root> is required." >&2; exit 1; }
[ -n "$XBE" ] || { echo "xbox-xiso: --xbe <path to the built default.xbe> is required." >&2; exit 1; }
[ -d "$STEAM" ] || { echo "xbox-xiso: --steam path does not exist: $STEAM" >&2; exit 1; }
[ -f "$XBE" ] || { echo "xbox-xiso: --xbe path does not exist: $XBE" >&2; exit 1; }
: "${OUT:="$REPO/.sweep-xiso/$TITLE.iso"}"

# The XBEH magic check: same reasoning and same cmp-against-a-literal-file
# idiom as tools/xbox-dedicated-link-check.sh (a python3 first-invocation
# Windows Store install-stub can print progress text into a captured
# variable; cmp's exit code has no such problem).
XBEH_MAGIC="$REPO/.sweep-xiso/.xbeh-magic.bin"
mkdir -p "$(dirname "$XBEH_MAGIC")"
printf 'XBEH' > "$XBEH_MAGIC"
if ! cmp -s -n 4 "$XBEH_MAGIC" "$XBE"; then
	echo "xbox-xiso: $XBE does not start with XBEH magic - not a real XBE." >&2
	exit 1
fi

# The identity gate: required, not advisory. tools/xbox-game-link-check.sh
# writes <xbe>.identity next to every --xbe it produces (title=/modules=/
# sha256_xbe=); this script refuses to package anything that sidecar does
# not vouch for as this exact title's real, complete build.
IDENTITY="$XBE.identity"
if [ ! -f "$IDENTITY" ]; then
	echo "xbox-xiso: $IDENTITY not found." >&2
	echo "  --xbe must come from tools/xbox-game-link-check.sh <title> --xbe <path>" >&2
	echo "  (with --include-client --include-freevgui for a real bootable disc) -" >&2
	echo "  that tool writes this sidecar. A hand-placed XBE is not accepted." >&2
	exit 1
fi
XBE_TITLE=$(sed -n 's/^title=//p' "$IDENTITY")
XBE_MODULES=$(sed -n 's/^modules=//p' "$IDENTITY")
XBE_SHA=$(sed -n 's/^sha256_xbe=//p' "$IDENTITY")
if ! [[ "$XBE_SHA" =~ ^[0-9a-f]{64}$ ]]; then
	echo "xbox-xiso: $IDENTITY's sha256_xbe is not a 64-hex-digit digest ('$XBE_SHA') - refusing a sidecar this script cannot verify." >&2
	exit 1
fi
if [ "$XBE_TITLE" != "$TITLE" ]; then
	echo "xbox-xiso: $IDENTITY says title=$XBE_TITLE, but this run asked for '$TITLE'." >&2
	echo "  Refusing to package a mismatched title under the wrong name." >&2
	exit 1
fi
if [ "$XBE_MODULES" != "server+client+freevgui" ]; then
	echo "xbox-xiso: $IDENTITY says modules=$XBE_MODULES, not server+client+freevgui." >&2
	echo "  A real bootable disc needs the real client - rebuild with" >&2
	echo "  --include-client --include-freevgui." >&2
	exit 1
fi
ACTUAL_SHA=$(sha256_of "$XBE") || { echo "xbox-xiso: could not compute sha256 of $XBE - is sha256sum installed?" >&2; exit 1; }
if [ "$ACTUAL_SHA" != "$XBE_SHA" ]; then
	echo "xbox-xiso: $XBE has changed since $IDENTITY was written" >&2
	echo "  (sidecar sha256_xbe=$XBE_SHA, actual $ACTUAL_SHA) - stale sidecar" >&2
	echo "  or a different file at this path. Re-run the link-check build." >&2
	exit 1
fi
echo "xbox-xiso: identity confirmed - $XBE is $TITLE, $XBE_MODULES, matching $IDENTITY." >&2

GAMEDIR_SRC="$STEAM/$GAMEDIR"
[ -f "$GAMEDIR_SRC/liblist.gam" ] || { echo "xbox-xiso: $GAMEDIR_SRC/liblist.gam not found - is --steam the Half-Life install root?" >&2; exit 1; }
[ -f "$GAMEDIR_SRC/gameinfo.txt" ] || echo "xbox-xiso: WARNING - $GAMEDIR_SRC has no gameinfo.txt; relying on liblist.gam alone (untested path)." >&2

STAGE="$REPO/.sweep-xiso/$TITLE"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# Every staging mutation checked explicitly (Codex review round 1: `set -u`
# alone does not catch a failed `cp` - a disk-full, permission, or
# vanished-source failure would otherwise leave extract-xiso to silently
# pack whatever partial content survived, and this script would still
# report success).
echo "xbox-xiso: staging $GAMEDIR ($(du -sh "$GAMEDIR_SRC" 2>/dev/null | cut -f1))..." >&2
if ! cp -r "$GAMEDIR_SRC" "$STAGE/$GAMEDIR"; then
	echo "xbox-xiso: copying $GAMEDIR_SRC to the stage failed." >&2
	exit 1
fi

if [ "$BASEDIR" != "$GAMEDIR" ]; then
	BASEDIR_SRC="$STEAM/$BASEDIR"
	[ -d "$BASEDIR_SRC" ] || { echo "xbox-xiso: basedir $BASEDIR_SRC not found under --steam." >&2; exit 1; }
	echo "xbox-xiso: staging basedir $BASEDIR ($(du -sh "$BASEDIR_SRC" 2>/dev/null | cut -f1))..." >&2
	if ! cp -r "$BASEDIR_SRC" "$STAGE/$BASEDIR"; then
		echo "xbox-xiso: copying $BASEDIR_SRC to the stage failed." >&2
		exit 1
	fi
fi

if ! cp "$XBE" "$STAGE/default.xbe"; then
	echo "xbox-xiso: copying $XBE to $STAGE/default.xbe failed." >&2
	exit 1
fi

# Verify the stage is actually complete before handing it to extract-xiso -
# the same identity check, now against the staged copy, so a copy that
# silently truncated or a `default.xbe` this step itself never placed
# cannot slip through as a source glitch would with success reported.
STAGED_SHA=$(sha256_of "$STAGE/default.xbe") || { echo "xbox-xiso: could not compute sha256 of the staged default.xbe - copy was incomplete or sha256sum is unavailable." >&2; exit 1; }
if [ "$STAGED_SHA" != "$XBE_SHA" ]; then
	echo "xbox-xiso: staged default.xbe does not match the verified source XBE - copy was incomplete." >&2
	exit 1
fi
if [ ! -f "$STAGE/$GAMEDIR/liblist.gam" ]; then
	echo "xbox-xiso: staged $GAMEDIR is missing liblist.gam - copy was incomplete." >&2
	exit 1
fi
if [ "$BASEDIR" != "$GAMEDIR" ] && [ ! -f "$STAGE/$BASEDIR/liblist.gam" ]; then
	echo "xbox-xiso: staged $BASEDIR is missing liblist.gam - copy was incomplete." >&2
	exit 1
fi

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
echo "xbox-xiso: packing $OUT ..." >&2
if ! "$EXTRACT_XISO" -c "$STAGE" "$OUT" > "$REPO/.sweep-xiso/$TITLE-pack.txt" 2>&1; then
	echo "xbox-xiso: extract-xiso failed - see .sweep-xiso/$TITLE-pack.txt" >&2
	tail -n 20 "$REPO/.sweep-xiso/$TITLE-pack.txt" >&2
	exit 1
fi
if [ ! -f "$OUT" ]; then
	echo "xbox-xiso: extract-xiso reported success but $OUT does not exist." >&2
	exit 1
fi

SIZE=$(wc -c < "$OUT")
echo "xbox-xiso: $OUT is a real ISO ($SIZE bytes) - title=$TITLE gamedir=$GAMEDIR basedir=$BASEDIR startmap=$STARTMAP"
