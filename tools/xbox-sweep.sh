#!/usr/bin/env bash
# Compile every C file under a directory for the Xbox target and summarise.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-sweep.sh engine
#
# Prints a per-file OK/FAIL line, then the distinct error messages ranked by
# how many files hit them — which is what says where the work actually is.
# Objects go to a scratch directory; nothing in the tree is written.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

DIR="${1:?usage: xbox-sweep.sh <dir> [more dirs...]}"
SCRATCH="${SCRATCH:-/tmp/r3d-sweep}"
mkdir -p "$SCRATCH"
LOG="$SCRATCH/errors.txt"
: > "$LOG"

ok=0
fail=0

for d in "$@"; do
	while IFS= read -r f; do
		out=$( OUT="$SCRATCH/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.obj/')" \
			"$REPO/tools/xbox-cc.sh" "$f" 2>&1 )
		n=$( printf '%s' "$out" | grep -c 'error:' )

		if [ "$n" -eq 0 ]; then
			ok=$(( ok + 1 ))
		else
			fail=$(( fail + 1 ))
			printf '  FAIL %-52s %s\n' "$f" "$n"
			printf '%s\n' "$out" | grep 'error:' >> "$LOG"
		fi
	done < <( find "$d" -name '*.c' | sort )
done

echo
echo "compile: $ok ok, $fail failing"
echo

if [ "$fail" -gt 0 ]; then
	echo "distinct errors, by frequency:"
	# Strip file:line and the -Wflag suffix so the same cause groups together.
	sed -E 's/^[^ ]*error: //; s/ \[-W[^]]*\]$//' "$LOG" \
		| sed -E "s/'[^']*'/'X'/g" \
		| sort | uniq -c | sort -rn | head -30
	echo
	echo "full log: $LOG"
fi
