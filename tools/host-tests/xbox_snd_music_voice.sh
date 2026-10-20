#!/usr/bin/env bash
# Build and run tools/host-tests/xbox_snd_music_voice_host_test.c
# (divergence #183): engine/platform/xbox/xbox_snd_music_voice.c and the
# baked-music stream (xbox_music_adpcm.c) on the host against nxdk-ex's
# real OpenAL (its null backend), built as xbox_snd_voices.sh builds it.
# run.sh calls it.
#
#   tools/host-tests/xbox_snd_music_voice.sh <out dir>
#
# From the repository root, in the MSYS2 MinGW-64 shell. Exit 0 iff it
# built and every check passed.
set -u
out="$1"
al=3rdparty/nxdk-ex/lib/openal
rm -rf "$out" && mkdir -p "$out/al" || exit 1
for c in "$al"/src/*.c; do
	clang -c -I "$al/include" -I "$al/src" -Wall -Werror -pedantic -std=c99 "$c" -o "$out/al/$(basename "${c%.c}").o" || exit 1
done
# each unit to an object first (xbox_snd_voices.sh says why)
inc="-I tools/host-tests/voices_stub -I engine -I engine/platform/xbox -I $al/include -I $al/src"
clang -c -std=gnu99 -Wall -Werror -Wno-unused-function $inc tools/host-tests/xbox_snd_music_voice_host_test.c -o "$out/test.o" || exit 1
# MV_SRC: another copy of the module (xbox_snd_music_voice_mutants.py)
clang -c -std=gnu99 -Wall -Werror -Wno-unused-function $inc "${MV_SRC:-engine/platform/xbox/xbox_snd_music_voice.c}" -o "$out/music_voice.o" || exit 1
clang -c -std=gnu99 -Wall -Werror $inc engine/platform/xbox/xbox_music_adpcm.c -o "$out/music_adpcm.o" || exit 1
clang "$out/test.o" "$out/music_voice.o" "$out/music_adpcm.o" "$out"/al/*.o -o "$out/xbox_snd_music_voice_test" || exit 1
"$out/xbox_snd_music_voice_test"
