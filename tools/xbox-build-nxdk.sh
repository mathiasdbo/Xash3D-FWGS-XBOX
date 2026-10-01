#!/usr/bin/env bash
# xbox-build-nxdk.sh - build nxdk's own libraries at a chosen optimisation
# level, and record the level next to them.
#
#   bash tools/xbox-build-nxdk.sh            # -O2 (the default)
#   env XBOX_NXDK_OPT=-O0 bash tools/xbox-build-nxdk.sh   # nxdk's own default
#   env XBOX_NXDK_JOBS=4 bash tools/xbox-build-nxdk.sh    # make -j (default 2)
#
# Why: `make NXDK_ONLY=y` (tools/xbox-env.sh) passes no CFLAGS, so every
# nxdk library - pdclib's malloc/free/qsort/strtol, pbkit's pb_push*,
# winapi, xboxrt, SDL2, libc++ - was built at clang's default -O0 while
# the engine is -O2 (tools/xbox-opt.sh; docs/r3d/lessons.md, "NEVER report
# a CPU or frame-time number without reading the compile line"). This is
# the reproducible -O2 build: nothing inside deps/nxdk is edited.
#
# How: nxdk's Makefile compiles with `$(CC) $(NXDK_CFLAGS) $(CFLAGS)` and
# `$(CXX) $(NXDK_CXXFLAGS) $(CXXFLAGS)` (deps/nxdk/Makefile, the %.obj
# rules); every library adds its own includes and defines to NXDK_CFLAGS /
# NXDK_CXXFLAGS (lib/*/Makefile, lib/sdl, lib/net, lib/libcxx), never to
# CFLAGS, so a command-line CFLAGS adds the level without overriding any
# library's flags. The target flags stay nxdk-cc's own (-target
# i386-pc-win32 -march=pentium3 -ffreestanding -nostdlib -fno-builtin,
# deps/nxdk/bin/nxdk-cc); -fno-builtin is what keeps pdclib's memcpy loop
# from being turned into a call to memcpy at -O2. make rebuilds only what
# is older than its sources, so a level change starts with `make clean`.
#
# The stamp: $NXDK_DIR/lib/r3d-nxdk-opt.txt, an untracked build artifact
# beside the .lib files it describes (like the .lib files themselves; the
# submodule's pointer and sources are untouched). It is removed before the
# build starts and written only after make succeeded and every library the
# link uses is newer than the start of this run, so it never claims a
# level a half-finished build does not have. tools/xbox-link-check.sh
# prints it; no stamp means the libraries came from a plain
# `make NXDK_ONLY=y`: -O0.
#
# deps/nxdk is SHARED: worktrees point their links at the main checkout's
# built nxdk (.sweep/nxdk-dir, tools/xbox-link-check.sh). Rebuilding it
# changes the libraries every worktree's next link uses.
#
# Runs nxdk's make in MSYS2 MinGW-64 through nxdk's own bin/activate, as
# tools/xbox-env.sh requires; called from Git Bash it re-runs itself
# there, passing the settings as arguments (an `export` does not survive
# that shell boundary, docs/r3d/lessons.md).
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LEVEL="${1:-${XBOX_NXDK_OPT:--O2}}"
JOBS="${2:-${XBOX_NXDK_JOBS:-2}}"

case "$LEVEL" in
	-O0|-O1|-O2|-O3|-Os|-Oz) ;;
	*) echo "xbox-build-nxdk: level '$LEVEL' is not one of -O0 -O1 -O2 -O3 -Os -Oz" >&2; exit 2 ;;
esac
case "$JOBS" in
	''|*[!0-9]*) echo "xbox-build-nxdk: jobs '$JOBS' is not a number" >&2; exit 2 ;;
esac

if [ "${MSYSTEM:-}" != "MINGW64" ]; then
	exec env MSYSTEM=MINGW64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash -lc \
		'cd "$0" && exec bash tools/xbox-build-nxdk.sh "$1" "$2"' "$REPO" "$LEVEL" "$JOBS"
fi

NXDK="$REPO/deps/nxdk"
STAMP="$NXDK/lib/r3d-nxdk-opt.txt"
# The libraries tools/xbox-link-check.sh links (its LIBS list).
LIBS="lib/xboxkrnl/libxboxkrnl.lib lib/libpdclib.lib lib/libwinapi.lib lib/libnxdk_hal.lib
lib/libnxdk.lib lib/libnxdk_automount_d.lib lib/libpbkit.lib lib/nxdk_usb.lib lib/libxboxrt.lib
lib/libzlib.lib lib/libpng.lib lib/libjpeg.lib lib/libnxdk_net.lib lib/libSDL2.lib
lib/libSDL2_image.lib lib/libSDL_ttf.lib lib/libfreetype.lib lib/winmm.lib lib/libc++.lib"

cd "$NXDK" || exit 1
eval "$(./bin/activate -s)" || { echo "xbox-build-nxdk: nxdk's bin/activate failed" >&2; exit 1; }

START="$NXDK/lib/.r3d-nxdk-build-start"
rm -f "$STAMP"
: > "$START"
sleep 1 # a library rebuilt in this second must still be newer than the marker

echo "xbox-build-nxdk: make clean, then make NXDK_ONLY=y CFLAGS=$LEVEL CXXFLAGS=$LEVEL -j$JOBS" >&2
make NXDK_ONLY=y clean >/dev/null || { echo "xbox-build-nxdk: make clean failed" >&2; exit 1; }
if ! make NXDK_ONLY=y CFLAGS="$LEVEL" CXXFLAGS="$LEVEL" -j"$JOBS"; then
	echo "xbox-build-nxdk: make failed at $LEVEL - no stamp written; the libraries are incomplete." >&2
	echo "  Rebuild at the old level, or report: a level that needs an nxdk source patch is a fork decision." >&2
	exit 1
fi

stale=0
for l in $LIBS; do
	if [ ! -f "$l" ] || [ ! "$l" -nt "$START" ]; then
		echo "xbox-build-nxdk: $l was not rebuilt by this run" >&2
		stale=1
	fi
done
rm -f "$START"
[ "$stale" -eq 0 ] || { echo "xbox-build-nxdk: no stamp written." >&2; exit 1; }

{
	echo "opt=$LEVEL"
	echo "nxdk=$(git -C "$NXDK" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "clang=$(clang --version 2>/dev/null | head -n 1)"
	echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "command=make NXDK_ONLY=y CFLAGS=$LEVEL CXXFLAGS=$LEVEL"
} > "$STAMP"
echo "xbox-build-nxdk: nxdk libraries built at $LEVEL; stamp $STAMP" >&2
exit 0
