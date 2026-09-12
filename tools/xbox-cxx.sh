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
# -D_UINTPTR_T_DEFINED: found 2026-09-12 compiling the real hl CLIENT SDK
# for the first time (tools/xbox-game-sweep.sh hl client) - root-caused to
# the fact that this target/flag combination defines _MSC_VER (verified
# directly: a `#ifdef _MSC_VER / #error` probe fires), which is enough for
# deps/hlsdk/external/SDL2's own `#if defined(_MSC_VER)`-guarded
# compatibility branches to activate even though nothing here is real
# MSVC. Blanket `-U_MSC_VER` was tried and rejected - nxdk's own
# libcxx/pdclib headers rely on `_MSC_VER` being defined for their own
# real branches (verified: undefining it globally breaks deps/nxdk/lib/
# libcxx/include/string.h's own strstr overload resolution, a real,
# working, unrelated codepath). SDL_config_windows.h's own
# `#ifndef _UINTPTR_T_DEFINED` block redeclares `uintptr_t` as `unsigned
# int`, conflicting with pdclib's own already-correct `unsigned long`
# typedef (a real "typedef redefinition with different types" hard error,
# not a warning) - this makes SDL2 skip only that one redundant typedef,
# not the surrounding block's other real setup (tried and rejected:
# skipping the whole block via `-D_STDINT_H_` fixed this but broke other
# things the same block also does, a strictly worse trade).
#
# A companion `_m_prefetch`/SDL_cpuinfo.h SIMD-macro conflict (same root
# cause) was found in the same pass - first fixed here too with a global
# `-D__PRFCHWINTRIN_H`, then corrected in Codex review: that flag
# predefines a header guard clang's OWN real `<intrin.h>` uses, so it
# would ALSO suppress clang's real `_m_prefetch`/`_m_prefetchw`
# declarations for any future file that legitimately wants them - fine
# for today's sweep (nothing does), wrong as a permanent global flag.
# Fixed instead at the source: `deps/hlsdk/external/SDL2/SDL_endian.h`
# and `SDL_cpuinfo.h` (divergence #39) each gained a `&& !defined(NXDK)`
# on the exact `_MSC_VER`-guarded block that redefines `_m_prefetch` (and,
# in SDL_cpuinfo.h's case, unconditionally `#define`s `__SSE2__`/
# `__SSE3__` too - wrong on real Xbox hardware, which clang's own
# `-march=pentium3` already correctly omits, verified directly). Verified:
# cl_dll/cdll_int.cpp's real error count dropped from the SAL-macro gap
# (compat/xbox/sal.h), the uintptr_t conflict, and the _m_prefetch
# conflict combined, down to just the separately-tracked, already-known
# `<memory_resource>` want; re-verified the real hl server sweep (107/107)
# and 3rdparty/mainui (82/82) are both unaffected.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

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

eval "$("$REPO/deps/nxdk/bin/activate" -s)"

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

exec clang++ \
	-target i386-pc-win32 \
	-march=pentium3 \
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
	-D_UINTPTR_T_DEFINED \
	"${OUR_INCLUDES[@]}" \
	"${NXDK_INCLUDES[@]}" \
	-c "$SRC" -o "$OUT" "$@"
