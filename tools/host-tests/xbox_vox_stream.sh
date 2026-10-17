#!/usr/bin/env bash
# Build and run tools/host-tests/xbox_vox_stream_host_test.c (divergence
# #177): the engine's real sound code twice in one binary - upstream's and
# the Xbox build's with the dialogue stream - compared over every retail
# sentence. run.sh calls it; xbox_vox_stream_mutants.py calls it with a
# directory of mutated sources that comes first on the include path.
#
#   tools/host-tests/xbox_vox_stream.sh <out dir> [<mutant source dir>] [quick]
#
# From the repository root, in the MSYS2 MinGW-64 shell, with the staged
# retail tree (.sweep-xiso/hl/valve, tools/xbox-xiso.sh). HOST_CFLAGS comes
# from run.sh (the Xbox build's -O2, with undefined-behaviour traps).
# Exit 0 iff it built and every check passed.
set -u
out="$1"
mut="${2:-}"
quick="${3:-}"
root=.sweep-xiso/hl/valve
if [ ! -f "$root/sound/sentences.txt" ]; then
	echo "xbox_vox_stream: no retail sound tree at $root/sound (stage it with tools/xbox-xiso.sh)" >&2
	exit 1
fi
rm -rf "$out" && mkdir -p "$out/synth" || exit 1

# s_main.c's functions the sentences, the channels and save/restore run
# through; the rest of the file pulls in the whole engine
main_src=engine/client/sound/s_main.c
[ -n "$mut" ] && [ -f "$mut/s_main.c" ] && main_src="$mut/s_main.c"
python tools/host-tests/extract_c_functions.py "$main_src" "$out/s_main_part.c" \
	S_AdjustLoopedSamplePosition S_RetrieveAudioSamples S_IsClient S_NotifyChannelUpdate S_FreeChannel \
	SND_FStreamIsPlaying SND_GetChannelTimeLeft SND_PickDynamicChannel SND_PickStaticChannel \
	S_MaybeAlterChannel S_AlterChannel S_StartSound S_RestoreSound S_GetCurrentStaticSounds \
	S_GetCurrentDynamicSounds S_InitAmbientChannels S_StopAllSounds || exit 1

inc="-I$out -Itools/host-tests/vox_stub -Iengine/client/sound -Iengine/client -Iengine/client/soundlib"
inc="$inc -Iengine/common/soundlib -Iengine/common -Iengine/platform/xbox -Iengine -Icommon -Ipublic -I3rdparty/library_suffix/include"
[ -n "$mut" ] && inc="-I$mut $inc"
flags="${HOST_CFLAGS:-} -std=gnu99 -Wall -Wno-unused-function -DENGINE_DLL=1"

gcc $flags $inc -DVOX_TEST_XBOX=0 -c tools/host-tests/xbox_vox_stream_host_test_tu.c -o "$out/up.o" &&
gcc $flags $inc -DVOX_TEST_XBOX=1 -c tools/host-tests/xbox_vox_stream_host_test_tu.c -o "$out/xb.o" &&
gcc $flags $inc -c tools/host-tests/xbox_vox_stream_host_test.c -o "$out/main.o" &&
gcc $flags $inc -Wno-address -c public/crtlib.c -o "$out/crtlib.o" &&
gcc $flags $inc -c public/crclib.c -o "$out/crclib.o" &&
gcc "$out/up.o" "$out/xb.o" "$out/main.o" "$out/crtlib.o" "$out/crclib.o" -lm -o "$out/xbox_vox_stream_test" || exit 1
"$out/xbox_vox_stream_test" "$root" "$out/synth" $quick
