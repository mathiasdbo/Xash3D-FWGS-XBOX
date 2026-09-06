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
OUR_INCLUDES=(
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
	                              # inflate path, via img_png.c and
	                              # filesystem_internal.h - so dropping stdio
	                              # removes the MSVC 64-bit CRT surface
	                              # (_ftelli64, _fseeki64, _wstat64, errno_t)
	                              # rather than shimming it.
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
