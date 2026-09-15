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

# One probe per number and a bash regex over the whole output: no grep, tr
# or head, and no dependence on line endings. Launched from MSYS2's own
# bash by tools/xemu-debug.py (Python subprocess), clang's diagnostics
# arrived CRLF and the external-tool grep chain this used to have matched
# nothing, while the same chain on the same text from Git Bash worked -
# measured, twice. Bash's own =~ sees `char (*)[N]` either way.
probe() { # <expression-with-a-value>: prints N from clang's `char (*)[N]`
	local src="$REPO/.sweep/tmp/offsetof_probe.c" out
	printf '#include <stddef.h>\n#include "%s"\nchar (*probe)[%s] = 1;\n' "$HEADER" "$1" > "$src"
	out="$(tools/xbox-cc.sh "$src" -o "$REPO/.sweep/tmp/offsetof_probe.obj" 2>&1)"
	if [[ $out =~ char\ \(\*\)\[([0-9]+)\] ]]; then
		printf '%s\n' "${BASH_REMATCH[1]}"
		return 0
	fi
	echo "xbox-offsetof: no 'char (*)[N]' in clang's output for '$1':" >&2
	printf '%s\n' "$out" >&2
	return 1
}
OFF="$(probe "offsetof($TYPE, $FIELD)")" || exit 1
SZ="$(probe "sizeof(((${TYPE} *)0)->$FIELD)")" || exit 1
echo "$TYPE.$FIELD offset=$OFF size=$SZ"
