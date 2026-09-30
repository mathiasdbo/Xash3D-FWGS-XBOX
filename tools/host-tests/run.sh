#!/usr/bin/env bash
# Run every host test under tools/host-tests/ with the command each test's
# own header documents, from the repository root, in the MSYS2 MinGW-64
# shell (tools/xbox-env.sh, "Which shell"). One entry point so a gate can
# require "the host tests" without a hand-maintained list of compile lines
# living anywhere but next to the test it belongs to.
#
#   tools/host-tests/run.sh
#
# Invoked bare, not through /c/msys64/usr/bin/bash: docs/r3d/lessons.md
# records that boundary dropping exported variables; the PATH pin below is
# what makes MinGW-64's gcc resolve.
#
# Exit 0 iff every test compiled and exited 0. Any compile or test failure
# exits 1, after running the remaining tests, so one run reports all of
# them. When a test is added here, add its documented command below and
# nothing else - the commands are copied verbatim from each file's header,
# which stays the source of truth.
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO" || exit 1
# Same PATH pin every tools/xbox-*.sh uses, so a bare invocation finds
# MinGW-64's gcc; a no-op where MSYS2 is not installed (a Linux host).
[ -d /c/msys64/mingw64/bin ] && export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
OUT="${SCRATCH:-$REPO/.sweep}/host-tests"
mkdir -p "$OUT" || exit 1

# The code under test is built as the Xbox build builds it (-O2,
# tools/xbox-opt.sh) with undefined-behaviour checks that trap instead of
# calling a runtime library (MinGW has none): an optimiser exploits UB the
# -O0 build never did, and a trap here (exit 132 / SIGILL) is where it
# shows first. It caught xbox_printf.c converting an out-of-range double
# to int when the Xbox build moved off -O0.
HOST_CFLAGS="-O2 -fsanitize=undefined -fsanitize=float-cast-overflow -fsanitize-undefined-trap-on-error"

fail=0
ran=0

run_test() {
	local name="$1"; shift
	ran=$((ran + 1))
	echo "host-tests: $name" >&2
	if "$@"; then
		echo "host-tests:   $name ok" >&2
	else
		echo "host-tests:   $name FAILED (exit $?)" >&2
		fail=$((fail + 1))
	fi
}

# tools/host-tests/nv2a_arena_host_test.c, header.
nv2a_arena() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_arena_host_test.c ref/nv2a/nv2a_arena.c ref/nv2a/nv2a_2d_util.c ref/nv2a/nv2a_tri_util.c ref/nv2a/nv2a_studio_util.c \
		-lm -o "$OUT/nv2a_arena_test" && "$OUT/nv2a_arena_test"
}
run_test nv2a_arena nv2a_arena

# tools/host-tests/nv2a_skin_cache_host_test.c, header.
nv2a_skin_cache() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_skin_cache_host_test.c ref/nv2a/nv2a_skin_cache.c ref/nv2a/nv2a_arena.c \
		-o "$OUT/nv2a_skin_cache_test" && "$OUT/nv2a_skin_cache_test"
}
run_test nv2a_skin_cache nv2a_skin_cache

# tools/host-tests/nv2a_sprite_util_host_test.c, header.
nv2a_sprite_util() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_sprite_util_host_test.c ref/nv2a/nv2a_sprite_util.c \
		-o "$OUT/nv2a_sprite_util_test" && "$OUT/nv2a_sprite_util_test"
}
run_test nv2a_sprite_util nv2a_sprite_util

# tools/host-tests/nv2a_efx_util_host_test.c, header.
nv2a_efx_util() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a 		tools/host-tests/nv2a_efx_util_host_test.c ref/nv2a/nv2a_efx_util.c 		-lm -o "$OUT/nv2a_efx_util_test" && "$OUT/nv2a_efx_util_test"
}
run_test nv2a_efx_util nv2a_efx_util

# tools/host-tests/nv2a_lightmap_util_host_test.c, header.
nv2a_lightmap_util() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_lightmap_util_host_test.c ref/nv2a/nv2a_lightmap_util.c \
		-o "$OUT/nv2a_lightmap_util_test" && "$OUT/nv2a_lightmap_util_test"
}
run_test nv2a_lightmap_util nv2a_lightmap_util

# tools/host-tests/nv2a_texanim_util_host_test.c, header.
nv2a_texanim_util() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_texanim_util_host_test.c ref/nv2a/nv2a_texanim_util.c \
		-o "$OUT/nv2a_texanim_util_test" && "$OUT/nv2a_texanim_util_test"
}
run_test nv2a_texanim_util nv2a_texanim_util

# tools/host-tests/nv2a_sky_util_host_test.c, header.
nv2a_sky_util() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_sky_util_host_test.c ref/nv2a/nv2a_sky_util.c \
		-lm -o "$OUT/nv2a_sky_util_test" && "$OUT/nv2a_sky_util_test"
}
run_test nv2a_sky_util nv2a_sky_util

# tools/host-tests/printf_host_test.c, header.
printf_test() {
	gcc $HOST_CFLAGS -Wall -Wno-format -fstack-protector-all \
		-Dvsnprintf=xb_vsnprintf -Dsnprintf=xb_snprintf -Dvsprintf=xb_vsprintf \
		-Dsprintf=xb_sprintf -D_vsnprintf=xb__vsnprintf -D_snprintf=xb__snprintf \
		-c compat/xbox/xbox_printf.c -o "$OUT/xp.o" &&
	gcc $HOST_CFLAGS -D__USE_MINGW_ANSI_STDIO=1 -fstack-protector-all \
		tools/host-tests/printf_host_test.c "$OUT/xp.o" -o "$OUT/printf_test" && "$OUT/printf_test"
}
run_test printf printf_test

if [ "$ran" -eq 0 ]; then
	echo "host-tests: ran zero tests - that is a broken script, not a pass" >&2
	exit 1
fi
if [ "$fail" -ne 0 ]; then
	echo "host-tests: $fail of $ran test(s) failed" >&2
	exit 1
fi
echo "host-tests: $ran of $ran test(s) passed" >&2
exit 0
