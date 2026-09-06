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
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

DIR="${1:?usage: xbox-sweep.sh <dir> [more dirs...]}"

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

ok=0
fail=0

for d in "$@"; do
	while IFS= read -r f; do
		obj="$SCRATCH/$( echo "$f" | tr '/' '_' | sed 's/\.c$/.obj/' )"
		# -o as an argument, not via the OUT env var: xbox-cc.sh appends "$@"
		# after its own -o and clang takes the last one, whereas the env var was
		# not reaching the child and every object silently landed on the same
		# default path, overwriting each other.
		out=$( "$REPO/tools/xbox-cc.sh" "$f" -o "$obj" 2>&1 )
		status=$?

		if [ "$status" -eq 0 ]; then
			ok=$(( ok + 1 ))
		else
			fail=$(( fail + 1 ))
			n=$( printf '%s' "$out" | grep -c 'error:' )
			printf '  FAIL %-52s %3s errors\n' "$f" "$n"
			printf '%s\n' "$out" | grep 'error:' >> "$LOG"
		fi
	done < <( find "$d" -name '*.c' | sort )
done

echo
echo "compile: $ok ok, $fail failing   (scratch: $SCRATCH)"
echo

if [ "$fail" -gt 0 ]; then
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
