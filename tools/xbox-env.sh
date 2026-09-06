#!/usr/bin/env sh
# Environment for building nxdk and the Xbox target on a Windows host.
#
#   . tools/xbox-env.sh        (source it, do not execute it)
#
# Every line here exists because getting it wrong produces a *silent* failure
# rather than an error. Read the notes before changing anything.

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)

# --- NXDK_DIR must be a Windows-form path ------------------------------------
#
# THE ONE THAT COSTS AN HOUR IF YOU GET IT WRONG.
#
# deps/nxdk/bin/nxdk-cc is a POSIX shell script that passes
# "-isystem ${NXDK_DIR}/lib/pdclib/include" and five more include paths to a
# NATIVE clang.exe. If NXDK_DIR holds an MSYS-form path (/g/Proyectos/...),
# clang cannot resolve those directories and **silently drops them** — there
# is no warning, because clang omits non-existent -I paths without comment.
#
# The compile then falls through to the Visual Studio and Windows SDK headers
# that clang auto-detects from the i386-pc-win32 triple, and fails much later
# with something that looks unrelated:
#
#   pdclib/functions/ctype/isalpha.c:15:14:
#     error: use of undeclared identifier '_PDCLIB_lc_ctype'
#
# because it compiled against C:\...\Windows Kits\10\...\ucrt\ctype.h instead
# of pdclib's. Confirm the search path with:
#
#   nxdk-cc -v -c <file> 2>&1 | sed -n '/search starts here/,/End of search/p'
#
# pdclib, xboxrt and winapi must all appear there. If they do not, this
# variable is wrong.
NXDK_DIR=$(cd "$REPO_ROOT/deps/nxdk" && pwd -W 2>/dev/null || echo "$REPO_ROOT/deps/nxdk")
export NXDK_DIR

# --- Toolchain ---------------------------------------------------------------
# clang and the LLVM binutils: nxdk needs clang, lld-link, llvm-lib, llvm-ar.
# llvm-objcopy is needed too, but note it rejects -G/--localize-symbol on COFF;
# --redefine-sym is the one symbol option that works. See docs/r3d/x0-probe-results.md §4.
LLVM_BIN="/c/Program Files/LLVM/bin"

# GNU make. Git Bash does not ship one; MSYS2 or the nxdk docs' toolchain does.
MAKE_BIN="/c/msys64/usr/bin"

# nxdk's own wrappers (nxdk-cc, nxdk-cxx, nxdk-link, cxbe, extract-xiso).
NXDK_BIN="$REPO_ROOT/deps/nxdk/bin"

PATH="$LLVM_BIN:$NXDK_BIN:$MAKE_BIN:$PATH"
export PATH

# --- Check -------------------------------------------------------------------
for tool in clang make nxdk-cc nxdk-link; do
	command -v "$tool" >/dev/null 2>&1 || echo "xbox-env: MISSING $tool"
done

echo "xbox-env: NXDK_DIR=$NXDK_DIR"
echo "xbox-env: clang $(clang --version 2>/dev/null | head -1 | sed 's/.*version //;s/ .*//')"

# --- Building nxdk -----------------------------------------------------------
#
#   cd deps/nxdk && make NXDK_ONLY=y -j8
#
# NXDK_ONLY=y implies NXDK_CXX=y and NXDK_SDL=y (Makefile:63-67), so it builds
# every library including SDL2 and libcxx. That is what we want — mainui is
# C++ and the platform layer is SDL2 — but it is also why the build pulls every
# library's include flags into every translation unit.
