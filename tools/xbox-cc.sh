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

# Same auto-apply mechanism, same reasoning, for opusfile's stream.c: found
# in review, X1, 2026-09-08. op_get_file_callbacks/op_fopen/op_freopen's
# _WIN32 branch calls GetFileType (nxdk's winapi does not have it) and
# _wfopen/_wfreopen (nxdk has no wide-char CRT at all) - neither is a gap
# this project can leave unpatched the way a missing symbol elsewhere gets
# a compat/xbox shim, because nxdk has no equivalent Win32 API to shim
# against. Widened to NXDK-specific branches instead: always-seekable (this
# platform's FILE* is always a real FATX file, never a pipe) and plain
# fopen/freopen (GoldSrc paths are ASCII, no Unicode filename to preserve).
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
#
# server/client, X1's stub game module: only lib_static.c itself includes this
# header, so passing real library names here does not affect any other file
# this script compiles - everything else that reaches this -I just never
# references the symbols this generates.
#
# menu: found in review, 2026-09-09 - an earlier version of this branch
# compiled and linked in all 82 mainui objects but never registered "menu"
# in this table at all. engine/common/lib_common.c:220's own default
# (COM_GenerateClientLibraryPath("menu",...)) and engine/common/host.c:914
# both confirm "menu" (not "gameui" or anything else) is the real name
# lib_static.c's Lib_Find has to resolve - under XASH_INTERNAL_GAMELIBS
# (set whenever XASH_STATIC_LIBS is, already true here), that function
# just copies the name through unchanged (lib_common.c:133-134). mainui's
# own 3rdparty/mainui/exports.txt (GetMenuAPI, GetExtAPI) already names
# its real entry points - those functions genuinely exist in
# 3rdparty/mainui/udll_int.cpp, this just wires the already-real menu
# module into the same by-name table the (genuinely stub) server/client
# modules use, no new stub code needed for menu at all.
#
# filesystem_stdio: found booting X2 W5's dedicated target in xemu for the
# first time (CHANGELOG's "a first real boot attempt") - Xbox_NetLogInit
# wired into the boot path showed the real error the boot-animation hang
# had been hiding: "FS_LoadProgs: can't load filesystem library
# filesystem_stdio", looping (Sys_Error -> host shutdown -> BIOS reboots
# -> same failure) exactly like the observed stuck animation. The same
# gap as "menu" above, same fix: filesystem_engine.c:219
# (#ifdef XASH_INTERNAL_GAMELIBS) names "filesystem_stdio" as the literal
# library string COM_LoadLibrary gets, and filesystem/exports.txt
# (GetFSAPI) already names the real entry point - filesystem/*.c is
# already part of this build's own object set (engine and filesystem are
# ordinarily two separate binaries upstream; the Xbox dedicated target
# links them into one, so the real GetFSAPI is already sitting right
# there needing only to be named in this table, exactly as real as menu's
# fix, not a new stub.
#
# Found in review, 2026-09-09: checking only generated_library_tables.h's own
# existence let a $GEN_DIR left behind by an older version of this script -
# back when it generated that header with zero library names, before
# generated_lib_*_exports.c existed at all - look "already generated" and
# skip regenerating forever, even though the exports files this script
# now also needs were never produced. Every expected output is checked now,
# not just the first one written.
# Found in review, 2026-09-10: existence alone is not freshness. Once
# every generated_lib_*_exports.c existed, editing an exports.txt (e.g.
# adding a third filesystem_stdio entry point after this one) would not
# regenerate anything - the stale array would keep compiling and linking
# clean, silently missing the new entry point at boot. Exactly the
# runtime-only failure divergence #30 (this same commit) just fixed.
# Each generated file's mtime is checked against its OWN exports.txt via
# `-nt` (newer-than) - the existence checks above still run first so a
# freshly-cloned tree with no $GEN_DIR at all still regenerates, rather
# than every `-nt` test failing against a nonexistent left-hand file.
GEN_DIR="$REPO/.sweep/gen"
GEN_STALE=0
if [ ! -f "$GEN_DIR/generated_library_tables.h" ] \
	|| [ ! -f "$GEN_DIR/generated_lib_server_exports.c" ] \
	|| [ ! -f "$GEN_DIR/generated_lib_client_exports.c" ] \
	|| [ ! -f "$GEN_DIR/generated_lib_menu_exports.c" ] \
	|| [ ! -f "$GEN_DIR/generated_lib_filesystem_stdio_exports.c" ] \
	|| [ ! -f "$GEN_DIR/generated_lib_ref_null_exports.c" ] \
	|| [ ! -f "$GEN_DIR/generated_lib_ref_soft_exports.c" ] \
	|| [ ! -f "$GEN_DIR/generated_lib_ref_nv2a_exports.c" ]; then
	GEN_STALE=1
elif [ "$REPO/stub_game/server/exports.txt" -nt "$GEN_DIR/generated_lib_server_exports.c" ] \
	|| [ "$REPO/stub_game/client/exports.txt" -nt "$GEN_DIR/generated_lib_client_exports.c" ] \
	|| [ "$REPO/3rdparty/mainui/exports.txt" -nt "$GEN_DIR/generated_lib_menu_exports.c" ] \
	|| [ "$REPO/filesystem/exports.txt" -nt "$GEN_DIR/generated_lib_filesystem_stdio_exports.c" ] \
	|| [ "$REPO/ref/null/exports.txt" -nt "$GEN_DIR/generated_lib_ref_null_exports.c" ] \
	|| [ "$REPO/ref/soft/exports.txt" -nt "$GEN_DIR/generated_lib_ref_soft_exports.c" ] \
	|| [ "$REPO/ref/nv2a/exports.txt" -nt "$GEN_DIR/generated_lib_ref_nv2a_exports.c" ]; then
	GEN_STALE=1
fi
if [ "$GEN_STALE" = "1" ]; then
	"$REPO/tools/gen-library-tables.sh" "$GEN_DIR" \
		"server:$REPO/stub_game/server/exports.txt" \
		"client:$REPO/stub_game/client/exports.txt" \
		"menu:$REPO/3rdparty/mainui/exports.txt" \
		"filesystem_stdio:$REPO/filesystem/exports.txt" \
		"ref_null:$REPO/ref/null/exports.txt" \
		"ref_soft:$REPO/ref/soft/exports.txt" \
		"ref_nv2a:$REPO/ref/nv2a/exports.txt" \
		> /dev/null
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
	-DALLOCA_H=\<malloc.h\>     # filesystem/VFileSystem009.cpp's only
		                              # gap: nxdk's alloca() lives in
		                              # xboxrt/libc_extensions/malloc.h
		                              # (#define alloca(size) _alloca(size)),
		                              # not <alloca.h> (does not exist here).
		-DXASH_SDL=2                  # nxdk ships XboxDev/nxdk-sdl, SDL 2.0.9 with
	                              # native src/video/xbox, src/audio/xbox and
	                              # src/joystick/xbox drivers. common/defaults.h:33
	                              # then selects VIDEO_SDL / INPUT_SDL / SOUND_SDL,
	                              # which is why no new backend enum is needed.
	-DXASH_REF_SOFT_ENABLED=1     # X5: waf sets this per configured renderer
	                              # (build/c4che/engine_cache.py shows the PC
	                              # build's own value); it only puts "soft" in
	                              # R_CollectRendererNames()'s auto-try list
	                              # (ref_common.c:709). ref/soft + ref/common
	                              # are compiled and linked by
	                              # tools/xbox-link-check.sh; a disc's retail
	                              # video.cfg (`r_refdll ""`) then selects soft
	                              # through R_Init()'s own loop, ref_null
	                              # staying the last resort it always was.
	-DXASH_REF_NV2A_ENABLED=1     # X9 bring-up: same mechanism as
	                              # XASH_REF_SOFT_ENABLED above, puts "nv2a" in
	                              # R_CollectRendererNames()'s auto-try list.
	                              # ref/nv2a + ref/common are compiled and
	                              # linked by tools/xbox-link-check.sh; a
	                              # disc's video.cfg selects it explicitly
	                              # (`r_refdll "nv2a"`) rather than relying on
	                              # auto-try order, so it never silently
	                              # displaces the already-working ref_soft.
	-DXASH_LOW_MEMORY=1           # X3: upstream's own low-memory mode, level 1
	                              # (the level MotoMAGX ships with, wscript:288;
	                              # level 2 changes the protocol, 1 does not -
	                              # netchan.h:82-101's own comments). Shrinks
	                              # exactly what X5's first map ran the 64 MiB
	                              # console out of: SINGLEPLAYER_BACKUP 64->4 and
	                              # NUM_PACKET_ENTITIES 256->64 (the failing
	                              # cls.packet_entities allocation), NET_MAX_FRAGMENT
	                              # 65535->32768, MAX_MODELS 4096->1024
	                              # (mod_known), MAX_VISIBLE_PACKET 512->256,
	                              # MAX_STATIC_ENTITIES 3096->128, and ref_soft's
	                              # MAX_TEXTURES 8192->1024 (r_local.h:49).
)

# ref/<module>/*.c and ref/common/*.c (X5): renderer modules are built by
# ref/<module>/wscript with REF_DLL=1 (NOT ENGINE_DLL - common/cvardef.h:96
# and engine/ref_api.h:661 both branch on which one is set) and with
# ref/<module> + ref/common ahead of the engine's include set. The order
# matters for one concrete reason: `#include "ref_common.h"` from
# ref/soft/r_local.h must resolve to ref/common/ref_common.h, but
# engine/client/ref_common.h (the ENGINE's own file of the same name) is
# already on OUR_INCLUDES via engine/client - with the engine set first, the
# wrong header wins and every ref/soft file fails with `unknown type name
# 'cl_entity_t'` twenty lines into r_local.h, which looks like a missing
# platform header and is not.
REF_INCLUDES=()
case "${SRC#"$REPO"/}" in
	ref/*)
		REF_MODULE="$(printf '%s' "${SRC#"$REPO"/}" | cut -d/ -f2)"
		REF_INCLUDES=( -I "$REPO/ref/$REF_MODULE" -I "$REPO/ref/common" )
		ENGINE_DEFINES=( "${ENGINE_DEFINES[@]/-DENGINE_DLL=1/-DREF_DLL=1}" )
		;;
esac

# XBOX_DEDICATED=1 (env, opt-in): X2 W5's dedicated Xbox target -
# engine/wscript's own "# basic build: dedicated only" source set
# (common/*.c, common/imagelib/*.c, common/soundlib/*.c, server/*.c,
# common/http/*.c), same as XASH_DEDICATED=1 already does for the PC
# `xash`/`xash_tests_dedicated` targets (engine/wscript:229,244) - verified
# by actually compiling the six files that used to be part of the parked
# 63-undefined-symbol networking gap under this exact define plus the
# platform/stub/net_stub.h additions below: net_chan.c, net_ws.c, xrcon.c,
# net_http_tls.c and net_http_xash.c now compile CLEAN (net_chan.c's whole
# gap was one #if !XASH_DEDICATED bzlib.h include it already carried;
# net_ws.c/xrcon.c needed net_stub.h completed with real IPv6/fd_set/
# getaddrinfo shapes, not a new define). cl_steam.c (client-only) is the
# one pre-existing failure this does NOT fix and does not need to - a
# dedicated build never compiles engine/client/*.c at all, matching
# engine/wscript:169-194's own default source list before any `if
# bld.env.CLIENT:` block runs.
#
# Deliberately NOT the default: every OTHER Xbox tool (xbox-sweep.sh's
# broad multi-platform coverage sweep, xbox-mainui-sweep.sh, the
# third-party compiles inside xbox-link-check.sh) wants the existing,
# unconditional define set, matching the eventual full client+menu target
# those measurements already track (divergences #20-23). Opt-in here keeps
# every prior "N ok / M failing" and "0 duplicate / K undefined" baseline
# meaning exactly what it always meant.
if [ "${XBOX_DEDICATED:-0}" = "1" ]; then
	ENGINE_DEFINES+=( -DXASH_DEDICATED=1 )
	# engine/common/launcher.c's own main() (XASH_GAMEDIR defaults to
	# "valve", matching every real bake this project has done so far) -
	# the same statically-linked, no-dynamic-loading entry point PSVita
	# and iOS already use, in place of engine/platform/xbox/xbox_main.c's
	# placeholder (X1, divergence #23 - kept as-is for the broad,
	# non-dedicated sweep this flag does not affect; excluded from THIS
	# target's file list by tools/xbox-dedicated-sweep.sh instead of
	# colliding with launcher.c's own main() at link time).
	ENGINE_DEFINES+=( -DXASH_ENABLE_MAIN=1 )
fi

# XBOX_CLIENT_MAIN=1 (env, opt-in, independent of XBOX_DEDICATED): the
# client-target equivalent of the block above - sets -DXASH_ENABLE_MAIN=1
# so engine/common/launcher.c's main() calls Host_Main for real, but
# WITHOUT -DXASH_DEDICATED=1, so common/defaults.h still selects
# VIDEO_SDL/INPUT_SDL/SOUND_SDL (XASH_SDL=2, above) instead of the
# dedicated-only source set. Needed because the two defines were
# previously coupled together above - the dedicated target is the only
# one that has ever called Host_Main for real, so this is the first time
# the client's own boot path (CL_Init, video/sound/input bring-up, all of
# it) is reachable from a linker's perspective at all. Before this,
# engine/platform/xbox/xbox_main.c's placeholder main() (X1, divergence
# #23) provided `_main` for the client link and never referenced
# Host_Main, so X1's "63 undefined, 0 duplicate" result never actually
# proved the client's real boot code links - only that the dead-code
# scaffolding around it does. xbox_main.c must be excluded from this
# target's file list, same as XBOX_DEDICATED already excludes it, to
# avoid a duplicate `_main`.
if [ "${XBOX_CLIENT_MAIN:-0}" = "1" ]; then
	ENGINE_DEFINES+=( -DXASH_ENABLE_MAIN=1 )
fi

# XBOX_ENGINE_TESTS=1 (env, opt-in, independent of XBOX_DEDICATED): the
# same boundary XASH_ENGINE_TESTS already draws elsewhere in this tree
# (test_dumpworld, test_mapprobe) - test/debug-only instrumentation,
# never in a real build. Xbox_NetLogInit() (engine/common/host.c, X2 W5
# follow-up) is gated behind this specifically because it is genuinely
# blocking (~10s DHCP wait with no cable/server) and must not cost every
# real, shipped boot that delay just to enable a diagnostic UDP log
# channel - found in review after the first version called it
# unconditionally on every Xbox boot. Pass XBOX_ENGINE_TESTS=1 when
# debugging a real boot (the exact xemu+listener workflow the CHANGELOG
# entry above describes); leave it unset for anything meant to represent
# what a real deployment would do.
if [ "${XBOX_ENGINE_TESTS:-0}" = "1" ]; then
	ENGINE_DEFINES+=( -DXASH_ENGINE_TESTS=1 )
fi

OUT="${OUT:-/tmp/$(basename "${SRC%.c}").obj}"

# Opt-in object cache, off unless configured: tools/xbox-cc-cache.sh says
# what a hit is and why it is the object this compile would produce. The
# file form is the one that survives the MSYS2 shell boundary (lessons.md).
# read, not $(cat): a fork per compile is measurable on MSYS2.
CC_CACHE="${XBOX_CC_CACHE_DIR:-}"
[ -n "$CC_CACHE" ] || { [ -f "$REPO/.sweep/cc-cache-dir" ] && read -r CC_CACHE < "$REPO/.sweep/cc-cache-dir"; }
CC_WRAP=()
[ -n "$CC_CACHE" ] && CC_WRAP=( env NXDK_DIR="${NXDK_DIR:-}" bash "$REPO/tools/xbox-cc-cache.sh" "$CC_CACHE" )

exec ${CC_WRAP[@]+"${CC_WRAP[@]}"} clang \
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
	"${REF_INCLUDES[@]}" \
	"${OUR_INCLUDES[@]}" \
	"${NXDK_INCLUDES[@]}" \
	-c "$SRC" -o "$OUT" "$@"
