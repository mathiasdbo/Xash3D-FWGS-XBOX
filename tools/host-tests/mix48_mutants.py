"""Mutation check for the 48 kHz mix (divergence #173): each mutant below
is one plausible mistake in engine/client/sound/s_dsp.c's SX_R3D_SetRate or
in engine/platform/xbox/xbox_s_mix_pitch.h's rounding of an off-grid step,
or in engine/platform/xbox/xbox_snd_clock.h's rebase (xbox_snd_clock_host_test.c),
or in the same header's s_lerping loops (divergence #174),
and the host test that covers it (tools/host-tests/s_dsp_host_test.c, or
s_mix_host_test.c through its two s_mix.c builds) must fail on every one.
A mutant the test passes is a gap in the test, reported and exit 1.

    python tools/host-tests/mix48_mutants.py <out_dir> <gcc flags...>

run.sh passes its HOST_CFLAGS. The mutated file is written under
<out_dir>, ahead of the real one on the include path; the tree is never
modified. Every pattern must occur exactly once in its file, so a refactor
that moves one fails here loudly instead of turning the mutant into a
silent copy of the original.
"""
import os
import subprocess
import sys

DSP = "engine/client/sound/s_dsp.c"
PITCH = "engine/platform/xbox/xbox_s_mix_pitch.h"
CLOCK = "engine/platform/xbox/xbox_snd_clock.h"
HALU = "engine/platform/xbox/xbox_audio_hal_util.c"

MUTANTS = [
    (DSP, "dsp: the base rate left at 11025",
     b"\tidsp_dma_speed = (int)(((int64_t)SOUND_11k * SOUND_DMA_SPEED ) / SOUND_44k );",
     b"\tidsp_dma_speed = SOUND_11k;"),
    (DSP, "dsp: the modulation periods not scaled (upstream's integer division)",
     b"\tsxmod1cur = sxmod1 = (int)(( 350 * (int64_t)idsp_dma_speed ) / SOUND_11k );",
     b"\tsxmod1cur = sxmod1 = 350 * ( idsp_dma_speed / SOUND_11k );"),
    (DSP, "dsp: the lines kept at their old length",
     b"\tDLY_Free( &monodly );\n\tDLY_Free( &reverbdly[0] );\n\tDLY_Free( &reverbdly[1] );\n\tDLY_Free( &stereodly );\n\n\t// SX_ReloadRoomFX's",
     b"\t// SX_ReloadRoomFX's"),
    (DSP, "dsp: the reverb not set up again",
     b"\tSetBits( sxrvb_size.flags, FCVAR_CHANGED );\n}\n#endif",
     b"}\n#endif"),
    (DSP, "dsp: SX_Init leaves the rate to upstream's constant",
     b"\tSX_ReloadRoomFX();\n#if XASH_XBOX\n\tSX_R3D_SetRate(); // the mix rate's delay lengths",
     b"\tSX_ReloadRoomFX();\n#if 0\n\tSX_R3D_SetRate(); // the mix rate's delay lengths"),
    (PITCH, "mix: off-grid steps approximated at 44.1 kHz too",
     b"if(( (double)fi != f || (double)ri != r ) && SOUND_DMA_SPEED == SOUND_44k )",
     b"if( 0 )"),
    (PITCH, "mix: off-grid steps never approximated (the double loop at 48 kHz)",
     b"if(( (double)fi != f || (double)ri != r ) && SOUND_DMA_SPEED == SOUND_44k )",
     b"if(( (double)fi != f || (double)ri != r ))"),
    (PITCH, "mix: the step rounded up (reads past the exact index)",
     b"\tconst int64_t ri = (int64_t)r;",
     b"\tconst int64_t ri = (int64_t)r + ( (double)(int64_t)r != r );"),
    (CLOCK, "clock: the rebase leaves the wrap count (upstream's statics)",
     b"\tc->buffers = t / fullsamples;\n", b""),
    (CLOCK, "clock: the device not moved to the time's place in the ring",
     b"\tconst int samplepos = ( t % fullsamples ) * 2;", b"\tconst int samplepos = 0;"),
    (CLOCK, "clock: the time not converted to the new rate",
     b"\treturn (int)(( (int64_t)t * new_rate ) / old_rate );", b"\treturn t;"),
    (CLOCK, "clock: the chop not reported",
     b"\t\t\t*chop = 1;\n", b""),
    (HALU, "hal: the rings cleared before the engines halt (nxdk's order)",
     b"\tconst unsigned stuck = xbox_aci_reset_engines( io, timeout_us, poll_us );\n\n\tif( stuck )\n\t\treturn stuck; // an engine may still be fetching: leave its ring alone\n\n\tclear_rings( ring_ctx );",
     b"\tunsigned stuck;\n\n\tclear_rings( ring_ctx );\n\tstuck = xbox_aci_reset_engines( io, timeout_us, poll_us );\n\n\tif( stuck )\n\t\treturn stuck;"),
    (HALU, "hal: the rings cleared although an engine would not halt",
     b"\tif( stuck )\n\t\treturn stuck; // an engine may still be fetching: leave its ring alone\n",
     b""),
    (HALU, "hal: RR written without waiting for the halt",
     b"\t\tif( !aci_wait( io, box[i] + XBOX_ACI_REG_SR, XBOX_ACI_SR_DCH, XBOX_ACI_SR_DCH, timeout_us, poll_us ))\n",
     b"\t\tif( 0 )\n"),
    (PITCH, "mix: the position rounded up",
     b"\tconst int64_t fi = (int64_t)f;",
     b"\tconst int64_t fi = (int64_t)f + ( (double)(int64_t)f != f && f < 4294967295.0 );"),
    # s_lerping on the fixed-point counter (divergence #174)
    (PITCH, "lerp: the next sample not read (point sampling again)",
     b"const int s = S_LERP ## x( S_SAMPLE ## x( data[sample_idx] ), S_SAMPLE ## x( data[sample_idx + 1] ), frac );",
     b"const int s = S_LERP ## x( S_SAMPLE ## x( data[sample_idx] ), S_SAMPLE ## x( data[sample_idx] ), frac );"),
    (PITCH, "lerp: the weight complemented (interpolating backwards)",
     b"#define S_LERP_WEIGHT16( frac ) ((int)(( frac ) >> 17 ))",
     b"#define S_LERP_WEIGHT16( frac ) ((int)(( ~( frac )) >> 17 ))"),
    (PITCH, "lerp: 8-bit mixed at upstream's unshifted scale",
     b"#define S_LERP8( a, b, frac )  ((( a ) * 256 ) + ((( (b) - (a) ) * S_LERP_WEIGHT8( frac )) >> 8 ))",
     b"#define S_LERP8( a, b, frac )  (( a ) + ((( (b) - (a) ) * S_LERP_WEIGHT8( frac )) >> 16 ))"),
    (PITCH, "lerp: the stereo right channel read from the left",
     b"const int sr = S_LERP ## x( S_SAMPLE ## x( data[sample_idx + 1] ), S_SAMPLE ## x( data[sample_idx + 3] ), frac );",
     b"const int sr = S_LERP ## x( S_SAMPLE ## x( data[sample_idx + 0] ), S_SAMPLE ## x( data[sample_idx + 2] ), frac );"),
]


def build_and_run(out, flags, i, kind):
    exe = os.path.join(out, f"mix48_mutant_{i}.exe")
    if kind == HALU:
        cc = ["gcc", *flags, "-w", "-std=c99", "-Iengine/platform/xbox",
              "tools/host-tests/xbox_audio_hal_util_host_test.c", os.path.join(out, "xbox_audio_hal_util.c"), "-o", exe]
        if subprocess.run(cc, capture_output=True).returncode != 0:
            return None
    elif kind == CLOCK:
        cc = ["gcc", *flags, "-w", "-std=c99", f"-I{out}/platform/xbox", "-Iengine/platform/xbox",
              "tools/host-tests/xbox_snd_clock_host_test.c", "-o", exe]
        if subprocess.run(cc, capture_output=True).returncode != 0:
            return None
    elif kind == DSP:
        cc = ["gcc", *flags, "-w", "-std=gnu99", "-DXASH_XBOX=1", f"-I{out}", "-Itools/host-tests/s_dsp_stub",
              "-Iengine/client/sound", "tools/host-tests/s_dsp_host_test.c", "-lm", "-o", exe]
        if subprocess.run(cc, capture_output=True).returncode != 0:
            return None
    else:
        inc = [f"-I{out}", "-Itools/host-tests/s_mix_stub", "-Iengine/client/sound", "-Iengine"]
        up = os.path.join(out, f"mix48_up_{i}.o")
        xb = os.path.join(out, f"mix48_xbox_{i}.o")
        steps = [
            ["gcc", *flags, "-w", "-std=gnu99", *inc, "-DXASH_XBOX=0", "-DS_MIX_TEST_ENTRY=mix_upstream",
             "-DS_MIX_TEST_ENTRY_LERP=mix_lerp_upstream",
             "-DS_MIX_TEST_CHANNEL=mix_channel_upstream", "-DS_PaintChannels=S_PaintChannels_up",
             "-DS_ClearBuffers=S_ClearBuffers_up", "-c", "tools/host-tests/s_mix_host_test_tu.c", "-o", up],
            ["gcc", *flags, "-w", "-std=gnu99", *inc, "-DXASH_XBOX=1", "-DS_MIX_TEST_ENTRY=mix_xbox",
             "-DS_MIX_TEST_ENTRY_LERP=mix_lerp_xbox",
             "-DS_MIX_TEST_CHANNEL=mix_channel_xbox", "-c", "tools/host-tests/s_mix_host_test_tu.c", "-o", xb],
            ["gcc", *flags, "-w", "-std=gnu99", *inc, "tools/host-tests/s_mix_host_test.c", up, xb, "-lm", "-o", exe],
        ]
        for cc in steps:
            if subprocess.run(cc, capture_output=True).returncode != 0:
                return None
    try:
        return subprocess.run([exe], capture_output=True, timeout=600).returncode
    except subprocess.TimeoutExpired:
        return -1  # a hang is a failure the test caught


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    flags = sys.argv[2:]
    gaps = 0
    for i, (path, name, old, new) in enumerate(MUTANTS):
        mdir = os.path.join(out, f"m{i}")
        original = open(path, "rb").read()
        if original.count(old) != 1:
            print(f"mutants: pattern for '{name}' occurs {original.count(old)} times in {path} - update the mutant")
            gaps += 1
            continue
        # the mutated file where the include path finds it first
        rel = {PITCH: "platform/xbox/xbox_s_mix_pitch.h", CLOCK: "platform/xbox/xbox_snd_clock.h",
               HALU: "xbox_audio_hal_util.c"}.get(path, "s_dsp.c")
        target = os.path.join(mdir, rel)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        open(target, "wb").write(original.replace(old, new))
        rc = build_and_run(mdir, flags, i, path)
        if rc is None:
            print(f"mutants: '{name}' does not compile - fix the mutant")
            gaps += 1
        elif rc == 0:
            print(f"mutants: SURVIVED '{name}' - the host test does not catch it")
            gaps += 1
        else:
            print(f"mutants: killed '{name}' (exit {rc})")
    print(f"mutants: {len(MUTANTS) - gaps} of {len(MUTANTS)} killed")
    return 1 if gaps else 0


if __name__ == "__main__":
    sys.exit(main())
