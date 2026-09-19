#!/usr/bin/env bash
# xbox-offsetof-cxx.sh - offsetof()/sizeof() of a C++ struct/class field (or
# sizeof() of a whole type), for the Xbox build's exact flags and ABI,
# without running anything on the Xbox.
#
#   tools/xbox-offsetof-cxx.sh <header> <type> <field> [extra cxx args...]
#     -> offset=... size=...
#   tools/xbox-offsetof-cxx.sh <header> <type> [extra cxx args...]
#     -> sizeof=... (a field argument that starts with '-' is treated as
#     the first extra arg, not a field name - field names never start
#     with '-', compiler flags always do)
#
# Extra args (e.g. -I3rdparty/mainui, -DSTDINT_H='<cstdint>') are forwarded
# verbatim to tools/xbox-cxx.sh, the same way tools/xbox-mainui-sweep.sh's
# own CXX_FLAGS get appended when compiling a real mainui .cpp - a header
# like WindowSystem.h only resolves its own #include "utllinkedlist.h" and
# friends with mainui's include path already on the command line.
#
# The C++ sibling of tools/xbox-offsetof.sh - same probe-and-parse-the-
# diagnostic technique (see that file's own comments for the full
# reasoning), through tools/xbox-cxx.sh instead of tools/xbox-cc.sh so
# template instantiation and this project's real C++ flags (-std=c++17,
# etc.) apply. xbox-offsetof.sh's own probe is C-only and cannot read a
# protected/private member at all - offsetof() on one is a hard compile
# error, not a silent success - which every field this sibling exists for
# is (CWindowStack::active/stack, CUtlLinkedList's internals,
# CMenuItemsHolder::m_iCursor).
#
# The private/protected members are made readable with one preprocessor
# trick, applied only inside this throwaway probe translation unit, never
# the real project headers: #define private/protected public before
# #include-ing the target header. This is safe here because access
# specifiers carry no ABI/layout weight in the object model this project's
# compiler uses (Clang, Itanium C++ ABI) for the non-virtual,
# non-multiply-inherited classes this script is used against - confirmed
# once by probing an already-public field both with and without the
# macro trick and comparing (see tools/test_xbox_offsetof_cxx.sh, if
# present, or do this by hand before trusting a new private field).
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1
HEADER="${1:?usage: xbox-offsetof-cxx.sh <header> <type> [field] [extra cxx args...]}"
TYPE="${2:?usage: xbox-offsetof-cxx.sh <header> <type> [field] [extra cxx args...]}"
shift 2
FIELD=""
if [ $# -gt 0 ] && [[ ${1:0:1} != "-" ]]; then
	FIELD="$1"
	shift
fi
EXTRA=("$@")
mkdir -p "$REPO/.sweep/tmp"

# One probe per number - see xbox-offsetof.sh's own comment on why this is
# a bash regex over clang's diagnostic text, not grep/tr/head.
probe() { # <expression-with-a-value>: prints N from clang's `char (*)[N]`
	local src="$REPO/.sweep/tmp/offsetof_cxx_probe.cpp" out
	{
		printf '#include <cstddef>\n'
		printf '#define private public\n'
		printf '#define protected public\n'
		printf '#include "%s"\n' "$HEADER"
		printf 'char (*probe)[%s] = 1;\n' "$1"
	} > "$src"
	out="$(tools/xbox-cxx.sh "$src" "${EXTRA[@]}" -o "$REPO/.sweep/tmp/offsetof_cxx_probe.obj" 2>&1)"
	if [[ $out =~ char\ \(\*\)\[([0-9]+)\] ]]; then
		printf '%s\n' "${BASH_REMATCH[1]}"
		return 0
	fi
	echo "xbox-offsetof-cxx: no 'char (*)[N]' in clang's output for '$1':" >&2
	printf '%s\n' "$out" >&2
	return 1
}

if [ -z "$FIELD" ]; then
	SZ="$(probe "sizeof($TYPE)")" || exit 1
	echo "$TYPE sizeof=$SZ"
else
	OFF="$(probe "offsetof($TYPE, $FIELD)")" || exit 1
	SZ="$(probe "sizeof(((${TYPE} *)0)->$FIELD)")" || exit 1
	echo "$TYPE.$FIELD offset=$OFF size=$SZ"
fi
