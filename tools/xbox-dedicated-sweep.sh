#!/usr/bin/env bash
# X2 W5 - compile and link the real Xbox DEDICATED-SERVER target: the same
# source list engine/wscript itself builds for the PC `xash`/
# `xash_tests_dedicated` targets (engine/wscript:169-194's default source
# set, captured as `server_source` BEFORE any `if bld.env.CLIENT:` block
# runs - verified against this repo's own compile_commands.json from a
# real, clean, from-scratch `--dedicated` build: 109 files, zero under
# engine/client/), with platform/win32/*.c swapped for platform/xbox/*.c.
#
#   MSYSTEM=MINGW64 /c/msys64/usr/bin/bash tools/xbox-dedicated-sweep.sh
#
# Two extra files beyond PC's own list, both load-bearing here and absent
# there: engine/platform/sdl2/sys_sdl2.c, because xbox-cc.sh's
# unconditional -DXASH_SDL=2 (divergence #13) makes common/defaults.h's
# TIMER selection pick TIMER_SDL regardless of XASH_DEDICATED (only the
# VIDEO/INPUT/SOUND arm is `#if !XASH_DEDICATED`-gated, not the timer one -
# common/defaults.h:32,75-77) - PC's real dedicated build instead gets
# TIMER_WIN32 from platform/win32/sys_win.c, which does not compile for
# Xbox at all (missing shellapi.h); and 3rdparty/library_suffix/src (build
# identification strings every Xbox target links, per tools/xbox-sweep.sh's
# own file list).
#
# Uses tools/xbox-cc.sh's XBOX_DEDICATED=1 opt-in (see that script's own
# comment): net_chan.c/net_ws.c/xrcon.c/net_http_tls.c/net_http_xash.c
# compile from real upstream sources under it, unmodified beyond
# platform/stub/net_stub.h growing the BSD types they already expected.
#
# The real entry point is engine/common/launcher.c's own main() (also
# opted in by XBOX_DEDICATED - it defines XASH_ENABLE_MAIN too), the same
# statically-linked path PSVita/iOS already use: Host_Main(argc, argv,
# "valve", 0, Sys_ChangeGame). engine/platform/xbox/xbox_main.c's own
# main() (X1, divergence #23 - "deliberately not Host_Main") is EXCLUDED
# from this file list for exactly that reason: two mains would collide at
# link time, and its job (prove _main resolves at all) is done.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

MSYS2_BIN="/c/msys64/mingw64/bin"
if [ ! -d "$MSYS2_BIN" ]; then
	echo "xbox-dedicated-sweep: $MSYS2_BIN not found - see tools/xbox-env.sh." >&2
	exit 1
fi
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
export XBOX_DEDICATED=1

SCRATCH="${SCRATCH:-$REPO/.sweep-dedicated}"
mkdir -p "$SCRATCH" || exit 1

# Found in review (Codex, round 2): reusing $SCRATCH across runs and never
# removing anything meant a source file later renamed, excluded or deleted
# left its LAST-SUCCESSFUL .obj sitting there - the next sweep would just
# skip the now-missing file (silently, the same "does not exist" path
# every other file takes when it is not found) and tools/xbox-dedicated-
# link-check.sh's plain `ls *.obj` would still find and link the stale
# object, reporting a clean link that packages code no longer in the
# checkout. Same failure shape tools/xbox-mainui-sweep.sh already guards
# against (its own manifest.txt) - matched here: clear everything before
# compiling, and only the consuming script trusts a manifest written
# after a verified-clean, non-empty run.
rm -f "$SCRATCH"/*.obj "$SCRATCH"/*.err "$SCRATCH/manifest.txt"

# The real dedicated source list, PC's own (see header) with win32 -> xbox.
FILES=(
	engine/common/*.c
	engine/common/imagelib/*.c
	engine/common/soundlib/*.c
	engine/common/http/*.c
	engine/server/*.c
	engine/platform/xbox/*.c
	engine/platform/sdl2/sys_sdl2.c
	engine/platform/misc/lib_static.c   # engine/wscript:193-194, XASH_STATIC_LIBS
	filesystem/*.c
	filesystem/*.cpp
	public/*.c
	3rdparty/library_suffix/src/*.c
	compat/xbox/*.c                     # real POSIX I/O (open/read/write/lseek/
	                                     # stat/opendir/mkdir/...) and Win32-API
	                                     # shims nxdk itself does not provide
)

failed=0
compiled=0
declare -A seen_errors
for pattern in "${FILES[@]}"; do
	for f in $pattern; do
		[ -f "$f" ] || continue
		base=$(basename "$f")
		# Same exclusions as PC's own dedicated build: test/CLI files a real
		# xash_tests_dedicated build does not link either (waf's own
		# engine/wscript never globs filesystem/tests or public/tests into
		# this target - those get their own bld.program() calls).
		case "$f" in
			filesystem/tests/*|public/tests/*) continue ;;
			engine/platform/xbox/xbox_main.c) continue ;; # see header: launcher.c is the real main() here
		esac
		obj="$SCRATCH/$(echo "$f" | tr '/' '_' | sed -E 's/\.(c|cpp)$/.obj/')"
		errfile="$SCRATCH/$(echo "$f" | tr '/' '_').err"
		extra=()
		case "$f" in
			# nxdk-link has no C++ exception-handling runtime at all (no
			# lib provides __CxxFrameHandler3/__std_terminate - grep-checked
			# against every deps/nxdk/lib/*.lib). VFileSystem009.cpp's only
			# use of it is compiler-generated cleanup code for a
			# non-trivial destructor, not an explicit try/catch/throw in
			# its own source - compiles and behaves identically without it.
			filesystem/VFileSystem009.cpp) extra=( -fno-exceptions -fno-rtti ) ;;
		esac
		if tools/xbox-cc.sh "$f" "${extra[@]}" -o "$obj" >"$errfile" 2>&1; then
			compiled=$((compiled + 1))
		else
			nerr=$(grep -c "error:" "$errfile")
			echo "  FAIL $f  ($nerr errors)"
			failed=$((failed + 1))
			firstline=$(grep -m1 "error:" "$errfile" | sed -E 's/^[^:]+:[0-9]+:[0-9]+: //')
			seen_errors["$firstline"]=$(( ${seen_errors["$firstline"]:-0} + 1 ))
		fi
	done
done

if [ $failed -gt 0 ]; then
	echo "xbox-dedicated-sweep: distinct first-errors, most common first:"
	for msg in "${!seen_errors[@]}"; do
		printf "  %3d  %s\n" "${seen_errors[$msg]}" "$msg"
	done | sort -rn
fi

echo "xbox-dedicated-sweep: $compiled ok, $failed failing   (scratch: $SCRATCH)"

# Written only on a clean, non-empty run - never "OK=0 FAIL=0 TOTAL=0" for
# a run that exits out early (there is no early exit above to worry about,
# but the ordering - after the failure report, before any exit) matches
# tools/xbox-mainui-sweep.sh's own fix for exactly that shape of bug.
if [ $failed -eq 0 ] && [ $compiled -gt 0 ]; then
	echo "OK=$compiled FAIL=$failed TOTAL=$((compiled + failed))" > "$SCRATCH/manifest.txt"
fi

[ $failed -eq 0 ] && [ $compiled -gt 0 ]
