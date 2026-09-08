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
# a short, stable script.
#
# Corrected 2026-09-07, found in review: this comment used to claim the
# divergence from nxdk-cc was the include order, nothing else - false,
# -fuse-ld=nxdk-link was silently missing below too. Currently inert, because
# this script always ends in -c and clang never invokes a linker either way -
# but a future version of this script that drops -c to attempt a real link
# would silently fall back to the default linker instead of nxdk-link with no
# error, which is exactly the kind of failure this project's tools keep
# producing when a claim about "the only difference" turns out to be wrong.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

# Auto-apply the XASH_XBOX sub-platform patch to the library_suffix submodule
# if it is not already applied. Idempotent - a grep before the apply, so a
# fresh clone gets patched on the first invocation and every later one is a
# no-op file read.
#
# Found in review, 2026-09-07: without this, XASH_XBOX is never defined on a
# fresh clone + `git submodule update --init`, because the patch lived only
# as a dirty, uncommitted change in one working tree's submodule checkout -
# invisible to git log, invisible to CI, invisible to anyone else who clones
# this branch. Every `#if XASH_XBOX` in the tree silently evaluates to false
# (an undefined macro in #if is 0, not an error), which is worse than a build
# failure: everything still compiles, just as the wrong platform. Divergence
# #5 in docs/r3d/divergences.md already named this "fragile as a process -
# an unapplied patch looks like a compiler bug"; this is that process fixed,
# not a new problem this script introduces.
BUILD_H="$REPO/3rdparty/library_suffix/include/build.h"
if [ ! -f "$BUILD_H" ]; then
	# Found in review: the `[ -f "$BUILD_H" ] &&` below short-circuits
	# silently when the submodule was never initialised at all
	# (`git submodule update --init`), which is exactly the fresh-clone case
	# this whole mechanism exists for - and unlike a failed git apply, that
	# case emitted no XBOX_CC_PREFLIGHT_FAILURE marker, so xbox-sweep.sh's
	# fast-fail never fired either; every file would instead fail
	# individually on clang's own "build.h: file not found", the identical
	# diagnosis repeated once per translation unit.
	echo "XBOX_CC_PREFLIGHT_FAILURE: 3rdparty/library_suffix/include/build.h does not exist"
	echo "  - the submodule is not initialised. Run:" >&2
	echo "  git submodule update --init 3rdparty/library_suffix" >&2
	exit 1
elif ! grep -q "define XASH_XBOX 1" "$BUILD_H"; then
	# -c core.autocrlf=false, not just relying on whatever is configured:
	# under MSYSTEM=MINGW64 (this script's own required invocation, per
	# xbox-env.sh) $HOME resolves to MSYS2's own home directory, not the
	# Windows user profile, so the personal .gitconfig that sets
	# core.autocrlf=false is never found - only the system-wide gitconfig at
	# `C:/Program Files/Git/etc/gitconfig`, which sets it TRUE, applies.
	# build.h is a pure-LF file (measured: 0 CRLF, 290 bare LF in the pinned
	# commit) and the patch is pure LF too, so with autocrlf=true still in
	# effect `git apply` "succeeds" - exit 0, XASH_XBOX genuinely gets
	# defined - while silently rewriting the entire file to CRLF in the
	# process, which turned a 9-line diff into a 589-line one and would have
	# made every future `git -C 3rdparty/library_suffix diff` against the
	# pinned commit unreadable. Caught only by testing this from a clean
	# checkout before shipping it, not by the mechanism itself.
	if ! git -c core.autocrlf=false -C "$REPO/3rdparty/library_suffix" apply \
		"$REPO/patches/library_suffix-xbox-subplatform.patch" 2>&1; then
		# XBOX_CC_PREFLIGHT_FAILURE, not just an error message: found in
		# review, git apply's own failure text ("error: patch failed: ...")
		# contains the literal substring xbox-sweep.sh greps for to count
		# compiler errors, so without a distinct marker this preflight
		# failure would be silently misclassified as an ordinary N-error
		# compile failure for every single file in a sweep - defeating the
		# exact "broken toolchain vs real compile error" distinction that
		# tool exists to make, by feeding it something that is neither a
		# broken toolchain in the sense it checks for (a mute compiler) nor
		# a real per-file compile error, but a third thing it had no way to
		# recognize. xbox-sweep.sh checks for this exact string first, ahead
		# of the generic "error:" count.
		echo "XBOX_CC_PREFLIGHT_FAILURE: could not apply patches/library_suffix-xbox-subplatform.patch"
		echo "  to 3rdparty/library_suffix/include/build.h - check it still applies" >&2
		echo "  cleanly against the submodule's pinned commit." >&2
		exit 1
	fi
fi

# MSYS2's MinGW-64 clang, by absolute path.
#
# NOT "/mingw64/bin". That only resolves when MSYS2's own bash is interpreting
# this script; when the shebang picks Git Bash instead - which happens whenever
# something exec's this file directly rather than passing it to a chosen bash -
# the path is meaningless, PATH silently gains nothing, and nxdk's activate
# reports "clang: command not found" three lines before the real failure.
#
# /c/msys64/... resolves in both, because both mount C: at /c.
export PATH="/c/msys64/mingw64/bin:$PATH"

eval "$("$REPO/deps/nxdk/bin/activate" -s)"

SRC="${1:?usage: xbox-cc.sh <file.c> [extra args]}"
shift

# Ours first. compat/xbox must precede nxdk's own directories.
#
# Mirrors what engine/wscript:163-167 hands each module: '.', 'common',
# 'common/imagelib' exported by engine_includes, plus 'server', 'client',
# 'client/vgui', 'common/soundlib' and 'platform'. Getting this wrong looks
# exactly like a platform problem - a short include set made engine/common/
# cvar.h report `unknown type name 'convar_t'`, which is neither a missing
# header nor an nxdk gap, just common/cvardef.h not being reachable yet.
# generated_library_tables.h is a BUILD ARTIFACT, not a source file: waf writes
# it during the build (xshlib.py:112) and the Xbox path bypasses waf. Generate it
# here so engine/platform/misc/lib_static.c compiles, and keep it out of the tree
# so it cannot go stale against the real library list.
GEN_DIR="$REPO/.sweep/gen"
if [ ! -f "$GEN_DIR/generated_library_tables.h" ]; then
	"$REPO/tools/gen-library-tables.sh" "$GEN_DIR" > /dev/null
fi

OUR_INCLUDES=(
	-I "$GEN_DIR"
	-I "$REPO/compat/xbox"
	-I "$REPO/public"
	-I "$REPO/common"
	-I "$REPO/engine"
	-I "$REPO/engine/common"
	-I "$REPO/engine/common/imagelib"
	-I "$REPO/engine/common/soundlib"
	-I "$REPO/engine/server"
	-I "$REPO/engine/client"
	-I "$REPO/engine/client/vgui"
	-I "$REPO/engine/platform"
	-I "$REPO/filesystem"
	-I "$REPO/pm_shared"
	-I "$REPO/3rdparty/library_suffix/include"
	-I "$REPO/3rdparty/MultiEmulator/include"        # cl_main.c
	-I "$REPO/3rdparty/opus/opus/include"            # voice.c
	-I "$REPO/3rdparty/opusfile/opusfile/include"    # snd_ogg_opus.c
	-I "$REPO/3rdparty/vorbis/vorbis-src/include"    # snd_ogg_vorbis.c
	-I "$REPO/3rdparty/libogg/libogg/include"        # both ogg decoders
	-I "$NXDK_DIR/lib/sdl/SDL2/include"   # XASH_SDL=2 means <SDL.h> must resolve
	-I "$NXDK_DIR/lib/net/lwip/src/include"  # <lwip/sockets.h> etc. -
	                                          # deps/nxdk/lib/net/Makefile:22
	-I "$NXDK_DIR/lib/net/nforceif/include"  # lwipopts.h - the config nxdk's
	                                          # own build actually used,
	                                          # deps/nxdk/lib/net/Makefile:23
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

# What engine/wscript defines for engine translation units. Without these the
# failures look like platform problems and are not: common/cvardef.h:96 hides
# `convar_t` behind `#if REF_DLL || ENGINE_DLL`, so a missing -DENGINE_DLL
# reports as `unknown type name 'convar_t'` in engine/common/cvar.h, twenty
# errors deep in a file that has nothing wrong with it.
#
# XASH_STATIC_LIBS is on because the Xbox has no dynamic loading at all
# (deps/nxdk/lib/winapi/libloaderapi.c:16-23 returns NULL unconditionally).
# Note it also implies XASH_INTERNAL_GAMELIBS and XASH_ALLOW_SAVERESTORE_OFFSETS
# via common/defaults.h:102-105 — see docs/r3d/x0-probe-results.md.
ENGINE_DEFINES=(
	-DENGINE_DLL=1                # engine/wscript:135
	-DXASH_STATIC_LIBS=1          # engine/wscript:155, and mandatory here
	-DXASH_NO_LIBDL=1             # engine/wscript:127
	-DXASH_CRASHHANDLER=0         # engine/wscript:83
	-D__XBOX__=1                  # SDL_config.h:32,46 picks SDL_config_windows.h
	                              # unless this is set, and that config typedefs
	                              # uintptr_t as unsigned int, colliding with
	                              # pdclib. Selects SDL_config_xbox.h instead.
	-DXASH_BUILD_COMMIT=\"xbox-bringup\"    # normally injected by waf from git;
	-DXASH_BUILD_BRANCH=\"x0\"              # public/build_vcs.c reads all three
	-DXASH_BUILD_COMMIT_DATE=\"0\"
	-DXASH_NO_NETWORK=1           # net_ws_private.h:17 selects the stub. It is
	                              # upstream's own no-network mode and the
	                              # direction X3 takes, but the stub is not
	                              # self-contained on a platform with no BSD
	                              # headers - see docs/r3d/fork-plan.md X0.
	-DMINIZ_NO_STDIO=1            # miniz's own supported flag. Nothing in the
	                              # engine uses its file API - only the
	                              # in-memory inflate path, via img_png.c and
	                              # filesystem_internal.h - so dropping stdio
	                              # removes _wstat64 and the archive-writing
	                              # CRT surface.
	                              #
	                              # It does NOT remove errno_t. miniz's TIME
	                              # path stays on, because MINIZ_NO_TIME broke
	                              # three filesystem/ files that were getting
	                              # <time.h> transitively through miniz.h. So
	                              # errno_t and localtime_s are still shimmed.
	                              # _ftelli64/_fseeki64 are shimmed too, but
	                              # for vorbis, not for miniz.
	-DXASH_SDL=2                  # nxdk ships XboxDev/nxdk-sdl, SDL 2.0.9 with
	                              # native src/video/xbox, src/audio/xbox and
	                              # src/joystick/xbox drivers. common/defaults.h:33
	                              # then selects VIDEO_SDL / INPUT_SDL / SOUND_SDL,
	                              # which is why no new backend enum is needed.
)

OUT="${OUT:-/tmp/$(basename "${SRC%.c}").obj}"

exec clang \
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
	"${ENGINE_DEFINES[@]}" \
	"${OUR_INCLUDES[@]}" \
	"${NXDK_INCLUDES[@]}" \
	-c "$SRC" -o "$OUT" "$@"
