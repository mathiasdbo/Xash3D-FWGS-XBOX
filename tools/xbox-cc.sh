#!/usr/bin/env bash
# Compile one engine translation unit for the Xbox target.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-cc.sh <file.c> [extra cc args]
#
# Exists so the environment does not have to be retyped, and so the include
# set stays in one place while X0 grows it. See tools/xbox-env.sh for why the
# shell and NXDK_DIR form matter.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

# MSYS2's MinGW-64 clang. `activate` probes for clang and misbehaves without
# it (see tools/xbox-env.sh), and MSYSTEM alone does not put it on PATH —
# that is the login profile's job, which a non-login shell skips.
export PATH="/mingw64/bin:$PATH"

eval "$("$REPO/deps/nxdk/bin/activate" -s)"

SRC="${1:?usage: xbox-cc.sh <file.c> [extra args]}"
shift

# The engine's own include set, mirroring what wscript hands each module.
INCLUDES=(
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

OUT="${OUT:-/tmp/$(basename "${SRC%.c}").obj}"

exec nxdk-cc -c "$SRC" -o "$OUT" "${INCLUDES[@]}" "$@"
