"""Mutation check for tools/host-tests/xbox_music_adpcm_host_test.c
(divergence #171): each mutant below is one plausible mistake in
engine/platform/xbox/xbox_music_adpcm.c - a decode table bit, a clamp, the
APU's 64-frame block, the stereo interleave, upstream's dropped tail, the
mp3 position quirk, the seek, the parser, the rate conversion, the CRC,
which file a request names -
and the host test must fail on every one of them. A mutant the test
passes is a gap in the test, reported and exit 1.

    python tools/host-tests/xbox_music_adpcm_mutants.py <out_dir> <gcc flags...>

run.sh passes its HOST_CFLAGS. (SplitName's separator rule has no mutant: text after a dot that holds
a separator can never equal a format name, so dropping the rule changes
no result - an equivalent mutant.) Every pattern must occur exactly once in the
source, so a refactor that moves one fails here loudly instead of turning
the mutant into a silent copy of the original.
"""
import os
import subprocess
import sys

SRC = "engine/platform/xbox/xbox_music_adpcm.c"
TEST = "tools/host-tests/xbox_music_adpcm_host_test.c"
ENC = "tools/asset-studio/src/formats/music_adpcm_encode.c"

MUTANTS = [
    ("table: the 1-bit adds step/2", b"if( c & 1 ) d += step >> 2;", b"if( c & 1 ) d += step >> 1;"),
    ("table: index clamps at 87", b"n > XBOX_ADPCM_MAX_INDEX ? XBOX_ADPCM_MAX_INDEX : n", b"n > XBOX_ADPCM_MAX_INDEX ? XBOX_ADPCM_MAX_INDEX - 1 : n"),
    ("decode: sample clamps at 32766", b"if( s_ > 32767 ) s_ = 32767;", b"if( s_ > 32767 ) s_ = 32766;"),
    ("decode: the sign bit ignored", b"-(int32_t)(( c_ >> 3 ) & 1u )", b"-(int32_t)(( c_ >> 4 ) & 1u )"),
    ("decode: header sample not frame 0", b"o[0] = (int16_t)sample;", b"o[0] = 0;"),
    ("decode: a different code skipped", b"if( group == 7 && k == 3 )", b"if( group == 6 && k == 3 )"),
    ("decode: no stereo interleave", b"codes += 4 * channels )", b"codes += 4 )"),
    ("decode: a bad header accepted", b"if( h[2] > XBOX_ADPCM_MAX_INDEX || h[3] != 0 )", b"if( h[2] > XBOX_ADPCM_MAX_INDEX )"),
    ("stream: the short last chunk played (WAV semantics, not upstream's mp3)", b"return got == chunk_frames;", b"return got > 0;"),
    ("stream: the chunk offset not reset", b"\t\ts->buffsize = 0; /* no bytes remaining */", b"\t\t/* mutant */"),
    ("stream: libmpg's last-chunk position after a seek too", b"if( s->info.tell_last_chunk && !s->seeked && !s->ended )", b"if( s->info.tell_last_chunk && !s->ended )"),
    ("stream: libmpg's last-chunk position never used", b"return (int)Xbox_MusicXwav_ToSourceFrames( &s->info, s->info.tell_last_chunk );", b"return (int)Xbox_MusicXwav_ToSourceFrames( &s->info, s->next_frame );"),
    ("seek: the chunk not flushed", b"\ts->next_frame = f;\n\ts->buffsize = 0;\n", b"\ts->next_frame = f;\n"),
    ("seek: past the end clamped", b"\ts->next_frame = f;\n\ts->buffsize = 0;", b"\ts->next_frame = f > s->info.frames ? s->info.frames : f;\n\ts->buffsize = 0;"),
    ("parser: a bake at another rate accepted", b"if( out->rate != out->source_rate || out->source_frames != out->frames )", b"if( out->source_frames != out->frames && out->rate == out->source_rate )"),
    ("discovery: the extension compared case-sensitively", b"\t\tif( LowerEq( dot + 1, formats[i] ))", b"\t\tif( !strcmp( dot + 1, formats[i] ))"),
    ("discovery: no extension takes the last format present, not the first", b"\t\t\tif( probe( ctx, path, &size ))\n\t\t\t{\n\t\t\t\tfound = i;\n\t\t\t\tbreak;", b"\t\t\tif( probe( ctx, path, &size ))\n\t\t\t{\n\t\t\t\tfound = i;\n\t\t\t\tcontinue;"),
    ("discovery: the size not checked", b"\tif( size != (int64_t)info->source_bytes )\n\t\treturn XBOX_XWAV_SOURCE_SIZE;", b""),
    ("discovery: the format not checked", b"\tif( !LowerEq( formats[found], info->source_ext ))\n\t\treturn XBOX_XWAV_SOURCE_FORMAT;", b""),
    ("read-ahead: a short read kept", b"\tif( got < bytes )\n\t{", b"\tif( got < bytes / 2 )\n\t{"),
    ("parser: a short data chunk accepted", b"if( (uint64_t)out->data_bytes != (uint64_t)BlocksFor( out->frames ) * out->block_bytes )", b"if( (uint64_t)out->data_bytes < (uint64_t)BlocksFor( out->frames ) * out->block_bytes )"),
    ("parser: odd chunks not padded", b"off += 8 + (uint64_t)size + ( size & 1u );", b"off += 8 + (uint64_t)size;"),
    ("parser: samples per block not checked", b"rd16( f + 18 ) != XBOX_ADPCM_BLOCK_FRAMES", b"rd16( f + 18 ) == 0"),
    ("rate: file frames rounded down", b"* info->rate + info->source_rate - 1 ) / info->source_rate );", b"* info->rate ) / info->source_rate );"),
    ("crc: wrong polynomial", b"0xEDB88320u", b"0xEDB88321u"),
]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    flags = sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    original = open(SRC, "rb").read()
    gaps = 0
    for i, (name, old, new) in enumerate(MUTANTS):
        if original.count(old) != 1:
            print(f"mutants: pattern for '{name}' occurs {original.count(old)} times in {SRC} - update the mutant")
            gaps += 1
            continue
        src = os.path.join(out, f"music_mutant_{i}.c")
        exe = os.path.join(out, f"music_mutant_{i}.exe")
        open(src, "wb").write(original.replace(old, new))
        cc = ["gcc", *flags, "-w", "-std=c99", "-Iengine/platform/xbox", "-Itools/asset-studio/src/formats",
              TEST, src, ENC, "-lm", "-o", exe]
        if subprocess.run(cc, capture_output=True).returncode != 0:
            print(f"mutants: '{name}' does not compile - fix the mutant")
            gaps += 1
            continue
        try:
            rc = subprocess.run([exe], capture_output=True, timeout=300).returncode
        except subprocess.TimeoutExpired:
            rc = -1  # a hang is a failure the test caught
        if rc == 0:
            print(f"mutants: SURVIVED '{name}' - the host test does not catch it")
            gaps += 1
        else:
            print(f"mutants: killed '{name}' (exit {rc})")
    print(f"mutants: {len(MUTANTS) - gaps} of {len(MUTANTS)} killed")
    return 1 if gaps else 0


if __name__ == "__main__":
    sys.exit(main())
