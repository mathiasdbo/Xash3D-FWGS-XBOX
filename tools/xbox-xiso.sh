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
# --overlay <dir>: copied over the finished stage, last, before packing -
# paths relative to the disc root (so <gamedir>/video.cfg, <gamedir>/
# userconfig.d/00-live.cfg, <gamedir>/<map>.mpak). This is how the
# agentic loop's live look gets a disc that boots ref_nv2a, loads a map
# and carries its baked archive: the three things run 2 (PR #149) placed
# on the disc by hand before its screenshots. Every file is cmp-verified
# after the copy; an empty overlay is an error, not a no-op.
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
# delta.lst is a hard requirement of the engine, not of any one content
# vintage: SV_Init -> Delta_Init -> Delta_InitFields does
# `FS_LoadFile( "delta.lst" )` and calls Sys_Error when it is missing
# (engine/common/net_encode.c:853-854). On Xbox that error fires BEFORE
# CL_Init/VID_Init/R_Init, so there is no renderer to draw it, and the
# host then restarts and hits it again about every 3 s - the screen keeps
# the nxdk boot splash forever and the CPU sits in the kernel's idle
# wait, which is indistinguishable at a glance from a hung syscall
# (docs/r3d/lessons.md). Found 2026-09-16 packaging a real box-retail
# Half-Life 1.0.0.9 install, which ships no delta.lst at all - not loose,
# not in pak0.pak (3453 entries, scanned) - because the file only exists
# from Half-Life 1.1.0.0 on; every Steam install carries valve/delta.lst.
# Four disc variants of that content (full, no intro videos, padded to
# 600 MB, an 8 MB menu-only subset) all showed the identical splash-and-
# idle signature, which is exactly why this script now refuses to pack a
# gamedir that provides no delta.lst (loose, any case, or inside a .pak -
# the pak directory is scanned, not guessed) unless --delta-lst names a
# file to supply, taken from the user's own install of a later release
# (e.g. the Steam valve/delta.lst) - never from this repository.
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

# extract-xiso is a built, untracked nxdk artifact: a git worktree has
# none, so honour a caller's NXDK_DIR (nxdk's own activate variable)
# pointing at the main checkout's built nxdk - see
# tools/xbox-link-check.sh's own NXDK_DIR line for the reasoning.
# A checkout may carry its nxdk location in .sweep/nxdk-dir (the agentic
# loop's setup stage writes it into each worktree). docs/r3d/lessons.md's
# entry on /c/msys64/usr/bin/bash shows an exported variable can be dropped
# at that shell boundary with no error, so the checkout itself answers
# when the environment does not. Unset and no file: the default below.
[ -n "${NXDK_DIR:-}" ] || NXDK_DIR="$(cat "$REPO/.sweep/nxdk-dir" 2>/dev/null || true)"
EXTRACT_XISO="${NXDK_DIR:-$REPO/deps/nxdk}/tools/extract-xiso/build/extract-xiso.exe"
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
	echo "usage: $0 <hl|op4|bshift> --steam <path> --xbe <path> [--out <path>] [--menu-bake <dir>] [--sky-bake <dir>] [--delta-lst <file>] [--resource-dir <dir>] [--ui-language <lang>]" >&2
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
SKY_BAKE=""
DELTA_LST=""
RESOURCE_DIR=""
UI_LANGUAGE=""
OVERLAY_DIR=""
while [ $# -gt 0 ]; do
	case "$1" in
		--steam) STEAM="$2"; shift 2 ;;
		--xbe) XBE="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--menu-bake) MENU_BAKE="$2"; shift 2 ;;
		--sky-bake) SKY_BAKE="$2"; shift 2 ;;
		--delta-lst) DELTA_LST="$2"; shift 2 ;;
		--resource-dir) RESOURCE_DIR="$2"; shift 2 ;;
		--ui-language) UI_LANGUAGE="$2"; shift 2 ;;
		--overlay) OVERLAY_DIR="$2"; shift 2 ;;
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
# liblist.gam-only content (no gameinfo.txt - every pre-Steam, box-retail
# install) is a tested path as of 2026-09-16: FS_ParseGameInfo derives
# gameinfo from liblist.gam and boot proceeds through FS_Init,
# FS_LoadGameInfo, input and server init exactly as with Steam content
# (traced live with tools/xemu-debug.py breakpoints). What such content
# actually lacks is delta.lst - see the gate below.
[ -f "$GAMEDIR_SRC/gameinfo.txt" ] || echo "xbox-xiso: note - $GAMEDIR_SRC has no gameinfo.txt; the engine derives it from liblist.gam." >&2

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
# divergences.md #131: `--bake-menu-art --menu-art-dxt1` writes opaque
# DXT1 .dds tiles instead (ref/nv2a uploads them compressed). dds_dims
# reads the same 128-byte header tools/asset-studio's makeDxt1Dds writes
# (UiPacker.cpp): "DDS " at 0, height at 12, width at 16, FourCC "DXT1" at
# 84. ref/nv2a sizes a DXT texture only by log2 BASE_SIZE fields, so a
# tile that is not a power of two on both axes would be refused at load -
# and one refused tile drops the whole background - so that is checked
# here too, with the exact file size (128 + whole 4x4 blocks * 8).
dds_dims() { # <path>: echoes "<w> <h>" for a DXT1 .dds, or fails
	local magic fourcc w h
	magic=$(head -c 4 "$1" 2>/dev/null)
	fourcc=$(dd if="$1" bs=1 skip=84 count=4 2>/dev/null)
	[ "$magic" = "DDS " ] && [ "$fourcc" = "DXT1" ] || return 1
	h=$(od -An -tu4 -j12 -N4 "$1" 2>/dev/null | tr -d '[:space:]')
	w=$(od -An -tu4 -j16 -N4 "$1" 2>/dev/null | tr -d '[:space:]')
	case "$w$h" in *[!0-9]*|"") return 1 ;; esac
	printf '%s %s\n' "$w" "$h"
}
is_pot() { [ "$1" -gt 0 ] && [ $(( $1 & ($1 - 1) )) -eq 0 ]; }
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
		case "$tilePath" in
		*.dds)
			if ! dims=$(dds_dims "$tileFile"); then
				echo "xbox-xiso: $tileFile: not a DXT1 .dds (bad magic or FourCC, or unreadable)." >&2
				return 1
			fi
			read -r w h <<<"$dims"
			if ! is_pot "$w" || ! is_pot "$h" || [ "$w" -lt 4 ] || [ "$h" -lt 4 ]; then
				echo "xbox-xiso: $tileFile: ${w}x${h} - ref/nv2a needs power-of-two DXT tiles of at least 4x4." >&2
				return 1
			fi
			expectedSize=$((128 + (w / 4) * (h / 4) * 8))
			actualSize=$(wc -c < "$tileFile")
			if [ "$actualSize" -ne "$expectedSize" ]; then
				echo "xbox-xiso: $tileFile: $actualSize bytes, expected $expectedSize for a one-level ${w}x${h} DXT1 .dds - truncated or corrupt." >&2
				return 1
			fi
			;;
		*)
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
			;;
		esac
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

# X9-X item 14 step 10 (divergences.md #137): --sky-bake overlays
# tools/asset-studio --bake-sky-dxt1's output (gfx/env/<name><side>.dds)
# onto the staged GAMEDIR, after the retail copy, beside the retail TGAs.
# The engine's CheckSkybox (ref_common.c:71) tries .dds first and needs all
# six sides in one extension, so validate_sky_bake checks every set is
# complete (rt bk lf ft up dn) and every side is a one-level power-of-two
# DXT1 of the exact size - a bad side would make ref/nv2a refuse it and
# the engine fall back to its default sky. Checked before the copy and
# again on the staged copy, as validate_menu_bake is.
validate_sky_bake() {
	local env="$1/gfx/env" f base side dims w h expectedSize actualSize sets=0
	if [ ! -d "$env" ]; then
		echo "xbox-xiso: $1 has no gfx/env - not a tools/asset-studio --bake-sky-dxt1 output." >&2
		return 1
	fi
	for f in "$env"/*rt.dds; do
		[ -f "$f" ] || continue
		base="${f%rt.dds}"
		for side in rt bk lf ft up dn; do
			if [ ! -f "$base$side.dds" ]; then
				echo "xbox-xiso: sky set $(basename "$base") is missing its $side side." >&2
				return 1
			fi
			if ! dims=$(dds_dims "$base$side.dds"); then
				echo "xbox-xiso: $base$side.dds: not a DXT1 .dds." >&2
				return 1
			fi
			read -r w h <<<"$dims"
			if ! is_pot "$w" || ! is_pot "$h" || [ "$w" -lt 4 ] || [ "$h" -lt 4 ]; then
				echo "xbox-xiso: $base$side.dds: ${w}x${h} - ref/nv2a needs power-of-two DXT of at least 4x4." >&2
				return 1
			fi
			expectedSize=$((128 + (w / 4) * (h / 4) * 8))
			actualSize=$(wc -c < "$base$side.dds")
			if [ "$actualSize" -ne "$expectedSize" ]; then
				echo "xbox-xiso: $base$side.dds: $actualSize bytes, expected $expectedSize - truncated or corrupt." >&2
				return 1
			fi
		done
		sets=$((sets + 1))
	done
	if [ "$sets" -eq 0 ]; then
		echo "xbox-xiso: $env holds no complete .dds sky set - refusing an empty bake." >&2
		return 1
	fi
	echo "$sets"
}
if [ -n "$SKY_BAKE" ]; then
	[ -d "$SKY_BAKE" ] || { echo "xbox-xiso: --sky-bake path does not exist or is not a directory: $SKY_BAKE" >&2; exit 1; }
	if ! SKY_SETS=$(validate_sky_bake "$SKY_BAKE"); then
		echo "xbox-xiso: --sky-bake $SKY_BAKE failed validation - refusing to pack." >&2
		exit 1
	fi
	echo "xbox-xiso: overlaying $SKY_SETS baked sky set(s) from $SKY_BAKE onto $GAMEDIR..." >&2
	mkdir -p "$STAGE/$GAMEDIR/gfx/env" || exit 1
	if ! cp "$SKY_BAKE"/gfx/env/*.dds "$STAGE/$GAMEDIR/gfx/env/"; then
		echo "xbox-xiso: overlaying $SKY_BAKE onto $STAGE/$GAMEDIR failed." >&2
		exit 1
	fi
	if ! STAGED_SETS=$(validate_sky_bake "$STAGE/$GAMEDIR") || [ "$STAGED_SETS" -lt "$SKY_SETS" ]; then
		echo "xbox-xiso: staged $GAMEDIR/gfx/env failed the sky bake validation after the copy - copy was incomplete." >&2
		exit 1
	fi
	echo "xbox-xiso: sky bake overlay verified in the stage ($STAGED_SETS set(s))." >&2
fi

# The menu font (XM.1 item 13 step 1). Unlike delta.lst above, this DOES
# come from this repository, and the difference is provenance, not
# convenience: delta.lst is retail content only the user's own install may
# supply, while this is an SIL OFL 1.1 face we are entitled to ship. Its
# licence travels with it because the OFL requires that.
#
# It lives in assets/fonts/, NOT in 3rdparty/extras/xash-extras/, for a
# structural reason: that path is an upstream submodule (FWGS/xash-extras)
# and nothing this fork owns may be added to it. assets/ is this fork's
# own home for content it ships. The face is Fira Sans **Condensed** -
# see FindFontDataFile in FontManager.cpp for why that and not the design
# canvas's Barlow Semi Condensed (Barlow cannot draw Cyrillic at all).
#
# Staged unconditionally, because the alternative failure is silent:
# CStbFont::Create asks the filesystem for the mapped path
# ("Trebuchet MS"), and when the file is absent CFontBuilder::Create falls
# back to CBitmapFont without a word (FontManager.cpp:506-514) - engine
# conchars, ASCII 33-126 plus Cyrillic and no accented Latin at all
# (BitmapFont.cpp:87-93). Every menu string then draws in a font that
# cannot spell "Trainingsgelände", and English looks identical either way,
# which is exactly how this went unnoticed until item 13 went looking. A
# disc that cannot render its own menu is not a disc worth packing, so a
# missing source font is fatal here.
#
# tahoma.ttf is deliberately NOT staged, though the tree carries it and
# FindFontDataFile maps "Tahoma" (the console face, FontManager.cpp:102)
# to it. Codex review caught the reason: it is Wine's Tahoma under LGPL
# 2.1-or-later, not OFL, and its own notice points at a COPYING.LIB this
# tree does not have - so shipping it would put materially wrong
# provenance and an incomplete licence on every disc. Leaving it off costs
# one face, which falls back to conchars: the 11 px help/console text,
# already the face item 13 step 2 accepted as the one that does not get
# its ideal typeface, and consistent with divergence #63 putting the
# engine console on WON conchars anyway. It also returns the 512 KiB that
# face measured. Shipping a real console face again means an OFL (or
# similarly unencumbered) one, not this file.
FONTS_SRC="$REPO/assets/fonts"
if [ ! -d "$FONTS_SRC" ]; then
	echo "xbox-xiso: menu fonts are missing from $FONTS_SRC - the menu would" >&2
	echo "  silently fall back to console glyphs with no accented Latin." >&2
	exit 1
fi
mkdir -p "$STAGE/$GAMEDIR/gfx/fonts"
for f in FiraSansCondensed-Regular.ttf FiraSansCondensed-OFL.txt; do
	if [ ! -f "$FONTS_SRC/$f" ]; then
		echo "xbox-xiso: menu font file $FONTS_SRC/$f is missing." >&2
		exit 1
	fi
	if ! cp "$FONTS_SRC/$f" "$STAGE/$GAMEDIR/gfx/fonts/$f"; then
		echo "xbox-xiso: copying $f into the stage failed." >&2
		exit 1
	fi
done
# Verify after copying rather than trusting cp, the same way every other
# stage gate in this script does - and compare sizes, since a truncated
# TTF loads as far as stbtt_InitFont and then fails the same silent way.
for f in FiraSansCondensed-Regular.ttf; do
	src_sz=$(wc -c < "$FONTS_SRC/$f")
	dst_sz=$(wc -c < "$STAGE/$GAMEDIR/gfx/fonts/$f" 2>/dev/null || echo 0)
	if [ "$src_sz" != "$dst_sz" ]; then
		echo "xbox-xiso: staged gfx/fonts/$f is $dst_sz bytes, source is $src_sz - copy was incomplete." >&2
		exit 1
	fi
done
echo "xbox-xiso: menu font staged into $GAMEDIR/gfx/fonts and verified." >&2

# The menu palette (XM.1 item 13 step 2). Same provenance rule as the
# font above - this fork's own content, staged unconditionally.
#
# Fatal if the SOURCE is missing, same as the font/dictionary gates above
# - Codex review caught this drawing the wrong conclusion from a true
# fact: UI_ApplyCustomColors treats an absent colors.lst *on the disc* as
# normal at runtime (BaseMenu.cpp's own "not found" log is deliberately
# not an error there, and every colour it can set has a stock default),
# but that is a statement about the ENGINE's own tolerance for a disc
# that never had this file, not licence for THIS script to tolerate its
# own tracked source file vanishing. assets/gfx/shell/colors.lst is
# owned content that always exists in this tree; its absence here means
# an incomplete checkout or a broken export, not a deliberate choice to
# ship stock colours - so it must fail loudly, exactly like a missing
# font, rather than silently produce a disc with the wrong colours and
# exit 0.
COLORS_SRC="$REPO/assets/gfx/shell/colors.lst"
if [ ! -f "$COLORS_SRC" ]; then
	echo "xbox-xiso: $COLORS_SRC is missing." >&2
	exit 1
fi
mkdir -p "$STAGE/$GAMEDIR/gfx/shell"
if ! cp "$COLORS_SRC" "$STAGE/$GAMEDIR/gfx/shell/colors.lst"; then
	echo "xbox-xiso: copying colors.lst into the stage failed." >&2
	exit 1
fi
if ! cmp -s "$COLORS_SRC" "$STAGE/$GAMEDIR/gfx/shell/colors.lst"; then
	echo "xbox-xiso: staged gfx/shell/colors.lst differs from source - copy was incomplete." >&2
	exit 1
fi
echo "xbox-xiso: menu palette staged into $GAMEDIR/gfx/shell/colors.lst and verified." >&2

# The legend's own face-button glyph (XM.1 item 12) - a plain tintable
# circle, this fork's own content, same provenance and fatal-if-missing
# rule as the font and colors.lst above. The D-pad and START glyphs need
# no asset at all (CMenuLegend draws them with UI_FillRect/
# UI_DrawRectangleExt directly), so this is the only new image this item
# adds.
LEGEND_BUTTON_SRC="$REPO/assets/gfx/shell/legend_button.tga"
if [ ! -f "$LEGEND_BUTTON_SRC" ]; then
	echo "xbox-xiso: $LEGEND_BUTTON_SRC is missing." >&2
	exit 1
fi
mkdir -p "$STAGE/$GAMEDIR/gfx/shell"
if ! cp "$LEGEND_BUTTON_SRC" "$STAGE/$GAMEDIR/gfx/shell/legend_button.tga"; then
	echo "xbox-xiso: copying legend_button.tga into the stage failed." >&2
	exit 1
fi
if ! cmp -s "$LEGEND_BUTTON_SRC" "$STAGE/$GAMEDIR/gfx/shell/legend_button.tga"; then
	echo "xbox-xiso: staged gfx/shell/legend_button.tga differs from source - copy was incomplete." >&2
	exit 1
fi
echo "xbox-xiso: legend glyph staged into $GAMEDIR/gfx/shell/legend_button.tga and verified." >&2

# The Controller screen's Duke diagram (XM.1 item 5) - this fork's own
# content, baked by tools/xbox-menu-art.py and already logged as
# divergence #74, same provenance and fatal-if-missing rule as the other
# shell art above. Staged ahead of the screen that loads it (branch 2 of
# 5 in that item's implementation sequence) so later branches don't have
# to touch this script at all.
DUKE_SRC="$REPO/assets/gfx/shell/duke.tga"
if [ ! -f "$DUKE_SRC" ]; then
	echo "xbox-xiso: $DUKE_SRC is missing." >&2
	exit 1
fi
mkdir -p "$STAGE/$GAMEDIR/gfx/shell"
if ! cp "$DUKE_SRC" "$STAGE/$GAMEDIR/gfx/shell/duke.tga"; then
	echo "xbox-xiso: copying duke.tga into the stage failed." >&2
	exit 1
fi
if ! cmp -s "$DUKE_SRC" "$STAGE/$GAMEDIR/gfx/shell/duke.tga"; then
	echo "xbox-xiso: staged gfx/shell/duke.tga differs from source - copy was incomplete." >&2
	exit 1
fi
echo "xbox-xiso: controller diagram staged into $GAMEDIR/gfx/shell/duke.tga and verified." >&2

# The localization dictionaries (XM.1 item 13 step 4), and the gate that
# makes them non-optional. Once the Xbox build draws menu rows as text
# rather than button-strip bitmaps, every row's label is whatever the
# string layer returns - and L() returns its own key on a miss
# (MenuStrings.cpp:75-89), with GameUI_* having no compiled-in fallback.
# A classic WON install ships no resource/ directory at all, so without
# these files Main would read "GameUI_NewGame", "GameUI_TrainingRoom",
# "GameUI_LoadGame", "GameUI_Options" on screen. StringsList_* keys do
# resolve from a compiled-in table (MenuStrings.cpp:35, :485-492), which
# is exactly why the status lines under those labels looked correct all
# along and hid this.
#
# Same provenance rule as delta.lst: these are content, taken from the
# user's own install via --resource-dir, never from this repository.
RESOURCE_FILES="gameui_english.txt valve_english.txt"
have_dicts=1
for d in $RESOURCE_FILES; do
	[ -f "$STAGE/$GAMEDIR/resource/$d" ] || have_dicts=0
done
if [ "$have_dicts" -eq 0 ] && [ -n "$RESOURCE_DIR" ]; then
	[ -d "$RESOURCE_DIR" ] || { echo "xbox-xiso: --resource-dir does not exist: $RESOURCE_DIR" >&2; exit 1; }
	mkdir -p "$STAGE/$GAMEDIR/resource"
	for d in $RESOURCE_FILES; do
		if [ ! -f "$RESOURCE_DIR/$d" ]; then
			echo "xbox-xiso: --resource-dir $RESOURCE_DIR has no $d." >&2
			exit 1
		fi
		if ! cp "$RESOURCE_DIR/$d" "$STAGE/$GAMEDIR/resource/$d"; then
			echo "xbox-xiso: copying $d into the stage failed." >&2
			exit 1
		fi
		if ! cmp -s "$RESOURCE_DIR/$d" "$STAGE/$GAMEDIR/resource/$d"; then
			echo "xbox-xiso: staged resource/$d differs from source - copy was incomplete." >&2
			exit 1
		fi
	done
	have_dicts=1
	echo "xbox-xiso: localization dictionaries supplied from $RESOURCE_DIR and verified." >&2
fi

# This fork's own translations (XM.1 item 13 step 3), staged from assets/
# alongside the retail dictionaries above. Keys in these files are the
# ORIGINAL ENGLISH STRINGS, not invented identifiers - that is mainui's
# own documented convention (translations/mainui_skeleton_*.txt: "Tokens
# on left are original strings, on right is translation"), and it is why
# no menu source file needed changing to become translatable: L() already
# looks the literal up and returns it unchanged when there is no entry,
# so English is the built-in fallback.
#
# mainui_<lang>.txt loads AFTER gameui_<lang>.txt and BEFORE the gamedir's
# own file (MenuStrings.cpp Localize_InitLanguage), so these may override
# GameUI_*/StringsList_* labels but cannot override valve_<lang>.txt.
shopt -s nullglob
r3d_translations=( "$REPO"/assets/resource/mainui_*.txt )
shopt -u nullglob
if [ ${#r3d_translations[@]} -gt 0 ]; then
	mkdir -p "$STAGE/$GAMEDIR/resource"
	for t in "${r3d_translations[@]}"; do
		bn="$(basename "$t")"
		if ! cp "$t" "$STAGE/$GAMEDIR/resource/$bn"; then
			echo "xbox-xiso: copying $bn into the stage failed." >&2
			exit 1
		fi
		if ! cmp -s "$t" "$STAGE/$GAMEDIR/resource/$bn"; then
			echo "xbox-xiso: staged resource/$bn differs from source - copy was incomplete." >&2
			exit 1
		fi
	done
	echo "xbox-xiso: staged ${#r3d_translations[@]} Resonance3D translation file(s)." >&2
fi

# --ui-language writes the gamedir's vfs.cfg, which is where the engine
# actually picks the language up: FS_LoadVFSConfig execs <gamedir>/vfs.cfg
# during filesystem init (filesystem_engine.c:115-116), long before
# CL_Init reaches the menu - unlike userconfig.d or config.cfg, which both
# exec too late for Localize_Init to see (the same ordering trap item 0
# hit with ui_xbox_menu_art).
if [ -n "$UI_LANGUAGE" ]; then
	case "$UI_LANGUAGE" in
		*[!a-z]*) echo "xbox-xiso: --ui-language must be a lowercase language name: $UI_LANGUAGE" >&2; exit 1 ;;
	esac
	if [ ! -f "$STAGE/$GAMEDIR/resource/mainui_$UI_LANGUAGE.txt" ] 	   && [ ! -f "$STAGE/$GAMEDIR/resource/gameui_$UI_LANGUAGE.txt" ]; then
		echo "xbox-xiso: --ui-language $UI_LANGUAGE but the stage has no" >&2
		echo "  resource/mainui_$UI_LANGUAGE.txt or resource/gameui_$UI_LANGUAGE.txt -" >&2
		echo "  the menu would fall back to English and the disc would lie about it." >&2
		exit 1
	fi
	# Rewrite vfs.cfg by REPLACING our two directives and preserving every
	# other line. Found in Codex review: truncating the file would silently
	# drop fs_mount_hd/fs_mount_lv/fs_mount_addon/fs_mount_l10n, which decide
	# which content directories the engine mounts (filesystem_engine.c:91-129)
	# - so picking a menu language could quietly remove HD or low-violence
	# content from the disc. The classic WON install ships no vfs.cfg at all,
	# which is exactly why the first version of this looked fine.
	vfs="$STAGE/$GAMEDIR/vfs.cfg"
	kept=0
	if [ -f "$vfs" ]; then
		kept=$(grep -c -v -iE '^[[:space:]]*(ui_language|fs_mount_l10n)([[:space:]]|$)' "$vfs" || true)
		grep -v -iE '^[[:space:]]*(ui_language|fs_mount_l10n)([[:space:]]|$)' "$vfs" > "$vfs.tmp" || true
	else
		: > "$vfs.tmp"
	fi
	# fs_mount_l10n mirrors what the engine's own -language path does
	# (filesystem_engine.c:119-123): selecting a language there also turns on
	# localization mounting, so a disc built for a language behaves the same
	# way as an engine launched for one.
	printf 'ui_language "%s"
' "$UI_LANGUAGE" >> "$vfs.tmp"
	printf 'fs_mount_l10n "1"
' >> "$vfs.tmp"
	mv "$vfs.tmp" "$vfs"
	if ! grep -q "ui_language \"$UI_LANGUAGE\"" "$vfs"; then
		echo "xbox-xiso: writing $GAMEDIR/vfs.cfg failed." >&2
		exit 1
	fi
	now_kept=$(grep -c -v -iE '^[[:space:]]*(ui_language|fs_mount_l10n)([[:space:]]|$)' "$vfs" || true)
	if [ "$now_kept" != "$kept" ]; then
		echo "xbox-xiso: $GAMEDIR/vfs.cfg lost directives while setting the language" >&2
		echo "  ($kept before, $now_kept after) - refusing to pack." >&2
		exit 1
	fi
	echo "xbox-xiso: $GAMEDIR/vfs.cfg sets ui_language to $UI_LANGUAGE ($kept other directive(s) preserved)." >&2
fi
if [ "$have_dicts" -eq 0 ]; then
	echo "xbox-xiso: $GAMEDIR provides no resource/gameui_english.txt." >&2
	echo "  The Xbox menu draws its rows as text, so without it every label" >&2
	echo "  renders as its own lookup key (\"GameUI_NewGame\" and friends)." >&2
	echo "  Supply them with --resource-dir <dir> from an install that has" >&2
	echo "  them (a Steam/HL25 valve/resource), the same way --delta-lst works." >&2
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

# The delta.lst gate (see the header): the staged gamedir, or its basedir,
# must provide delta.lst somewhere the engine's FS_LoadFile will find it -
# a loose file in any case (the Xbox filesystem layer is case-insensitive)
# or an entry inside any .pak, which is where pre-Steam content keeps most
# of its files. The pak directory is read from the file's own header
# (offset 4: directory offset, offset 8: directory size, then 64-byte
# entries of a 56-byte NUL-padded name plus offset/size - filesystem/pak.c
# and the Quake PACK format), NOT guessed from a grep over the raw bytes,
# which a sound file called `delta.lst.wav` or a stray string could fool.
PACK_MAGIC="$REPO/.sweep-xiso/.pack-magic.bin"
printf 'PACK' > "$PACK_MAGIC"
pak_entries() { # <pak>: "name<TAB>offset<TAB>size" per entry, name lowercased; fails on a non-PACK file
	local off size
	cmp -s -n 4 "$PACK_MAGIC" "$1" || return 1
	off=$(od -An -tu4 -j4 -N4 "$1" 2>/dev/null | tr -d '[:space:]')
	size=$(od -An -tu4 -j8 -N4 "$1" 2>/dev/null | tr -d '[:space:]')
	case "$off$size" in *[!0-9]*|"") return 1 ;; esac
	od -An -v -tx1 -w64 -j "$off" -N "$size" "$1" | awk '{
		if( NF < 64 ) next
		s = ""
		for( i = 1; i <= 56; i++ ) {
			if( $i == "00" ) break
			s = s sprintf( "%c", strtonum( "0x" $i ))
		}
		printf "%s\t%d\t%d\n", tolower( s ), strtonum( "0x" $60 $59 $58 $57 ), strtonum( "0x" $64 $63 $62 $61 )
	}'
}
# find_delta_lst <dir>: prints "loose<TAB><path>" or "pak<TAB><pak><TAB><offset><TAB><size>"
# for the delta.lst the ENGINE would load from that directory, or fails.
# Mirrors FS_AddGameDirectory (filesystem/searchpath.c:215-249): the
# directory listing is sorted with Q_strcmp - byte order, case-sensitive
# (filesystem/sys.c:123-137) - every .pak is mounted in that order and each
# one is PREPENDED to the search list (:161-162), so the pak that sorts
# LAST is searched first; the directory itself is added after all archives
# and its loose files win over every pak (:244-246). Codex review round 2:
# the first version returned the first .pak the shell glob happened to
# yield, so a good pak0.pak would pass the gate while the engine loaded a
# broken pak1.pak. Only .pak archives are considered - the other archive
# types g_archives knows (.pk3, .wad) are not where retail keeps delta.lst.
find_delta_lst() {
	local dir="$1" loose name pak hit
	loose=$(find "$dir" -maxdepth 1 -type f -iname delta.lst 2>/dev/null | head -n1)
	if [ -n "$loose" ]; then
		printf 'loose\t%s\n' "$loose"
		return 0
	fi
	while IFS= read -r name; do
		[ -n "$name" ] || continue
		pak="$dir/$name"
		if ! pak_entries "$pak" > "$REPO/.sweep-xiso/.pak-entries.txt"; then
			echo "xbox-xiso: WARNING - $pak is not a readable PACK archive; the engine would not mount it either. Skipped." >&2
			continue
		fi
		hit=$(awk -F'\t' '$1 == "delta.lst" { printf "%s\t%s", $2, $3; exit }' "$REPO/.sweep-xiso/.pak-entries.txt")
		if [ -n "$hit" ]; then
			printf 'pak\t%s\t%s\n' "$pak" "$hit"
			return 0
		fi
	done < <(find "$dir" -maxdepth 1 -type f -iname '*.pak' -printf '%f\n' 2>/dev/null | LC_ALL=C sort -r)
	return 1
}
# materialize_delta_lst <find_delta_lst line> <out>: copies the loose file,
# or extracts the pak entry by its directory offset/size, into <out>.
materialize_delta_lst() {
	local kind a b c out="$2"
	IFS=$'\t' read -r kind a b c <<<"$1"
	case "$kind" in
		loose) cp "$a" "$out" ;;
		pak)
			tail -c +$(( b + 1 )) "$a" | head -c "$c" > "$out" || return 1
			[ "$(wc -c < "$out")" -eq "$c" ]
			;;
		*) return 1 ;;
	esac
}
# validate_delta_lst <file> [<label>]: the same grammar Delta_InitFields
# walks (engine/common/net_encode.c:849-885), token for token, so a file
# that would Sys_Error there is refused here instead of being packaged
# into the very boot loop this gate exists to prevent (Codex review round
# 1: the first version only grepped for section names and a brace, and a
# file made of the seven names followed by one '{' passed it). The
# tokenizer is COM_ParseFileSafe's (public/crtlib.c:791-): anything <= ' '
# is whitespace, `//` runs to end of line, "..." is one token, and each of
# { } ( ) , ' is a token by itself. What is fatal in the engine is fatal
# here: a section name outside dt_info[] (:860-865, case-insensitive
# match), and anything but '{' - end of file included - after the
# `<struct> <none|gamedll|clientdll> [<function>]` header (:874-879).
# Deliberately STRICTER than the engine on three things it tolerates by
# silently dropping data: a DEFINE_DELTA/DEFINE_DELTA_POST whose syntax
# Delta_ParseField rejects (:660-796, Con_DPrintf and the field is lost),
# a flag outside the DT_* set (:707-724, ignored), and a stray token or a
# missing '}' inside a block (:809-828, skipped / parsed to end of file) -
# a disc built from such a file boots but sends the wrong netcode, which
# is not a packaging outcome worth reporting as success. Field NAMES are
# checked against the engine's own per-struct tables (Delta_FindFieldInfo,
# :505-517, Q_strcmp - exact and case-sensitive; an unknown name drops the
# field at :678-683), derived at run time from net_encode.c's `*_DEF( x )`
# entries and its dt_info[] struct->table mapping rather than copied here,
# so they cannot drift from the engine (Codex review round 2). Round 2
# also found that seven syntactically valid but EMPTY sections passed:
# Delta_ParseTable marks a table initialized with numFields = 0
# (:805-846), so every required section must parse at least one field.
# The seven sections every retail delta.lst declares are required: the
# engine asserts on the first packet that needs one it never read (:590).
DELTA_KNOWN_STRUCTS="event_t movevars_t usercmd_t clientdata_t weapon_data_t entity_state_t entity_state_player_t custom_entity_state_t delta_test_struct_t"
DELTA_REQUIRED_STRUCTS="clientdata_t entity_state_t entity_state_player_t custom_entity_state_t usercmd_t weapon_data_t event_t"
delta_field_tables() { # "struct<TAB>field" per line, read out of net_encode.c itself
	awk '
	/^static const delta_field_t [a-z_]+\[\] *=/ {
		match( $0, /delta_field_t [a-z_]+/ )
		cur = substr( $0, RSTART + 14, RLENGTH - 14 )
		next
	}
	cur != "" && /^};/ { cur = ""; next }
	cur != "" && match( $0, /_DEF_?\( *[^,)]+/ ) {
		f = substr( $0, RSTART, RLENGTH )
		sub( /_DEF_?\( */, "", f )
		gsub( / /, "", f )
		tbl[cur] = tbl[cur] f "\n"
		next
	}
	/^\[DT_[A-Z_0-9]+\] *= *\{ *"[a-z_]+", *[a-z_]+/ {
		match( $0, /"[a-z_]+"/ )
		s = substr( $0, RSTART + 1, RLENGTH - 2 )
		rest = substr( $0, RSTART + RLENGTH )
		match( rest, /[a-z_]+/ )
		map[s] = substr( rest, RSTART, RLENGTH )
	}
	END {
		for( s in map ) {
			n = split( tbl[map[s]], arr, "\n" )
			for( i = 1; i <= n; i++ ) if( arr[i] != "" ) printf "%s\t%s\n", s, arr[i]
		}
	}' "$REPO/engine/common/net_encode.c"
}
# The tables go to awk through a file, not `-v`: under the MSYS2 bash this
# script is documented to run in, an `awk -v` value of ~11 KB reached awk
# truncated, so the last entries of whichever struct landed at the end
# (weapon_data_t's m_iWeaponState and later) were "unknown" and the real
# Steam delta.lst was refused - while the same command under Git Bash
# passed. Found in the live packaging matrix, not in the isolation tests,
# which had run under the other shell.
DELTA_FIELDS_FILE="$REPO/.sweep-xiso/.delta-fields.tsv"
delta_field_tables > "$DELTA_FIELDS_FILE" || { echo "xbox-xiso: could not read the delta field tables out of engine/common/net_encode.c. Refusing to pack." >&2; exit 1; }
for name in $DELTA_REQUIRED_STRUCTS; do
	if [ "$(grep -c "^$name	" "$DELTA_FIELDS_FILE")" -lt 3 ]; then
		echo "xbox-xiso: could not read $name's field table out of engine/common/net_encode.c - the delta.lst check cannot run. Refusing to pack." >&2
		exit 1
	fi
done
validate_delta_lst() {
	local f="$1" label="${2:-$1}"
	if [ ! -s "$f" ]; then
		echo "xbox-xiso: $label is empty or unreadable." >&2
		return 1
	fi
	awk -v label="$label" -v known="$DELTA_KNOWN_STRUCTS" -v required="$DELTA_REQUIRED_STRUCTS" -v fieldfile="$DELTA_FIELDS_FILE" '
	function fail( msg ) { printf "xbox-xiso: %s: %s\n", label, msg > "/dev/stderr"; exit 1 }
	function emit( t ) { if( t != "" ) { n++; tok[n] = t } }
	function tokat( i ) { return ( i <= n ) ? tok[i] : "<end of file>" }
	function isnum( t ) { return t ~ /^-?([0-9]+(\.[0-9]*)?|\.[0-9]+)$/ }
	BEGIN {
		ntbl = 0
		while(( getline line < fieldfile ) > 0 ) {
			sub( /\r$/, "", line )
			if( line == "" ) continue
			tbl[line] = 1; split( line, kv, "\t" ); tblcount[kv[1]]++; ntbl++
		}
		close( fieldfile )
		if( ntbl == 0 ) fail( "internal: no field table could be read from " fieldfile )
		RS = "\001"
	}
	{
		s = $0; L = length( s ); cur = ""; n = 0
		for( i = 1; i <= L; i++ ) {
			c = substr( s, i, 1 )
			if( c == "/" && substr( s, i + 1, 1 ) == "/" ) {
				emit( cur ); cur = ""
				while( i <= L && substr( s, i, 1 ) != "\n" ) i++
				continue
			}
			if( c == "\"" ) {
				emit( cur ); cur = ""; i++
				while( i <= L && substr( s, i, 1 ) != "\"" ) { cur = cur substr( s, i, 1 ); i++ }
				emit( cur ); cur = ""
				continue
			}
			if( c <= " " ) { emit( cur ); cur = ""; continue }
			if( c == "{" || c == "}" || c == "(" || c == ")" || c == "," || c == "\x27" ) { emit( cur ); cur = ""; emit( c ); continue }
			cur = cur c
		}
		emit( cur )
	}
	END {
		split( known, ka, " " ); for( k in ka ) if( ka[k] != "" ) isknown[tolower( ka[k] )] = 1
		p = 1
		if( n == 0 ) fail( "contains no tokens at all" )
		while( p <= n ) {
			name = tok[p++]
			if( !( tolower( name ) in isknown )) fail( "section \x27" name "\x27 is not a struct engine/common/net_encode.c\x27s dt_info table knows - Sys_Error at net_encode.c:864" )
			seen[tolower( name )] = 1
			enc = tokat( p ); p++
			if( p > n + 1 ) fail( "section " name " ends before its encode type - Sys_Error (missing \x27{\x27) at net_encode.c:878" )
			if( tolower( enc ) != "none" ) { p++ }
			if( tokat( p ) != "{" ) fail( "section " name ": expected \x27{\x27 after its header, found \x27" tokat( p ) "\x27 - Sys_Error at net_encode.c:878" )
			p++
			closed = 0
			while( p <= n ) {
				t = tok[p++]
				if( t == "}" ) { closed = 1; break }
				if( t != "DEFINE_DELTA" && t != "DEFINE_DELTA_POST" ) fail( "section " name ": unexpected token \x27" t "\x27 inside its block - the engine would skip it (net_encode.c:809-828)" )
				post = ( t == "DEFINE_DELTA_POST" )
				if( tokat( p ) != "(" ) fail( "section " name ": " t " not followed by \x27(\x27 (found \x27" tokat( p ) "\x27) - the field would be dropped (net_encode.c:665)" )
				p++
				fname = tokat( p ); p++
				if( fname == "<end of file>" || fname ~ /^[(){},]$/ ) fail( "section " name ": " t " has no field name - the field would be dropped (net_encode.c:672)" )
				if( tblcount[tolower( name )] > 0 && !(( tolower( name ) "\t" fname ) in tbl )) fail( "section " name ": field \x27" fname "\x27 is not in the engine\x27s " name " table - Delta_FindFieldInfo would not find it and the field would be dropped (net_encode.c:678-683)" )
				if( tokat( p ) != "," ) fail( "section " name ": field " fname ": expected \x27,\x27 after the name, found \x27" tokat( p ) "\x27 - the field would be dropped (net_encode.c:686)" )
				p++
				nflags = 0
				while( p <= n && tok[p] != "," ) {
					if( tok[p] != "|" ) {
						if( tok[p] !~ /^DT_(BYTE|SHORT|FLOAT|INTEGER|ANGLE|TIMEWINDOW_8|TIMEWINDOW_BIG|STRING|SIGNED)$/ ) fail( "section " name ": field " fname ": unknown flag \x27" tok[p] "\x27 - the engine would ignore it (net_encode.c:707-724) and encode the field wrongly" )
						nflags++
					}
					p++
				}
				if( p > n ) fail( "section " name ": field " fname ": flags never end with \x27,\x27 - the field would be dropped (net_encode.c:727)" )
				if( nflags == 0 ) fail( "section " name ": field " fname " has no DT_* flag at all" )
				p++
				bits = tokat( p ); p++
				if( bits !~ /^[0-9]+$/ ) fail( "section " name ": field " fname ": bits argument \x27" bits "\x27 is not a number (net_encode.c:734-740)" )
				if( tokat( p ) != "," ) fail( "section " name ": field " fname ": expected \x27,\x27 after bits, found \x27" tokat( p ) "\x27 - the field would be dropped (net_encode.c:743)" )
				p++
				mul = tokat( p ); p++
				if( !isnum( mul )) fail( "section " name ": field " fname ": multiplier \x27" mul "\x27 is not a number (net_encode.c:750-756)" )
				if( post ) {
					if( tokat( p ) != "," ) fail( "section " name ": field " fname ": DEFINE_DELTA_POST expected \x27,\x27 before post_multiply, found \x27" tokat( p ) "\x27 (net_encode.c:761)" )
					p++
					pm = tokat( p ); p++
					if( !isnum( pm )) fail( "section " name ": field " fname ": post_multiply \x27" pm "\x27 is not a number (net_encode.c:768-774)" )
				}
				if( tokat( p ) != ")" ) fail( "section " name ": field " fname ": expected \x27)\x27, found \x27" tokat( p ) "\x27 - the field would be dropped (net_encode.c:784)" )
				p++
				if( p <= n && tok[p] == "," ) p++
				fields[tolower( name )]++
			}
			if( !closed ) fail( "section " name " is never closed with \x27}\x27 - the engine would parse to end of file and stop (net_encode.c:809)" )
		}
		split( required, ra, " " )
		for( r in ra ) {
			if( ra[r] == "" ) continue
			if( !( tolower( ra[r] ) in seen )) fail( "has no \x27" ra[r] "\x27 section - every retail delta.lst declares it, and the engine asserts on the first packet that needs it (net_encode.c:590)" )
			if( fields[tolower( ra[r] )] == 0 ) fail( "section " ra[r] " declares no field - the engine would mark an empty table initialized (net_encode.c:805-846) and send nothing for it" )
		}
		exit 0
	}' "$f"
}
DELTA_CHECK="$REPO/.sweep-xiso/.delta-check.lst"
DELTA_WHERE=""
DELTA_FOUND=""
DELTA_DIRS=( "$STAGE/$GAMEDIR" )
[ "$BASEDIR" != "$GAMEDIR" ] && DELTA_DIRS+=( "$STAGE/$BASEDIR" )
for d in "${DELTA_DIRS[@]}"; do
	if DELTA_FOUND=$(find_delta_lst "$d"); then
		DELTA_WHERE="${d#"$STAGE/"}"
		break
	fi
done
if [ -n "$DELTA_WHERE" ]; then
	if [ -n "$DELTA_LST" ]; then
		echo "xbox-xiso: $DELTA_WHERE already provides delta.lst - refusing --delta-lst, which would shadow the title's own file." >&2
		exit 1
	fi
	# The title's own file gets the same parse as a supplied one: a retail
	# tree edited by hand, or a mod's own broken delta.lst, would otherwise
	# pack into the identical invisible loop.
	if ! materialize_delta_lst "$DELTA_FOUND" "$DELTA_CHECK"; then
		echo "xbox-xiso: could not read $DELTA_WHERE's delta.lst (${DELTA_FOUND//$'\t'/ }) - refusing to pack." >&2
		exit 1
	fi
	if ! validate_delta_lst "$DELTA_CHECK" "$DELTA_WHERE's own delta.lst (${DELTA_FOUND//$'\t'/ })"; then
		echo "xbox-xiso: the title's own delta.lst would not survive Delta_InitFields - refusing to pack a disc that cannot boot." >&2
		exit 1
	fi
	echo "xbox-xiso: delta.lst present in $DELTA_WHERE (${DELTA_FOUND%%$'\t'*}) and parses." >&2
else
	if [ -z "$DELTA_LST" ]; then
		echo "xbox-xiso: $GAMEDIR provides no delta.lst - not loose, not inside any .pak$( [ "$BASEDIR" != "$GAMEDIR" ] && printf ' (nor does %s)' "$BASEDIR" )." >&2
		echo "  The engine calls Sys_Error without it (engine/common/net_encode.c:854) before any" >&2
		echo "  renderer exists to show the message: the disc would sit on the nxdk boot splash" >&2
		echo "  and restart forever. Pre-1.1.0.0 (box-retail) Half-Life never shipped one." >&2
		echo "  Pass --delta-lst <file> with the delta.lst from a later install of this title" >&2
		echo "  (e.g. its Steam valve/delta.lst). Refusing to pack." >&2
		exit 1
	fi
	[ -f "$DELTA_LST" ] || { echo "xbox-xiso: --delta-lst path does not exist: $DELTA_LST" >&2; exit 1; }
	if ! validate_delta_lst "$DELTA_LST"; then
		echo "xbox-xiso: --delta-lst $DELTA_LST failed validation - refusing to pack." >&2
		exit 1
	fi
	echo "xbox-xiso: $GAMEDIR has no delta.lst; supplying $DELTA_LST..." >&2
	if ! cp "$DELTA_LST" "$STAGE/$GAMEDIR/delta.lst"; then
		echo "xbox-xiso: copying $DELTA_LST into the stage failed." >&2
		exit 1
	fi
	if ! cmp -s "$DELTA_LST" "$STAGE/$GAMEDIR/delta.lst"; then
		echo "xbox-xiso: staged delta.lst differs from $DELTA_LST - copy was incomplete." >&2
		exit 1
	fi
	if ! DELTA_FOUND=$(find_delta_lst "$STAGE/$GAMEDIR") || ! materialize_delta_lst "$DELTA_FOUND" "$DELTA_CHECK" || ! cmp -s "$DELTA_LST" "$DELTA_CHECK"; then
		echo "xbox-xiso: the staged $GAMEDIR does not resolve delta.lst to the supplied file after the copy - postcondition failed." >&2
		exit 1
	fi
	echo "xbox-xiso: delta.lst supplied and verified in the stage." >&2
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

# --overlay: the caller's files over the finished stage, last, so nothing
# above (retail copy, menu bake, dictionaries, gameinfo sync) can undo
# them and a disc built without the flag is untouched. Same shape as the
# --resource-dir copy: cp, then cmp, per file - a copy that "succeeds"
# without the bytes arriving is the failure this script keeps finding.
if [ -n "$OVERLAY_DIR" ]; then
	[ -d "$OVERLAY_DIR" ] || { echo "xbox-xiso: --overlay does not exist: $OVERLAY_DIR" >&2; exit 1; }
	overlay_count=0
	while IFS= read -r -d '' src; do
		rel="${src#"$OVERLAY_DIR"/}"
		dst="$STAGE/$rel"
		mkdir -p "$(dirname "$dst")" || { echo "xbox-xiso: cannot create $(dirname "$dst")" >&2; exit 1; }
		if ! cp "$src" "$dst"; then
			echo "xbox-xiso: copying overlay file $rel into the stage failed." >&2
			exit 1
		fi
		if ! cmp -s "$src" "$dst"; then
			echo "xbox-xiso: staged overlay file $rel differs from source - copy was incomplete." >&2
			exit 1
		fi
		overlay_count=$((overlay_count + 1))
		echo "xbox-xiso: overlay: $rel" >&2
	done < <(find "$OVERLAY_DIR" -type f -print0)
	if [ "$overlay_count" -eq 0 ]; then
		echo "xbox-xiso: --overlay $OVERLAY_DIR contains no files - an empty overlay is a mistake, not a no-op." >&2
		exit 1
	fi
	echo "xbox-xiso: $overlay_count overlay file(s) staged and verified from $OVERLAY_DIR." >&2
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
