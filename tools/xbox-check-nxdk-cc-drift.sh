#!/usr/bin/env bash
# Check tools/xbox-cc.sh's hand-copied flags against deps/nxdk/bin/nxdk-cc's
# real ones. Not run automatically by xbox-cc.sh or xbox-sweep.sh - this adds
# real overhead (parsing a shell script and grepping another) and has no
# reason to run on every one of 220 single-file compiles. Run this by hand
# after any `git submodule update` that touches deps/nxdk, or whenever
# xbox-cc.sh's own comment ("keep this in sync with nxdk-cc") is the only
# thing standing between a stale flag set and a build that quietly compiles
# against an outdated toolchain configuration.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-check-nxdk-cc-drift.sh
#
# Found in review, 2026-09-07: xbox-cc.sh:12-14 says "keep this in sync ...
# if nxdk is ever updated" - a comment for a human to notice, not a check
# anything runs. This is that check. It is deliberately loose (existence,
# not exact positional match): xbox-cc.sh reorders nxdk-cc's includes on
# purpose (that reordering is the whole point of xbox-cc.sh existing, see
# its own header comment), so a byte-for-byte diff would flag its own
# intended divergence as drift. What this catches is a flag nxdk-cc passes
# that xbox-cc.sh's text does not contain anywhere at all - the case that
# actually means "nxdk-cc changed and nobody carried it over".
#
# KNOWN LIMITATION, sharpened 2026-09-07, found in review: this is
# ONE-DIRECTIONAL. It only ever asks "is every current nxdk-cc flag present
# somewhere in xbox-cc.sh" - it never asks the reverse, "does xbox-cc.sh
# still carry a flag nxdk-cc has since REMOVED or CHANGED". If a future
# nxdk update drops or rewrites a flag nxdk-cc currently passes, that flag
# just quietly stops appearing in the extraction below and is never checked
# against again - xbox-cc.sh keeps the old, now-stale copy forever, and
# this script reports a clean pass regardless. A symmetric reverse check is
# NOT a small addition: xbox-cc.sh legitimately carries flags of its own
# that never appear in nxdk-cc at all (Xbox-repo-specific defines and
# include paths), so a naive "every xbox-cc.sh flag must exist in nxdk-cc"
# pass would false-positive on all of those - a correct fix needs to track
# which of xbox-cc.sh's flags were hand-copied FROM nxdk-cc in the first
# place, which this script does not do today. Accepted as a real, disclosed
# gap in this manual, human-run tool rather than built now: re-running the
# existing one-directional check after any nxdk-cc update still catches the
# common case (an addition nobody carried over), and a human diffing the
# two files by hand remains the only way to catch a removal until this
# script grows real flag-provenance tracking.
set -u

# Same reasoning as xbox-cc.sh's own PATH line: whatever invoked this bash
# process may have Git Bash's /usr/bin ahead of MSYS2's on PATH regardless
# of MSYSTEM, and Git Bash's sed/grep are a different build from MSYS2's -
# found live while writing this script: the exact grep/sed pipeline below
# gave different, silently wrong results depending on which PATH the
# invoking shell happened to inherit, with no error either way. Pin it
# rather than trust the ambient PATH.
#
# Fail loudly if the pin itself does not exist, rather than silently
# falling through to whatever the ambient PATH resolves to next: found in
# review, a machine with more than one MSYS-like runtime installed (a
# standalone MSYS2 plus Git for Windows' own bundled one, say) could have
# this exact path not exist, and the pin would then be a silent no-op -
# precisely the failure mode this pin exists to close, one layer up. The
# whole project already hard-requires MSYS2 at this path
# (tools/xbox-env.sh, tools/xbox-cc.sh); this check makes that requirement
# a loud failure here too, instead of a quiet wrong answer.

# Found in review, 2026-09-07: the PATH pin two lines below is not enough by
# itself. It changes which sed.exe/grep.exe get resolved as SUBPROCESSES,
# but says nothing about which bash.exe is already running THIS script -
# and mixing a Git-for-Windows bash process (a different msys-2.0.dll build
# from MSYS2's own, confirmed by "bash --version" reporting a different
# version under each) with MSYS2's sed/grep as subprocesses corrupts them:
# reproduced live - `bash tools/xbox-check-nxdk-cc-drift.sh`, the single
# most natural way to invoke this script, produced sixteen
# `sed: -e expression #1, char 20: unterminated 's' command` lines on
# stderr from the per-flag escaping step, and the script STILL printed its
# normal clean-pass line and exited 0 - the exact silent-corruption shape
# this file exists to catch, now caught in itself. No sed/grep call in
# this file checks its own exit status, and a version of the corruption
# was also reproduced where sed exits 0 with no stderr at all while still
# emitting silently wrong output - so an exit-status check on the sed
# calls would not have been sufficient by itself either.
#
# The fix: don't rely on the caller to invoke the right interpreter - make
# this script always run under it. If the running bash is not literally
# /c/msys64/usr/bin/bash.exe, re-exec this same script under that exact
# binary before anything else runs. XBOX_DRIFT_REEXECED guards against
# looping if that binary is somehow missing or the re-exec itself fails to
# replace the process image.
if [ -z "${XBOX_DRIFT_REEXECED:-}" ]; then
	if [ -x "/c/msys64/usr/bin/bash.exe" ]; then
		export XBOX_DRIFT_REEXECED=1
		exec /c/msys64/usr/bin/bash.exe "$0" "$@"
	fi
	echo "xbox-check-nxdk-cc-drift: /c/msys64/usr/bin/bash.exe not found -" >&2
	echo "  cannot guarantee a consistent msys runtime. Refusing to guess;" >&2
	echo "  install MSYS2 at C:\msys64 (see tools/xbox-env.sh) and retry." >&2
	exit 1
fi

MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-check-nxdk-cc-drift: $MSYS2_BIN not found." >&2
	echo "  This project requires MSYS2 installed at C:\\msys64 - see" >&2
	echo "  tools/xbox-env.sh. Refusing to guess at a different grep/sed;" >&2
	echo "  a wrong one has already produced silent, wrong results here" >&2
	echo "  once during this script's own development." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
NXDK_CC="$REPO/deps/nxdk/bin/nxdk-cc"
XBOX_CC="$REPO/tools/xbox-cc.sh"

if [ ! -f "$NXDK_CC" ]; then
	echo "xbox-check-nxdk-cc-drift: $NXDK_CC not found - is deps/nxdk initialised?" >&2
	exit 1
fi

# Every flag-shaped line in nxdk-cc, "$@" and comments excluded, with
# ${NXDK_DIR} normalised out so an include path compares on its suffix
# rather than on a shell variable xbox-cc.sh spells differently ($NXDK_DIR
# vs $REPO-relative forms in its own arrays).
#
# Strip \r explicitly (the `s/\r$//` below), not just trust the working
# tree to be LF: this project's own CRLF-vs-core.autocrlf trap (see
# xbox-cc.sh's build.h auto-apply comment) bit deps/nxdk/bin/nxdk-cc itself
# during this branch's own testing - found and fixed once already, and a
# checker that assumes clean input is exactly the kind of tool that goes
# quiet the next time the same trap fires.
#
# [[:space:]], not \s, and [\] for a literal backslash, not \\: found while
# testing this very script. Whichever grep/sed the ambient PATH resolved to
# before the pin above was added silently matched nothing against `\s`
# despite reporting as GNU grep, and silently failed to strip a trailing
# backslash with `\\$` in either basic or extended sed mode, while the
# bracket-expression form `[\]$` strips it correctly - verified byte-for-
# byte with od/xxd, not assumed from a clean exit status. A checker that
# goes quiet exactly when the flags it is supposed to compare are missing
# would be worse than not writing it: this was only caught by injecting a
# real flag into nxdk-cc with a plain text edit (not a shell one-liner - a
# printf/sed test harness for the injection hit the identical PATH-
# dependent quirk and produced a false negative of its own) and confirming
# the checker actually flagged it, not by trusting the pipeline ran clean.
flags=$( grep -E '^[[:space:]]*-[A-Za-z-]' "$NXDK_CC" \
	| sed -e 's/\r$//' -e 's/[\][[:space:]]*$//' \
	      -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
	      -e 's|\${NXDK_DIR}|@NXDK_DIR@|g' )

# Found in review, 2026-09-07: nothing above checked that this extraction
# actually produced anything. If nxdk-cc is ever reformatted, emptied, or
# replaced upstream in a way the grep pattern above no longer matches, $flags
# comes out empty, the while-loop below never runs even once, $missing stays
# 0, and this script prints its normal clean-pass line with exit 0 - having
# verified nothing. That is the exact shape of the tools/xbox-sweep.sh
# "182 ok, 0 failing" bug this project already rewrote itself to catch (see
# that script's own zero-file guards) - a check that goes quiet exactly when
# there is nothing to check is worse than no check. Fail loudly here instead.
flag_count=$( printf '%s
' "$flags" | grep -c . )
if [ "$flag_count" -eq 0 ]; then
	echo "xbox-check-nxdk-cc-drift: extracted zero flag-shaped lines from" >&2
	echo "  $NXDK_CC - the extraction pattern matched nothing, not that nxdk-cc" >&2
	echo "  has zero flags. Treat this as a broken check, not a clean pass:" >&2
	echo "  inspect $NXDK_CC by hand and fix the grep/sed pipeline above." >&2
	exit 1
fi

# nxdk-cc writes -I${NXDK_DIR}/lib as one token, no space; xbox-cc.sh writes
# -I "$NXDK_DIR/lib" as two, quoted. Same flag, different shape - collapse
# xbox-cc.sh's -I "..." pair (only -I; -isystem always takes a separate
# argument in both files, so its own space must stay) and strip quotes so a
# literal-substring search can find nxdk-cc's joined form inside it.
#
# Strip comments first (`s/#.*//`), found in review: without this, a flag
# this file's own header prose merely QUOTES while explaining its history
# (this very script's #-fuse-ld=nxdk-link comment a few lines above nxdk-cc's
# actual clang invocation is exactly such a case in xbox-cc.sh) reads as
# "present" whether or not the real exec line still passes it. No comment in
# xbox-cc.sh contains a literal '#' inside a string value, so `#.*` is safe
# to strip wholesale rather than needing to dodge quoted content.
xbox_cc_text=$( sed -e 's/#.*//' \
                     -e 's|\$NXDK_DIR|@NXDK_DIR@|g' \
                     -e 's/-I "/-I"/g' \
                     -e 's/"//g' \
                     "$XBOX_CC" )

# grep -E with a trailing boundary, not grep -qF: found in review, plain
# substring matching means deleting the real -I@NXDK_DIR@/lib line still
# "finds" it as a false match inside -I@NXDK_DIR@/lib/xboxrt/... (and two
# other -I lines under NXDK_INCLUDES), because the shorter flag's text is a
# literal prefix of the longer one's. Escaping regex metacharacters in
# $flag and requiring what follows it to be whitespace or end-of-line
# rejects that false match (the character after "lib" in the longer line is
# "/", not whitespace) while still matching the flag's own genuine
# occurrence, wherever it sits in xbox-cc.sh's argument lists.
missing=0
while IFS= read -r flag; do
	[ -z "$flag" ] && continue
	escaped=$( printf '%s' "$flag" | sed -e 's/[][\.*^$(){}+?|]/\\&/g' )
	if ! printf '%s' "$xbox_cc_text" | grep -Eq -- "$escaped(\$|[[:space:]])"; then
		echo "MISSING from xbox-cc.sh: $flag"
		missing=$(( missing + 1 ))
	fi
done <<< "$flags"

if [ "$missing" -gt 0 ]; then
	echo
	echo "$missing flag(s) that nxdk-cc passes are not found anywhere in" >&2
	echo "tools/xbox-cc.sh's text. nxdk-cc has changed since xbox-cc.sh was" >&2
	echo "last hand-synced to it - review both and update xbox-cc.sh." >&2
	exit 1
fi

echo "xbox-check-nxdk-cc-drift: every nxdk-cc flag is present in xbox-cc.sh."
