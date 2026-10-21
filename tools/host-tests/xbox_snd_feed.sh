#!/usr/bin/env bash
# Build and run tools/host-tests/xbox_snd_feed_host_test.c (divergence
# #184): engine/platform/xbox/xbox_snd_feed.c on the host against
# nxdk-ex's real OpenAL (its null backend), built here as run_tests.py
# builds it (clang, -std=c99). run.sh calls it.
#
#   tools/host-tests/xbox_snd_feed.sh <out dir>
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
# each unit to an object first: a clang call with several sources needs a
# temp directory, which MSYS2's bash does not always pass on
inc="-I tools/host-tests/voices_stub -I engine -I $al/include -I $al/src"
clang -c -std=gnu99 -Wall -Werror -Wno-unused-function $inc tools/host-tests/xbox_snd_feed_host_test.c -o "$out/test.o" || exit 1
clang -c -std=gnu99 -Wall -Werror -Wno-unused-function $inc engine/platform/xbox/xbox_snd_feed.c -o "$out/feed.o" || exit 1
clang "$out/test.o" "$out/feed.o" "$out"/al/*.o -o "$out/xbox_snd_feed_test" || exit 1
"$out/xbox_snd_feed_test"
