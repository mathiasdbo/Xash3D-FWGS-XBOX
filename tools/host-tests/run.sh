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
# The same with x87 floats (-mfpmath=387): the splitter's floor/ceil
# replacement (#159) must match the libm form under both arithmetics.
nv2a_efx_util_x87() {
	gcc $HOST_CFLAGS -mfpmath=387 -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_efx_util_host_test.c ref/nv2a/nv2a_efx_util.c \
		-lm -o "$OUT/nv2a_efx_util_test_x87" && "$OUT/nv2a_efx_util_test_x87"
}
run_test nv2a_efx_util_x87 nv2a_efx_util_x87

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

# tools/host-tests/nv2a_warp_util_host_test.c, header.
nv2a_warp_util() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a -Iengine 		tools/host-tests/nv2a_warp_util_host_test.c ref/nv2a/nv2a_warp_util.c 		-lm -o "$OUT/nv2a_warp_util_test" && "$OUT/nv2a_warp_util_test"
}
run_test nv2a_warp_util nv2a_warp_util

# tools/host-tests/nv2a_decal_util_host_test.c, header.
nv2a_decal_util() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a 		tools/host-tests/nv2a_decal_util_host_test.c ref/nv2a/nv2a_decal_util.c 		-lm -o "$OUT/nv2a_decal_util_test" && "$OUT/nv2a_decal_util_test"
}
run_test nv2a_decal_util nv2a_decal_util

# tools/host-tests/printf_host_test.c, header.
# X9-X item 14 step 13: the frame-time recorder's ring, statistics and file
# encoders; the C test also writes a recording through those encoders,
# which the report tool's own test then parses.
nv2a_studio_gpu() {
	# Every retail model is checked against the TriAPI assembler when the
	# staged disc tree is present (it is gitignored); the synthetic cases
	# run regardless.
	local models=""
	[ -d .sweep-xiso/hl/valve/models ] && models=$(ls .sweep-xiso/hl/valve/models/*.mdl)
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a 		tools/host-tests/nv2a_studio_gpu_host_test.c ref/nv2a/nv2a_studio_gpu_util.c ref/nv2a/nv2a_tri_util.c 		-lm -o "$OUT/nv2a_studio_gpu_test" && "$OUT/nv2a_studio_gpu_test" $models | grep -v "^  .*submodels,"
	return "${PIPESTATUS[0]}"
}
run_test nv2a_studio_gpu nv2a_studio_gpu

xbox_frametime_util() {
	# Stale fixtures from an earlier run must not stand in for ones this
	# run failed to write.
	rm -f "$OUT"/frametime_fixture*.bin
	rm -rf "$OUT/frametime_sessions" && mkdir -p "$OUT/frametime_sessions" || return 1
	gcc $HOST_CFLAGS -Wall -std=c99 -Iengine/platform/xbox \
		tools/host-tests/xbox_frametime_util_host_test.c engine/platform/xbox/xbox_frametime_util.c \
		-o "$OUT/xbox_frametime_util_test" && "$OUT/xbox_frametime_util_test" "$OUT/frametime_fixture.bin" "$OUT/frametime_fixture_v1.bin" \
		"$OUT/frametime_fixture_lc.bin" "$OUT/frametime_fixture_lc_v1.bin" "$OUT/frametime_fixture_spans.bin" \
		"$OUT/frametime_sessions" "$OUT/frametime_fixture_single.bin"
}
run_test xbox_frametime_util xbox_frametime_util

frametime_report() {
	for f in frametime_fixture.bin frametime_fixture_v1.bin frametime_fixture_lc.bin frametime_fixture_lc_v1.bin frametime_fixture_spans.bin \
		frametime_fixture_single.bin; do
		[ -f "$OUT/$f" ] || { echo "host-tests: no fixture $f - the C test above did not write it" >&2; return 1; }
	done
	FRAMETIME_FIXTURE="$OUT/frametime_fixture.bin" FRAMETIME_FIXTURE_V1="$OUT/frametime_fixture_v1.bin" \
	FRAMETIME_FIXTURE_LC="$OUT/frametime_fixture_lc.bin" FRAMETIME_FIXTURE_LC_V1="$OUT/frametime_fixture_lc_v1.bin" \
	FRAMETIME_FIXTURE_SPANS="$OUT/frametime_fixture_spans.bin" \
	FRAMETIME_SESSIONS="$OUT/frametime_sessions" FRAMETIME_FIXTURE_SINGLE="$OUT/frametime_fixture_single.bin" \
		python tools/test_frametime_report.py
}
run_test frametime_report frametime_report

# tools/host-tests/xbox_bitrun_host_test.c, header (divergence #155): a run
# of zero change bits read at once, against upstream's MSG_ReadOneBit.
xbox_bitrun() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iengine/platform/xbox \
		tools/host-tests/xbox_bitrun_host_test.c \
		-o "$OUT/xbox_bitrun_test" && "$OUT/xbox_bitrun_test"
}
run_test xbox_bitrun xbox_bitrun

# tools/host-tests/xbox_audio_hal_util_host_test.c, header (divergence
# #150): the AC97 buffer accounting against nxdk's, through underruns,
# under both controller behaviours; and the stall watchdog's decision.
xbox_audio_hal_util() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iengine/platform/xbox \
		tools/host-tests/xbox_audio_hal_util_host_test.c engine/platform/xbox/xbox_audio_hal_util.c \
		-o "$OUT/xbox_audio_hal_util_test" && "$OUT/xbox_audio_hal_util_test"
}
run_test xbox_audio_hal_util xbox_audio_hal_util

# tools/host-tests/nv2a_shot_util_host_test.c, header (divergence #153):
# the savegame thumbnail's packed 2x2 average against the per-channel loop.
nv2a_shot_util() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a \
		tools/host-tests/nv2a_shot_util_host_test.c \
		-o "$OUT/nv2a_shot_util_test" && "$OUT/nv2a_shot_util_test"
}
run_test nv2a_shot_util nv2a_shot_util

# tools/host-tests/xbox_save_stream_host_test.c, header (divergence #153):
# the deferred save's .sav bytes, chunk by chunk, against upstream's
# straight concatenation; aligned reads, open limits, short reads.
xbox_save_stream() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iengine/platform/xbox 		tools/host-tests/xbox_save_stream_host_test.c engine/platform/xbox/xbox_save_stream.c 		-o "$OUT/xbox_save_stream_test" && "$OUT/xbox_save_stream_test"
}
run_test xbox_save_stream xbox_save_stream

# tools/host-tests/s_mix_host_test.c, header (divergence #145): the real
# engine/client/sound/s_mix.c built with XASH_XBOX 0 and 1 against a stub
# engine (tools/host-tests/s_mix_stub), and the two builds' pitched mixer
# loops compared on the same calls; once with SSE2 doubles, once with x87
# ones (-mfpmath=387, the Pentium III's arithmetic).
s_mix() {
	local fp="$1" tag="$2"
	local inc="-Itools/host-tests/s_mix_stub -Iengine/client/sound -Iengine"
	gcc $HOST_CFLAGS $fp -Wall -std=gnu99 -Wno-unused-function $inc -DXASH_XBOX=0 \
		-DS_MIX_TEST_ENTRY=mix_upstream -DS_PaintChannels=S_PaintChannels_up -DS_ClearBuffers=S_ClearBuffers_up \
		-c tools/host-tests/s_mix_host_test_tu.c -o "$OUT/s_mix_up_$tag.o" &&
	gcc $HOST_CFLAGS $fp -Wall -std=gnu99 -Wno-unused-function $inc -DXASH_XBOX=1 -DS_MIX_TEST_ENTRY=mix_xbox \
		-c tools/host-tests/s_mix_host_test_tu.c -o "$OUT/s_mix_xbox_$tag.o" &&
	gcc $HOST_CFLAGS $fp -Wall -std=gnu99 $inc tools/host-tests/s_mix_host_test.c \
		"$OUT/s_mix_up_$tag.o" "$OUT/s_mix_xbox_$tag.o" -lm -o "$OUT/s_mix_test_$tag" && "$OUT/s_mix_test_$tag"
}
run_test s_mix_sse2 s_mix "" sse2
run_test s_mix_x87 s_mix "-mfpmath=387" x87

printf_test() {
	gcc $HOST_CFLAGS -Wall -Wno-format -fstack-protector-all \
		-Dvsnprintf=xb_vsnprintf -Dsnprintf=xb_snprintf -Dvsprintf=xb_vsprintf \
		-Dsprintf=xb_sprintf -D_vsnprintf=xb__vsnprintf -D_snprintf=xb__snprintf \
		-c compat/xbox/xbox_printf.c -o "$OUT/xp.o" &&
	gcc $HOST_CFLAGS -D__USE_MINGW_ANSI_STDIO=1 -fstack-protector-all \
		tools/host-tests/printf_host_test.c "$OUT/xp.o" -o "$OUT/printf_test" && "$OUT/printf_test"
}
run_test printf printf_test

# tools/host-tests/string_fast_host_test.c, header (divergence #142): the
# Xbox's memcpy/memmove/memset/memcmp/strlen against the host C library.
# The implementation is compiled as the Xbox compiles it (-ffreestanding
# -fno-builtin) so neither compiler can turn its loops back into calls to
# the library it is checked against; once with gcc and once with clang,
# the Xbox's own compiler family.
string_fast() {
	local cc="$1"
	local extra=""
	[ "$cc" = gcc ] && extra="-fno-tree-loop-distribute-patterns"
	$cc $HOST_CFLAGS -Wall -std=c99 -ffreestanding -fno-builtin $extra -DR3D_STRING_FAST_HOST_TEST 		-c compat/xbox/xbox_string_fast.c -o "$OUT/sf_$cc.o" &&
	$cc $HOST_CFLAGS -Wall -std=c99 tools/host-tests/string_fast_host_test.c "$OUT/sf_$cc.o" 		-o "$OUT/string_fast_test_$cc" && "$OUT/string_fast_test_$cc"
}
run_test string_fast_gcc string_fast gcc
run_test string_fast_clang string_fast clang

# tools/host-tests/mainui_atlas_tga_host_test.cpp, header (divergence #152):
# the font cache's TGA atlas decodes to the texels and flags the BMP did.
mainui_atlas_tga() {
	g++ $HOST_CFLAGS -Wall -std=c++11 -I3rdparty/mainui/font \
		tools/host-tests/mainui_atlas_tga_host_test.cpp \
		-o "$OUT/mainui_atlas_tga_test" && "$OUT/mainui_atlas_tga_test"
}
run_test mainui_atlas_tga mainui_atlas_tga

# tools/host-tests/nv2a_atlas_util_host_test.cpp, header (divergence #157):
# ref/nv2a's straight-from-TGA atlas upload against the decode-and-pack path.
nv2a_atlas_util() {
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a \
		-c ref/nv2a/nv2a_atlas_util.c -o "$OUT/nv2a_atlas_util.o" &&
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a \
		-c ref/nv2a/nv2a_2d_util.c -o "$OUT/nv2a_2d_util_atlas.o" &&
	gcc $HOST_CFLAGS -Wall -std=c99 -Iref/nv2a \
		-c ref/nv2a/nv2a_tri_util.c -o "$OUT/nv2a_tri_util_atlas.o" &&
	g++ $HOST_CFLAGS -Wall -std=c++11 -Iref/nv2a -I3rdparty/mainui/font \
		tools/host-tests/nv2a_atlas_util_host_test.cpp \
		"$OUT/nv2a_atlas_util.o" "$OUT/nv2a_2d_util_atlas.o" "$OUT/nv2a_tri_util_atlas.o" \
		-o "$OUT/nv2a_atlas_util_test" && "$OUT/nv2a_atlas_util_test"
}
run_test nv2a_atlas_util nv2a_atlas_util

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
