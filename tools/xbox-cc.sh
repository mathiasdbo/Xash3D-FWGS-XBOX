#!/usr/bin/env bash
# Compile one engine translation unit for the Xbox target.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-cc.sh <file.c> [extra cc args]
#
# See tools/xbox-env.sh for why the shell and NXDK_DIR form matter.
#
# This does NOT call deps/nxdk/bin/nxdk-cc. That wrapper appends "$@" after its
# own -I flags, so nxdk's include directories are always searched first and
# <unistd.h> resolves to nxdk's empty licence stub instead of
# compat/xbox/unistd.h. The flags below are nxdk-cc's, reordered so ours win.
#
# Keep this in sync with deps/nxdk/bin/nxdk-cc if nxdk is ever updated. It is
# a short, stable script; the divergence is the include ORDER, nothing else.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

# MSYS2's MinGW-64 clang. MSYSTEM alone does not put it on PATH — that is the
# login profile's job, which a non-login shell skips.
export PATH="/mingw64/bin:$PATH"

eval "$("$REPO/deps/nxdk/bin/activate" -s)"

SRC="${1:?usage: xbox-cc.sh <file.c> [extra args]}"
shift

# Ours first. compat/xbox must precede nxdk's own directories.
OUR_INCLUDES=(
	-I "$REPO/compat/xbox"
	-I "$REPO/public"
	-I "$REPO/common"
	-I "$REPO/engine"
	-I "$REPO/engine/common"
	-I "$REPO/engine/server"
	-I "$REPO/engine/client"
	-I "$REPO/filesystem"
	-I "$REPO/pm_shared"
	-I "$REPO/3rdparty/library_suffix/include"
)

# nxdk-cc's own set, verbatim but after ours.
NXDK_INCLUDES=(
	-I "$NXDK_DIR/lib"
	-I "$NXDK_DIR/lib/xboxrt/libc_extensions"
	-isystem "$NXDK_DIR/lib/pdclib/include"
	-I "$NXDK_DIR/lib/pdclib/platform/xbox/include"
	-I "$NXDK_DIR/lib/winapi"
	-I "$NXDK_DIR/lib/xboxrt/vcruntime"
)

OUT="${OUT:-/tmp/$(basename "${SRC%.c}").obj}"

exec clang \
	-target i386-pc-win32 \
	-march=pentium3 \
	-ffreestanding \
	-nostdlib \
	-fno-builtin \
	-Wno-builtin-macro-redefined \
	-DNXDK \
	-D__STDC__=1 \
	-U__STDC_NO_THREADS__ \
	"${OUR_INCLUDES[@]}" \
	"${NXDK_INCLUDES[@]}" \
	-c "$SRC" -o "$OUT" "$@"
