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
# by the modern Xash3D FWGS build Valve now distributes Half-Life with.
# **Corrected 2026-09-16, XM item 0**: an earlier version of this comment
# claimed gameinfo.txt's fields always match what deriving from that same
# gamedir's liblist.gam would give - checked directly and wrong for two
# fields. filesystem/gameinfo.c's FS_ParseGameInfo prefers gameinfo.txt
# whenever it exists and is not older than liblist.gam - true on the real
# Steam install - and does not re-derive from liblist.gam in that case;
# the real gameinfo.txt has no animated_title/hd_background line at all,
# even though liblist.gam sets both to 1, so a disc packaged by copying
# both files verbatim never sets GFL_ANIMATED_TITLE/GFL_HD_BACKGROUND
# regardless of liblist.gam's own content - found live, chasing why the
# animated title never drew (`CMenuAnimatedBanner::TryLoad` never even
# gets called). This script now copies those two fields from liblist.gam
# into gameinfo.txt when the latter is missing them (see
# sync_gameinfo_hl25_flags below) - forwarding what the disc's own
# liblist.gam already says, not inventing a value. Likewise video.cfg:
# retail's own already sets `r_refdll ""`, which is what selects ref_soft
# (X5) - the numeric fields (resolution, refresh, fullscreen) are read
# but overridden by the Xbox video path's own hardcoded 640x480
# (divergence #50), so retail's copy needs no Xbox-specific edit. If
# either fact ever stops holding for a title, this is the file to teach
# how to generate one instead of copying it - see filesystem/gameinfo.c
# for the real algorithm to port faithfully, not guess.
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
	echo "usage: $0 <hl|op4|bshift> --steam <path> --xbe <path> [--out <path>] [--menu-bake <dir>]" >&2
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
MENU_BAKE=""
while [ $# -gt 0 ]; do
	case "$1" in
		--steam) STEAM="$2"; shift 2 ;;
		--xbe) XBE="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--menu-bake) MENU_BAKE="$2"; shift 2 ;;
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

# XM item 0 stage B (fork-plan.md): --menu-bake overlays a directory tree
# (tools/asset-studio --bake-menu-art's own output shape - e.g.
# resource/BackgroundLayout.txt + resource/background/*.tga) onto the
# staged GAMEDIR, AFTER the retail copy above, so a disc built without
# this flag is completely unaffected (the retail Steam install is never
# modified - the overlay only ever touches this run's own scratch stage).
# `cp -r <bakeDir>/. <stage>/` (the trailing `/.` copies CONTENTS into an
# existing directory rather than nesting bakeDir itself one level deep).
#
# validate_menu_bake checks a directory actually IS a complete bake, not
# just that it exists (Codex review round 1: an empty or unrelated
# --menu-bake directory would otherwise copy nothing relevant, print the
# same "overlaying" message, and still report a successful pack - a
# baked-disc claim with nothing baked, indistinguishable from a real one
# in a later memstats reading). Checked BEFORE the copy (catch a bad
# --menu-bake argument immediately) and AGAIN AFTER it against the staged
# copy (catch a copy that silently dropped or truncated files, the same
# "verify the postcondition, not the command's own exit code" rule the
# sha256 checks above already use).
#
# EVERY referenced tile, not just the first (Codex review round 2: mainui
# aborts loading the WHOLE background as soon as any one referenced tile
# is missing or unreadable, BackgroundBitmap.cpp:249-258 - a bake missing
# a later tile, or one truncated mid-copy, would otherwise pass this
# check and package a disc whose baked background never loads at all).
# tga_dims reads width/height straight out of the file's own 18-byte
# header (bytes 12-13, 14-15, little-endian - the exact shape
# tools/asset-studio's writeTga() always produces: uncompressed 32bpp,
# no ID field); comparing the file's actual byte count against
# 18 + w*h*4 catches a truncated/corrupt tile without needing a full TGA
# parser or a source-vs-staged hash compare - proportionate to what
# writeTga can actually get wrong (a short write), not to every way a
# TGA file in general could be malformed.
tga_dims() { # <path>: echoes "<w> <h>" or fails
	local w h
	w=$(od -An -tu2 -j12 -N2 "$1" 2>/dev/null | tr -d '[:space:]')
	h=$(od -An -tu2 -j14 -N2 "$1" 2>/dev/null | tr -d '[:space:]')
	case "$w$h" in *[!0-9]*|"") return 1 ;; esac
	printf '%s %s\n' "$w" "$h"
}
validate_menu_bake() {
	local dir="$1" layout="$1/resource/BackgroundLayout.txt" resLine tileCount=0
	local tilePath tileFile dims w h expectedSize actualSize
	if [ ! -f "$layout" ]; then
		echo "xbox-xiso: $dir has no resource/BackgroundLayout.txt - not a tools/asset-studio --bake-menu-art output." >&2
		return 1
	fi
	resLine=$(grep -m1 -E '^[[:space:]]*resolution[[:space:]]+[0-9]+[[:space:]]+[0-9]+' "$layout")
	if [ -z "$resLine" ]; then
		echo "xbox-xiso: $layout has no valid 'resolution <w> <h>' line." >&2
		return 1
	fi
	while IFS= read -r tilePath; do
		[ -n "$tilePath" ] || continue
		tileFile="$dir/$tilePath"
		if [ ! -f "$tileFile" ]; then
			echo "xbox-xiso: $layout references $tilePath, which does not exist under $dir." >&2
			return 1
		fi
		if ! dims=$(tga_dims "$tileFile"); then
			echo "xbox-xiso: $tileFile: could not read a TGA header (too short or unreadable)." >&2
			return 1
		fi
		read -r w h <<<"$dims"
		if [ "$w" -eq 0 ] || [ "$h" -eq 0 ]; then
			echo "xbox-xiso: $tileFile: TGA header declares 0x0 dimensions." >&2
			return 1
		fi
		expectedSize=$((18 + w * h * 4))
		actualSize=$(wc -c < "$tileFile")
		if [ "$actualSize" -ne "$expectedSize" ]; then
			echo "xbox-xiso: $tileFile: $actualSize bytes, expected $expectedSize for an uncompressed 32-bit ${w}x${h} TGA - truncated or corrupt." >&2
			return 1
		fi
		tileCount=$((tileCount + 1))
	done < <(awk '$1 != "resolution" && NF >= 4 { print $1 }' "$layout")
	if [ "$tileCount" -eq 0 ]; then
		echo "xbox-xiso: $layout declares no tile line - refusing an empty bake." >&2
		return 1
	fi
	return 0
}
if [ -n "$MENU_BAKE" ]; then
	[ -d "$MENU_BAKE" ] || { echo "xbox-xiso: --menu-bake path does not exist or is not a directory: $MENU_BAKE" >&2; exit 1; }
	if ! validate_menu_bake "$MENU_BAKE"; then
		echo "xbox-xiso: --menu-bake $MENU_BAKE failed validation - refusing to pack." >&2
		exit 1
	fi
	echo "xbox-xiso: overlaying menu bake $MENU_BAKE onto $GAMEDIR..." >&2
	if ! cp -r "$MENU_BAKE/." "$STAGE/$GAMEDIR/"; then
		echo "xbox-xiso: overlaying $MENU_BAKE onto $STAGE/$GAMEDIR failed." >&2
		exit 1
	fi
	if ! validate_menu_bake "$STAGE/$GAMEDIR"; then
		echo "xbox-xiso: staged $GAMEDIR failed the same bake validation after the overlay copy - copy was incomplete." >&2
		exit 1
	fi
	echo "xbox-xiso: menu bake overlay verified in the stage." >&2
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

# XM item 0 (fork-plan.md), found chasing why the animated title never
# drew on a real disc: this header's own "gameinfo.txt already carries
# everything liblist.gam does" claim is wrong for exactly two fields.
# filesystem/gameinfo.c's FS_ParseGameInfo prefers gameinfo.txt over
# liblist.gam whenever gameinfo.txt exists and is not older than
# liblist.gam - true on the real Steam install (gameinfo.txt's mtime is
# the later one) - and never falls back to re-deriving from liblist.gam
# in that case. The real gameinfo.txt (checked directly, not assumed)
# has no animated_title or hd_background line at all, even though
# liblist.gam sets both to 1 - so GFL_ANIMATED_TITLE/GFL_HD_BACKGROUND
# are never set from a disc packaged by copying both files verbatim, no
# matter what liblist.gam says. Confirmed live: a breakpoint on
# CMenuAnimatedBanner::TryLoad never fired, even from a guest reset, on
# a disc with all eleven logo*.tga files present and readable.
# Fix, not a guess: copy forward exactly what THIS disc's own liblist.gam
# already says, for these two fields only, only when gameinfo.txt is
# missing them - never invented, never applied to a title whose
# liblist.gam does not set the flag.
# Codex review round 1: the first version used grep's exit status as a
# plain boolean (found/not-found), which is wrong - grep exits 1 for "no
# match" but ALSO anything >1 for a real error (missing file mid-run,
# I/O failure, a broken regex), and `set -u` alone does not catch either
# the misread or an unchecked `printf ... >>` append failing on a
# read-only or full filesystem. Any of those would have left gameinfo.txt
# silently unchanged while this script still reported a successful pack -
# recreating the exact missing-title failure this fix exists to close,
# invisibly. grep_status EXPLICITLY distinguishes "no match" (1, expected,
# keep going) from anything else (a real error, fail loudly); the append
# is checked; and the whole function's exit status is checked by its
# caller, which now also re-reads the flag straight back out of the
# staged file afterward as the actual postcondition - not "the append
# command returned 0", but "the file now contains what was asked for".
grep_status() { # <pattern> <file>: echoes 0=match 1=no-match 2=error, distinct from $?
	local rc
	grep -qiE "$1" "$2"; rc=$?
	if [ "$rc" -gt 1 ]; then
		echo "xbox-xiso: grep failed reading $2 (exit $rc) - not treating that as 'no match'." >&2
		return 2
	fi
	return "$rc"
}
sync_gameinfo_hl25_flags() {
	local liblist="$1/liblist.gam" gameinfo="$1/gameinfo.txt"
	[ -f "$liblist" ] && [ -f "$gameinfo" ] || return 0
	local key already liblist_has
	for key in animated_title hd_background; do
		grep_status "^[[:space:]]*${key}[[:space:]]" "$gameinfo"; already=$?
		case "$already" in
			0) continue ;;
			2) return 1 ;;
		esac
		grep_status "^[[:space:]]*${key}[[:space:]]+\"?1\"?[[:space:]]*(//.*)?\$" "$liblist"; liblist_has=$?
		case "$liblist_has" in
			1) continue ;;
			2) return 1 ;;
		esac
		if ! printf '%s\t\t"1"\n' "$key" >> "$gameinfo"; then
			echo "xbox-xiso: could not append $key to $gameinfo (append failed)." >&2
			return 1
		fi
		if ! grep -qiE "^[[:space:]]*${key}[[:space:]]" "$gameinfo"; then
			echo "xbox-xiso: appended $key to $gameinfo but it still does not read back - refusing to pack a disc this fix silently failed on." >&2
			return 1
		fi
		echo "xbox-xiso: $gameinfo had no $key line but $liblist sets it - added $key \"1\"." >&2
	done
}
if ! sync_gameinfo_hl25_flags "$STAGE/$GAMEDIR"; then
	echo "xbox-xiso: syncing HL25 gameinfo flags for $GAMEDIR failed." >&2
	exit 1
fi
if [ "$BASEDIR" != "$GAMEDIR" ] && ! sync_gameinfo_hl25_flags "$STAGE/$BASEDIR"; then
	echo "xbox-xiso: syncing HL25 gameinfo flags for $BASEDIR failed." >&2
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
