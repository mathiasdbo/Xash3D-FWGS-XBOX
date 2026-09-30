#!/usr/bin/env bash
# Compile one C++ translation unit for the Xbox target.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-cxx.sh <file.cpp> [extra cc args]
#
# The C++ analogue of tools/xbox-cc.sh - same reasoning throughout, see that
# file's own comments for the parts duplicated here rather than re-explained.
#
# Not deps/nxdk/bin/nxdk-cxx, for the identical reason xbox-cc.sh does not
# call nxdk-cc: nxdk-cxx appends "$@" after its own -I flags, so
# compat/xbox's headers would lose to nxdk's when both declare the same
# name. The flags below are nxdk-cxx's (deps/nxdk/bin/nxdk-cxx), reordered
# so ours win, plus this project's own -include prelude and submodule-patch
# preflight (shared verbatim with xbox-cc.sh, since the same two patches -
# library_suffix's XASH_XBOX subplatform, opusfile's stream.c - are
# reachable from a C++ translation unit exactly as they are from a C one).
#
# Scope: this script only ever compiles (-c, like xbox-cc.sh) - it does not
# attempt or prove a real link. Corrected 2026-09-09, found in review: an
# earlier version of this comment claimed deps/nxdk/lib/libcxx has no
# prebuilt library at all and that operator new/delete are undefined
# anywhere in the tree - false. deps/nxdk/lib/libc++.lib is a real,
# prebuilt library (named literally "libc++.lib", not "libcxx.lib" - the
# search that produced the wrong claim missed it on the name alone), and
# deps/nxdk/share/toolchain-nxdk.cmake:37 already links every C++ target
# against it. It genuinely defines operator new/delete (deps/nxdk/lib/
# libcxx/src/new.cpp, weak symbols under `defined(NXDK)`) - verified by a
# real nxdk-link against a translation unit that calls new/delete plus
# libc++.lib and the base library set: 0 errors, only a benign lld
# .edata-merge warning. A caller linking real C++ code needs to add
# deps/nxdk/lib/libc++.lib to its library list - this script's own -c-only
# scope means it was never the thing missing that library in the first
# place.
#
# -std=c++17: found 2026-09-12 building X7.4's real hl SDK sweep - without
# an explicit -std=, this exact target/flag combination's real default is
# C++14 (measured directly: `clang++ ... -dM -E -x c++ /dev/null | grep
# __cplusplus` prints 201402L, not the C++17 201703L one might assume from
# clang 21 being recent), which is not a cosmetic difference: class
# template argument deduction (deps/hlsdk/dlls/player.cpp's
# `CallOnDestroy resetIsSpawning{...}`) is unavailable, and std::clamp
# (also player.cpp) isn't declared at all - both hard errors, not
# warnings. The HL SDK's own build (deps/hlsdk/CMakeLists.txt's
# `set_common_properties`) already requires `target_compile_features(...
# cxx_std_17)`; this was this project's compile route silently not
# honouring it. Fixed 4 of 5 real server-sweep compile failures outright
# (player.cpp, game.cpp, nodes.cpp, client.cpp - all 0 errors before this
# were CTAD/std::clamp/other C++17-feature symptoms of the same root
# cause); re-verified the existing 3rdparty/mainui sweep
# (tools/xbox-mainui-sweep.sh, 82/82 clean before this change) is
# unaffected - C++17 is a compatible superset of C++14 for real, existing
# code here, not just in theory.
#
# Two real deps/hlsdk/external/SDL2 conflicts, found 2026-09-12 compiling
# the real hl CLIENT SDK for the first time (tools/xbox-game-sweep.sh hl
# client), both root-caused to the fact that this target/flag combination
# defines _MSC_VER (verified directly: a `#ifdef _MSC_VER / #error` probe
# fires), which is enough for deps/hlsdk/external/SDL2's own
# `#if defined(_MSC_VER)`-guarded compatibility branches to activate even
# though nothing here is real MSVC. Blanket `-U_MSC_VER` was tried and
# rejected - nxdk's own libcxx/pdclib headers rely on `_MSC_VER` being
# defined for their own real branches (verified: undefining it globally
# breaks deps/nxdk/lib/libcxx/include/string.h's own strstr overload
# resolution, a real, working, unrelated codepath).
#
# Both were first fixed HERE, as global compiler flags
# (`-D_UINTPTR_T_DEFINED`/`-D__PRFCHWINTRIN_H`) - then corrected in Codex
# review (round 1 for the first, round 2 for the second, once it noticed
# the first hadn't been held to the same standard): a flag predefining a
# guard macro affects EVERY translation unit project-wide, not just the
# one vendored block it was meant for, which is wrong for a guard name
# clang's own real `<intrin.h>` also uses (`__PRFCHWINTRIN_H`) and risky
# in general even for an SDL2-specific one. Fixed at the source instead,
# on `deps/hlsdk`'s `resonance3d` branch (divergence #39): three vendored
# SDL2 headers (`SDL_config_windows.h`'s `uintptr_t` redeclaration -
# `unsigned int`, conflicting with pdclib's own already-correct
# `unsigned long`, found as a real "typedef redefinition with different
# types" hard error; `SDL_endian.h`/`SDL_cpuinfo.h`'s `_m_prefetch`
# pseudo-header, colliding with clang's own real builtin) each gained a
# narrow `&& !defined(NXDK)` on the SPECIFIC guarded block responsible,
# not the whole surrounding `_MSC_VER` branch (tried and rejected for the
# uintptr_t case: skipping the WHOLE block via `-D_STDINT_H_` fixed this
# but broke other real setup the same block also does). Confirmed correct
# rather than just convenient for `SDL_cpuinfo.h` specifically: its same
# block also unconditionally `#define`s `__SSE2__`/`__SSE3__`, which the
# real Xbox CPU (Pentium III class) never had - clang's own
# `-march=pentium3` already predefines the accurate feature set
# (`__MMX__`/`__SSE__` only, verified directly), so excluding NXDK here
# is not a workaround, it is the correct answer either way. Verified:
# cl_dll/cdll_int.cpp's real error count dropped from the SAL-macro gap
# (compat/xbox/sal.h), the uintptr_t conflict, and the _m_prefetch
# conflict combined, down to just the separately-tracked, already-known
# `<memory_resource>` want; re-verified the real hl server sweep (107/107)
# and 3rdparty/mainui (82/82) are both unaffected, and the full client
# sweep result is unchanged (70/85 to 77/85) between the flag-based and
# source-based fixes.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1
# The optimisation level (XBOX_OPT, default -O2): see tools/xbox-opt.sh.
. "$REPO/tools/xbox-opt.sh"

# Same two auto-apply preflights as xbox-cc.sh, verbatim - see that file's
# own comments for the full reasoning. Duplicated rather than sourced: this
# project has no shared-sourcing convention between tools/xbox-*.sh scripts
# yet, and inventing one for two copy-pasted blocks is not worth it until a
# third script needs the same preflight.
BUILD_H="$REPO/3rdparty/library_suffix/include/build.h"
if [ ! -f "$BUILD_H" ]; then
	echo "XBOX_CC_PREFLIGHT_FAILURE: 3rdparty/library_suffix/include/build.h does not exist"
	echo "  - the submodule is not initialised. Run:" >&2
	echo "  git submodule update --init 3rdparty/library_suffix" >&2
	exit 1
elif ! grep -q "define XASH_XBOX 1" "$BUILD_H"; then
	if ! git -c core.autocrlf=false -C "$REPO/3rdparty/library_suffix" apply \
		"$REPO/patches/library_suffix-xbox-subplatform.patch" 2>&1; then
		echo "XBOX_CC_PREFLIGHT_FAILURE: could not apply patches/library_suffix-xbox-subplatform.patch"
		echo "  to 3rdparty/library_suffix/include/build.h - check it still applies" >&2
		echo "  cleanly against the submodule's pinned commit." >&2
		exit 1
	fi
fi

STREAM_C="$REPO/3rdparty/opusfile/opusfile/src/stream.c"
if [ ! -f "$STREAM_C" ]; then
	echo "XBOX_CC_PREFLIGHT_FAILURE: 3rdparty/opusfile/opusfile/src/stream.c does not exist"
	echo "  - the submodule is not initialised. Run:" >&2
	echo "  git submodule update --init 3rdparty/opusfile/opusfile" >&2
	exit 1
elif ! grep -q "nxdk's winapi has no GetFileType" "$STREAM_C"; then
	if ! git -c core.autocrlf=false -C "$REPO/3rdparty/opusfile/opusfile" apply \
		"$REPO/patches/opusfile-xbox-stream.patch" 2>&1; then
		echo "XBOX_CC_PREFLIGHT_FAILURE: could not apply patches/opusfile-xbox-stream.patch"
		echo "  to 3rdparty/opusfile/opusfile/src/stream.c - check it still applies" >&2
		echo "  cleanly against the submodule's pinned commit." >&2
		exit 1
	fi
fi

# Found in review, 2026-09-09: an earlier version of this script also ran
# a miniutl/winlite.h preflight here (mainui's own nested-submodule patch).
# That made compiling ANY C++ translation unit - including unrelated
# future work like the HL SDK client/server this script's own docstring
# says it exists to serve - fail whenever 3rdparty/mainui/miniutl was not
# initialised, even though that submodule has nothing to do with
# non-mainui code. Removed: this script stays generic, exactly what its
# own docstring claims (compile one C++ translation unit, given the right
# flags). tools/xbox-mainui-sweep.sh - the only caller that actually needs
# the miniutl patch - carries that preflight itself now, scoped to where
# it belongs.

export PATH="/c/msys64/mingw64/bin:$PATH"

# ${NXDK_DIR:-...}: a caller may point at another checkout's built nxdk
# (nxdk's own activate keeps a pre-set NXDK_DIR, so this only changes
# WHICH activate is read, not what it exports). A git worktree has no
# built nxdk of its own; see tools/xbox-link-check.sh's NXDK_DIR line.
# Unset, this is the checkout's own deps/nxdk, unchanged.
# A checkout may carry its nxdk location in .sweep/nxdk-dir (the agentic
# loop's setup stage writes it into each worktree). docs/r3d/lessons.md's
# entry on /c/msys64/usr/bin/bash shows an exported variable can be dropped
# at that shell boundary with no error, so the checkout itself answers
# when the environment does not. Unset and no file: the default below.
[ -n "${NXDK_DIR:-}" ] || NXDK_DIR="$(cat "$REPO/.sweep/nxdk-dir" 2>/dev/null || true)"
eval "$("${NXDK_DIR:-$REPO/deps/nxdk}/bin/activate" -s)"

SRC="${1:?usage: xbox-cxx.sh <file.cpp> [extra args]}"
shift

OUR_INCLUDES=(
	-I "$REPO/compat/xbox"
)

# nxdk-cxx's own set, verbatim but after ours.
NXDK_INCLUDES=(
	-I "$NXDK_DIR/lib/libcxx/include"
	-I "$NXDK_DIR/lib"
	-I "$NXDK_DIR/lib/xboxrt/libc_extensions"
	-isystem "$NXDK_DIR/lib/pdclib/include"
	-I "$NXDK_DIR/lib/pdclib/platform/xbox/include"
	-I "$NXDK_DIR/lib/winapi"
	-I "$NXDK_DIR/lib/xboxrt/vcruntime"
)

OUT="${OUT:-/tmp/$(basename "${SRC%.cpp}").obj}"

# Opt-in object cache, off unless configured: tools/xbox-cc-cache.sh says
# what a hit is and why it is the object this compile would produce. The
# file form is the one that survives the MSYS2 shell boundary (lessons.md).
# read, not $(cat): a fork per compile is measurable on MSYS2.
CC_CACHE="${XBOX_CC_CACHE_DIR:-}"
[ -n "$CC_CACHE" ] || { [ -f "$REPO/.sweep/cc-cache-dir" ] && read -r CC_CACHE < "$REPO/.sweep/cc-cache-dir"; }
CC_WRAP=()
[ -n "$CC_CACHE" ] && CC_WRAP=( env NXDK_DIR="${NXDK_DIR:-}" bash "$REPO/tools/xbox-cc-cache.sh" "$CC_CACHE" )

exec ${CC_WRAP[@]+"${CC_WRAP[@]}"} clang++ \
	-target i386-pc-win32 \
	-march=pentium3 \
	"$XBOX_OPT" \
	-fuse-ld=nxdk-link \
	-ffreestanding \
	-nostdlib \
	-fno-builtin \
	-Wno-builtin-macro-redefined \
	-include "$REPO/compat/xbox/xbox_prelude.h" \
	-DNXDK \
	-D__STDC__=1 \
	-U__STDC_NO_THREADS__ \
	-fno-exceptions \
	-std=c++17 \
	"${OUR_INCLUDES[@]}" \
	"${NXDK_INCLUDES[@]}" \
	-c "$SRC" -o "$OUT" "$@"
