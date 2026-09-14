#!/usr/bin/env bash
# Assert the main-thread stack size actually recorded in a linked PE or a
# converted XBE - read-only, no toolchain involved.
#
#   tools/xbox-check-stack.sh <file.exe|file.xbe> <expected-bytes>
#
# Exit 0 only when the header field equals <expected-bytes>. Exit 1, with
# the real value printed, on a mismatch; exit 2 when the file is missing,
# truncated, or neither a PE nor an XBE. Read: the value is what the target
# will run on, not what a linker flag asked for.
#
# Why this exists (found in review, divergence #47): every nxdk-link call in
# this fork's link tools passes -stack:1048576 to override nxdk-link's own
# hardcoded -stack:65536, relying on lld taking the LAST -stack: given. That
# ordering, cxbe's propagation of it into the XBE header, or the option
# being ignored outright could each regress silently - the resulting
# binary would still link, still be a valid XBE, and still pass every
# other check here, while restoring the first-frame NET_QueuePacket stack
# overflow the flag exists to prevent. So the field itself is measured,
# every time, on the actual output.
#
# Field locations (both little-endian dwords):
#   PE:  e_lfanew at 0x3c -> "PE\0\0" -> optional header at +24 ->
#        SizeOfStackReserve at optional header +72 (PE32, magic 0x10b)
#   XBE: "XBEH" at 0 -> SizeOfStackCommit at 0x130 (the value nxdk's
#        CreateThread(..., 0, ...) hands every thread, main() included)
set -u

FILE="${1:-}"
EXPECTED="${2:-}"
if [ -z "$FILE" ] || [ -z "$EXPECTED" ]; then
	echo "usage: $0 <file.exe|file.xbe> <expected-bytes>" >&2
	exit 2
fi
if [ ! -f "$FILE" ]; then
	echo "xbox-check-stack: $FILE does not exist" >&2
	exit 2
fi

# Little-endian dword at a byte offset, as decimal. od prints nothing (not an
# error) past EOF, so an empty result is how truncation shows up.
dword() {
	od -An -tu4 -j "$2" -N 4 "$1" 2>/dev/null | tr -d ' \n'
}

MAGIC=$(od -An -c -N 4 "$FILE" 2>/dev/null | tr -d ' \n')
case "$MAGIC" in
	XBEH)
		KIND="XBE SizeOfStackCommit (offset 0x130)"
		VALUE=$(dword "$FILE" 304)
		;;
	MZ*)
		PE_OFF=$(dword "$FILE" 60)
		if [ -z "$PE_OFF" ]; then
			echo "xbox-check-stack: $FILE is truncated before e_lfanew" >&2
			exit 2
		fi
		PE_MAGIC=$(od -An -c -j "$PE_OFF" -N 4 "$FILE" 2>/dev/null | tr -d ' \n')
		if [ "$PE_MAGIC" != 'PE\0\0' ]; then
			echo "xbox-check-stack: $FILE has no PE signature at e_lfanew=$PE_OFF (got '$PE_MAGIC')" >&2
			exit 2
		fi
		OPT=$((PE_OFF + 24))
		OPT_MAGIC=$(od -An -tu2 -j "$OPT" -N 2 "$FILE" 2>/dev/null | tr -d ' \n')
		if [ "$OPT_MAGIC" != "267" ]; then   # 0x10b = PE32; PE32+ would put the field elsewhere
			echo "xbox-check-stack: $FILE optional header magic is $OPT_MAGIC, not PE32 (267)" >&2
			exit 2
		fi
		KIND="PE SizeOfStackReserve"
		VALUE=$(dword "$FILE" $((OPT + 72)))
		;;
	*)
		echo "xbox-check-stack: $FILE is neither an XBE nor a PE (magic '$MAGIC')" >&2
		exit 2
		;;
esac

if [ -z "$VALUE" ]; then
	echo "xbox-check-stack: $FILE is truncated before its stack-size field" >&2
	exit 2
fi
if [ "$VALUE" -ne "$EXPECTED" ]; then
	echo "xbox-check-stack: $FILE $KIND is $VALUE bytes, expected $EXPECTED" >&2
	echo "  - the -stack: override did not reach this binary; see divergence #47." >&2
	exit 1
fi
echo "xbox-check-stack: $FILE $KIND = $VALUE bytes (ok)"
