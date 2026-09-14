#!/usr/bin/env bash
# Compile every C file under a directory for the Xbox target and summarise.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-sweep.sh engine
#
# Prints a per-file FAIL line, then the distinct error messages ranked by how
# many files hit them — which is what says where the work is, rather than which
# file happens to come first alphabetically.
#
# Success is the COMPILER'S EXIT STATUS, not a grep for "error:". An earlier
# version of this script grepped, and reported "182 ok, 0 failing" for an
# engine directory where the very first file produces twenty errors: the
# compiler had failed for a reason whose message did not contain that word, and
# the script called it a pass. A sweep that cannot fail is worse than no sweep,
# because it gets believed.
#
# WHAT THIS DOES NOT CATCH, found in review 2026-09-07: every invocation ends
# in `-c` (see tools/xbox-cc.sh), so clang never runs a linker and a call to a
# DECLARED-but-never-DEFINED function is invisible here - legal C at the TU
# level, and a hard failure only at final link. That is exactly the shape of
# bug X0's log-channel work found and fixed (Wcon_WinPrint, divergence #7c):
# declared in platform.h, defined only in a file that itself does not compile
# for this target. "N of M compile" has never meant "N of M would link", and
# no version of this tool checks the second, stronger claim - there is no
# Xbox link step yet to check it with (that is X1's job). Read a clean sweep
# as "no syntax or missing-declaration errors", not as "this would boot".
set -u

# Found in review, 2026-09-07: the previous fix to this file's own exit
# code (make the fall-through explicit rather than accidental) was itself
# incomplete - explicit is not the same as machine-checkable. "$fail -gt 0"
# being the expected, tracked X0 baseline is a fact a human reading this
# file's comments knows; nothing distinguished that tracked baseline from
# a real regression for any caller checking only the exit code, whether
# that caller is a human running `&&` or a future CI gate. --expect-fail
# closes that gap without hardcoding a project-specific number into this
# general-purpose script: the caller supplies the currently-tracked count
# (CHANGELOG.md's own "N ok, M failing" line is the source of truth), and
# this script treats ANY difference - worse OR better - as something that
# needs a conscious decision, not silent drift in either direction. Optional
# and off by default, so ad-hoc single-directory sweeps with no meaningful
# baseline are unaffected.
EXPECT_FAIL=""
ARGS=()
while [ $# -gt 0 ]; do
	case "$1" in
		--expect-fail)
			EXPECT_FAIL="$2"
			shift 2
			;;
		*)
			ARGS+=("$1")
			shift
			;;
	esac
done
set -- "${ARGS[@]}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

DIR="${1:?usage: xbox-sweep.sh [--expect-fail N] <dir> [more dirs...]}"

SCRATCH="${SCRATCH:-$REPO/.sweep}"
if ! mkdir -p "$SCRATCH"; then
	echo "xbox-sweep: cannot create scratch dir $SCRATCH" >&2
	exit 1
fi

LOG="$SCRATCH/errors.txt"
if ! : > "$LOG"; then
	echo "xbox-sweep: cannot write $LOG" >&2
	exit 1
fi

# Failures the "error:" grep cannot explain go here VERBATIM.
RAW="$SCRATCH/unexplained.txt"
if ! : > "$RAW"; then
	echo "xbox-sweep: cannot write $RAW" >&2
	exit 1
fi

# One line per file, "OK <src> <obj>" or "FAIL <src> <obj>", plus a header
# naming the compiler driver whose flags produced the OK objects. This is
# what tools/xbox-link-check.sh links against: found in Codex review (X5)
# that the link check discovered whatever *.obj happened to sit in
# $SCRATCH and could report "0 duplicate, 0 undefined" on objects an
# earlier sweep left behind - stale against their source, against a
# define added to tools/xbox-cc.sh since, or surviving a source that now
# fails to compile (the object was written in place and never removed).
# Per-directory sweeps append to a manifest a full sweep started, so the
# canonical five-directory run and a later single-directory refresh both
# leave one consistent list. Written through one persistent descriptor
# (tools/xbox-game-isolate.sh's measured reason: reopening for append per
# line lost entries on this host).
MANIFEST="$SCRATCH/sweep-manifest.txt"
if [ "${XBOX_SWEEP_APPEND_MANIFEST:-0}" != "1" ]; then
	if ! : > "$MANIFEST"; then
		echo "xbox-sweep: cannot write $MANIFEST" >&2
		exit 1
	fi
	printf 'DRIVER tools/xbox-cc.sh %s\n' "$(cksum "$REPO/tools/xbox-cc.sh" | awk '{print $1}')" >> "$MANIFEST"
fi
exec 5>> "$MANIFEST"

ok=0
fail=0
mute=0

for d in "$@"; do
	d_ok=0
	d_fail=0

	while IFS= read -r f; do
		obj="$SCRATCH/$( echo "$f" | tr '/' '_' | sed -E 's/\.(c|cpp)$/.obj/' )"
		# Every .cpp file in this tree, unconditionally: nxdk-link has no C++
		# exception-handling runtime at all (no deps/nxdk/lib/*.lib defines
		# __CxxFrameHandler3/__std_terminate - grep-checked). A file that
		# compiles clean without these flags but needs them at FINAL LINK
		# (compiler-generated cleanup code for a non-trivial destructor, not
		# necessarily an explicit try/catch in its own source) would report
		# "ok" here and only fail much later, in a full link - found exactly
		# this way with filesystem/VFileSystem009.cpp, X2 W5's follow-up.
		cxxflags=()
		case "$f" in
			*.cpp) cxxflags=( -fno-exceptions -fno-rtti ) ;;
		esac
		# -o as an argument, not via the OUT env var: xbox-cc.sh appends "$@"
		# after its own -o and clang takes the last one, whereas the env var was
		# not reaching the child and every object silently landed on the same
		# default path, overwriting each other.
		out=$( "$REPO/tools/xbox-cc.sh" "$f" "${cxxflags[@]}" -o "$obj" 2>&1 )
		status=$?

		if [ "$status" -eq 0 ]; then
			ok=$(( ok + 1 ))
			d_ok=$(( d_ok + 1 ))
			printf 'OK %s %s\n' "$f" "$obj" >&5
		else
			# A source that now fails must not leave its previous, successful
			# object behind for the link to pick up (see the manifest comment).
			rm -f "$obj"
			printf 'FAIL %s %s\n' "$f" "$obj" >&5
			# Checked before the generic "error:" count, not folded into it:
			# found in review, xbox-cc.sh's own preflight (the library_suffix
			# auto-patch step) can fail with text that itself contains
			# "error:" (git apply's "error: patch failed: ..."), which would
			# otherwise be misclassified as an ordinary per-file compile
			# error - and would repeat identically for every remaining file
			# in this sweep, since the preflight runs before clang ever sees
			# a single one of them. Fail the whole sweep here rather than
			# grinding through 200+ files that cannot possibly do anything
			# but reproduce the same preflight failure.
			if printf '%s' "$out" | grep -q '^XBOX_CC_PREFLIGHT_FAILURE:'; then
				echo
				echo "*** xbox-cc.sh's own preflight failed before reaching clang. ***"
				echo "*** Every remaining file would fail identically - stopping now. ***"
				echo
				printf '%s\n' "$out"
				exit 2
			fi

			fail=$(( fail + 1 ))
			d_fail=$(( d_fail + 1 ))
			n=$( printf '%s' "$out" | grep -c 'error:' )

			if [ "$n" -eq 0 ]; then
				# The compiler failed and said nothing this script recognises. That
				# is what a BROKEN TOOLCHAIN looks like: set -u in xbox-cc.sh aborts
				# on an unset NXDK_DIR, and a missing clang exits 127 - neither
				# prints "error:". An earlier version discarded $out here and
				# reported "FAIL <file>   0 errors" with an empty summary and a
				# zero-byte log, which is indistinguishable from a real sweep and
				# would have made every one of 182 files look like a compat gap.
				mute=$(( mute + 1 ))
				printf '  FAIL %-52s exit %-3s NO DIAGNOSTIC\n' "$f" "$status"
				{
					printf '=== %s: exit %s, no "error:" line ===\n' "$f" "$status"
					printf '%s\n' "$out"
				} >> "$RAW"
			else
				printf '  FAIL %-52s %3s errors\n' "$f" "$n"
				printf '%s\n' "$out" | grep 'error:' >> "$LOG"
			fi
		fi
	done < <( find "$d" \( -name '*.c' -o -name '*.cpp' \) | sort )

	# Found in review, 2026-09-07: the guard below this loop only ever looked
	# at the AGGREGATE ok+fail total across every directory on the command
	# line. The canonical invocation passes five directories at once (engine
	# filesystem public common compat); a single bad one among them - typo'd,
	# or an uninitialised submodule - contributes 0/0 here and is silently
	# absorbed into a large, legitimate-looking nonzero total from the other
	# four. The run then reports what looks like a full, successful sweep
	# while an entire directory's files were never compiled. Check
	# per-directory, right here, before moving to the next one, so which
	# directory came up empty is also known rather than inferred after the
	# fact.
	#
	# Zero .c FILES is not by itself the fault: common/ is one of the five
	# directories the canonical invocation passes, and it is genuinely
	# header-only - found while testing this exact fix against the real
	# five-directory command, not a hypothetical one. What actually means
	# "typo, or a submodule never initialised" is a directory with NO FILES
	# AT ALL, .c or otherwise - so check that, and only warn (not fail) on
	# the header-only case.
	if [ "$(( d_ok + d_fail ))" -eq 0 ]; then
		if [ -z "$( find "$d" -type f -print -quit )" ]; then
			echo "xbox-sweep: directory contains no files at all: $d" >&2
			echo "  Typo'd directory, or a submodule not yet initialised" \
				"(git submodule update --init)?" >&2
			exit 2
		else
			echo "xbox-sweep: note - $d has files but zero .c files (header-only" >&2
			echo "  directory - expected, not a failure)." >&2
		fi
	fi
done

# What's left for THIS check, now that the per-directory check above
# handles the real failure (a directory with no files at all): every
# directory passed on the command line was individually confirmed to have
# at least one file, so the only way ok+fail is still 0 here is that every
# directory given was legitimately header-only (running this against
# `common` alone, say). That is not a bug - there was nothing to compile -
# so this is informational, not fatal. ${1:?...} above already rejects a
# fully empty "$@" separately, before the for loop is ever reached.
if [ "$(( ok + fail ))" -eq 0 ]; then
	echo "xbox-sweep: no .c files in any of: $* (header-only input, nothing to compile)"
	exit 0
fi

echo
echo "compile: $ok ok, $fail failing   (scratch: $SCRATCH)"
echo

if [ "$mute" -gt 0 ]; then
	echo "*** $mute of the $fail failures produced NO compiler diagnostic. ***"
	echo
	echo "Treat this as a broken environment until proven otherwise, not as a"
	echo "count of missing APIs. First unexplained failure, verbatim:"
	echo
	head -20 "$RAW" | sed 's/^/    /'
	echo
	echo "full unexplained log: $RAW"
	echo

	# If NOTHING compiled and nothing explained why, the sweep measured the
	# environment, not the tree. Say so with an exit status a caller can test.
	#
	# mute -eq fail, not ok -eq 0: found in review, 2026-09-07. "Nothing
	# explained why" means every failure is unexplained, not merely that
	# zero files fully succeeded - the two are different questions. A mass
	# real regression (fail=220, every one with a genuine clang "error:",
	# so ok=0) that happens to ALSO include one unrelated, truly
	# undiagnosed hiccup on a single file (mute=1) satisfied the old
	# `ok -eq 0` check and exited here before ever reaching the "distinct
	# errors, by frequency" ranking below - misreporting a real, already-
	# diagnosed 219-file regression as "check your toolchain" and hiding
	# the one summary that would have shown it for what it was. A
	# genuinely broken toolchain (NXDK_DIR unset, say) mutes every file
	# identically, so mute -eq fail already covers the case this check
	# exists for; it just also correctly declines to fire when most of the
	# failures have a real diagnostic sitting in $LOG.
	if [ "$mute" -eq "$fail" ]; then
		echo "compile: $fail failing and no diagnostics - this is an environment fault." >&2
		echo "Run tools/xbox-env.sh and check NXDK_DIR before believing any number." >&2
		exit 2
	fi
fi

if [ "$fail" -gt "$mute" ]; then
	echo "distinct errors, by frequency:"
	# Drop the file:line prefix and the -Wflag suffix so the same cause groups
	# together. Identifiers are KEPT: an earlier version blanked them, which
	# turned "unknown type name 'convar_t'; did you mean 'cvar_t'?" into
	# "unknown type name 'X'; did you mean 'X'?" and hid the answer.
	sed 's/^.*error: //' "$LOG" | sed 's/ \[-W.*\]$//' \
		| sort | uniq -c | sort -rn | head -30
	echo
	echo "full log: $LOG"
fi

# Found in review, 2026-09-07, critical: everything above this point only
# ever echoes diagnostics - there is no exit statement anywhere after the
# early-outs above (preflight failure, a genuinely empty directory, the
# all-mute environment-fault case), so on the single most common real
# outcome - some files compile and some do not, exactly what "186 ok, 34
# failing" is - the script falls off the end of the file and exits with
# whatever the last command (an "echo" or "fi") happened to return: 0,
# always, regardless of $fail. `tools/xbox-sweep.sh ...; echo $?` prints 0
# even when real clang "error:" diagnostics were seen on every failing
# file. Nothing in this tree currently reads this exit code (no CI
# workflow, no other script calls this one), so this was not a live outage
# - but it is exactly the shape this project keeps finding and fixing in
# its own tools (see this file's own header: "a sweep that cannot fail is
# worse than no sweep, because it gets believed"), here at the aggregate
# script level rather than the per-file one, and it survived nine review
# rounds on the branch that hardened this exact file twice already.
#
# Made explicit rather than unconditionally nonzero: X0's own baseline is
# 186 of 220 compiling, by design - 26 files this build never selects under
# any Xbox config, 6 in the parked networking group, 2 in platform/posix/ -
# so "$fail -gt 0" is the EXPECTED, tracked outcome right now, not a
# regression by itself. Exiting nonzero on any fail>0 would make every
# sweep this project has run so far report as failed, training whoever
# runs it to ignore or override the exit code - worse than no check.
#
# But "expected right now" is not the same as "unverifiable" - found in
# review, 2026-09-07, second pass: without --expect-fail, this script
# genuinely cannot distinguish its own tracked baseline from a real
# regression by exit status alone, which is exactly what "a sweep that
# cannot fail is worse than no sweep, because it gets believed" (this
# file's own header) describes, one level up from the per-file check that
# line was originally about.
if [ -n "$EXPECT_FAIL" ]; then
	if [ "$fail" -eq "$EXPECT_FAIL" ]; then
		echo "baseline: $fail failing matches --expect-fail $EXPECT_FAIL."
		exit 0
	elif [ "$fail" -gt "$EXPECT_FAIL" ]; then
		echo "xbox-sweep: REGRESSION - $fail failing, expected $EXPECT_FAIL." >&2
		echo "  $(( fail - EXPECT_FAIL )) more file(s) fail than the tracked baseline." >&2
		exit 1
	else
		echo "xbox-sweep: IMPROVEMENT - $fail failing, expected $EXPECT_FAIL." >&2
		echo "  $(( EXPECT_FAIL - fail )) fewer file(s) fail than the tracked baseline -" >&2
		echo "  update the tracked count (CHANGELOG.md) and the --expect-fail caller" >&2
		echo "  passes, so this does not silently drift the next time it changes." >&2
		exit 1
	fi
fi

exit 0
