#!/usr/bin/env bash
# xbox-offsetof.sh - offsetof()/sizeof() of an engine struct field, for the
# Xbox build's exact flags and ABI, without running anything on the Xbox.
#
#   tools/xbox-offsetof.sh <header> <type> <field>
#   tools/xbox-offsetof.sh common.h host_parm_t framecount
#   -> host_parm_t.framecount offset=... size=...
#
# How: a one-line probe TU compiled through tools/xbox-cc.sh (so every
# -D/-I the real build uses applies - XASH_LOW_MEMORY changes struct sizes,
# for one) deliberately fails to compile with the numbers in the error
# text: `char (*p)[offsetof(T, f)] = 1;` makes clang report the type
# `char (*)[N]`. Nothing is guessed from source layout by hand - the
# host_parm_t offsets tools/xemu-debug.py reads live come from here, the
# same way X7.4's nextstate reads were once computed by hand and had to
# be re-derived when a field moved.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1
HEADER="${1:?usage: xbox-offsetof.sh <header> <type> <field>}"
TYPE="${2:?usage: xbox-offsetof.sh <header> <type> <field>}"
FIELD="${3:?usage: xbox-offsetof.sh <header> <type> <field>}"
mkdir -p "$REPO/.sweep/tmp"
PROBE="$REPO/.sweep/tmp/offsetof_probe.c"
cat > "$PROBE" <<EOF
#include <stddef.h>
#include "$HEADER"
char (*offset_probe)[offsetof($TYPE, $FIELD)] = 1;
char (*size_probe)[sizeof(((${TYPE} *)0)->$FIELD)] = 1;
EOF
OUT="$(tools/xbox-cc.sh "$PROBE" -o "$REPO/.sweep/tmp/offsetof_probe.obj" 2>&1)"
# clang prints the error line (carrying the type) BEFORE the echoed source
# line that names the probe, so look one line back from the probe's name.
OFF="$(printf '%s\n' "$OUT" | grep -B1 "(\*offset_probe)" | grep -o "char (\*)\[[0-9]*\]" | head -1 | grep -o "[0-9]*")"
SZ="$(printf '%s\n' "$OUT" | grep -B1 "(\*size_probe)" | grep -o "char (\*)\[[0-9]*\]" | head -1 | grep -o "[0-9]*")"
if [ -z "$OFF" ] || [ -z "$SZ" ]; then
	echo "xbox-offsetof: could not read the offset from clang's output:" >&2
	printf '%s\n' "$OUT" | head -20 >&2
	exit 1
fi
echo "$TYPE.$FIELD offset=$OFF size=$SZ"
