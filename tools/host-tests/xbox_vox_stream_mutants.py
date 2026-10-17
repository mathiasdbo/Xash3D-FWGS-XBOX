"""Mutation check for tools/host-tests/xbox_vox_stream_host_test.c
(divergence #177): each mutant below is one plausible mistake in the
dialogue stream (engine/platform/xbox/xbox_vox_stream.{c,h}) or in its
hooks in upstream's sound code (s_load.c, s_main.c, s_vox.c) - the
loader's sign step, the unit arithmetic, the window's slide, a read used
before it lands, a buffer freed while a read writes into it, the end trim
read from the wrong bytes, the restore's seek, the placeholder's lifetime,
the `mark` truncation - and the host test must fail on every one of them,
by its own failure lines ("FAIL", or the undefined-behaviour trap), not
only a non-zero exit (docs/r3d/lessons.md). A mutant the test passes is a
gap in the test, reported, and exit 1.

    python tools/host-tests/xbox_vox_stream_mutants.py <out_dir>

run.sh sets HOST_CFLAGS and VOX_BASH (its own shell) in the environment.
(Vox_TakePrefetch's checks have no mutant: a prefetch is issued for the
word at its index in the channel's own words, which never change, so
dropping one of its sfx, index or words tests adopts nothing it would not
have - an equivalent mutant. Nor does r3d_vox_crc's reclaim of orphaned
slots between sentences: in the host's disk model a sentence ends with no
read in flight, so it never has one to reclaim.) Each mutant runs the
test's quick pass first (every 37th sentence and all
the scenarios); one that survives it gets the full pass before it counts
as a gap. Every pattern must occur exactly once in its file, so a
refactor that moves one fails here loudly instead of turning the mutant
into a silent copy of the original.
"""
import os
import re
import shutil
import subprocess
import sys

VOX = "engine/platform/xbox/xbox_vox_stream.c"
VOXH = "engine/platform/xbox/xbox_vox_stream.h"
LOAD = "engine/client/sound/s_load.c"
MAIN = "engine/client/sound/s_main.c"
SVOX = "engine/client/sound/s_vox.c"

MUTANTS = [
    ("the loader's sign step dropped (no XOR 0x80)", VOX,
     b"\t\tp[i] ^= 0x80;", b"\t\tp[i] ^= 0x00;"),
    ("the sign step from one byte past the data's start", VOX,
     b"lo = Q_max( b->off, s->hdr->data_off );", b"lo = Q_max( b->off, s->hdr->data_off + 1 );"),
    ("units read from one unit further on", VOX,
     b"\t\t\tVox_Issue( s, j, k * s->unit, Vox_UnitBytes( s, k ), true );", b"\t\t\tVox_Issue( s, j, ( k + 1 ) * s->unit, Vox_UnitBytes( s, k ), true );"),
    ("the window slid only when a fourth unit is needed (nothing queued ahead)", VOX,
     b"\tif( ka > s->k0 && ka < s->k0 + R3D_VOX_UNITS )\n\t{", b"\tif( ka > s->k0 && ka < s->k0 + R3D_VOX_UNITS && kb >= s->k0 + R3D_VOX_UNITS )\n\t{"),
    ("a slide moves the bytes but not their states", VOX,
     b"\tmemmove( &s->b[0], &s->b[d], ( R3D_VOX_UNITS - d ) * sizeof( vox_buf_t ));\n", b""),
    ("a queued unit used before it lands", VOX,
     b"\t\tif( b->state == BUF_PENDING )\n\t\t\tVox_Land( s, true );", b"\t\tif( b->state == BUF_PENDING )\n\t\t\tb->state = BUF_READY, b->valid = s->unit, b->conv = true, s->pending = -1;"),
    ("a slot's memory freed while a read still writes into it", VOX,
     b"\t\tif( s->state == VOX_ORPHAN )\n\t\t{\n\t\t\tVox_Land( s, true ); /* the buffer is the kernel's until it lands */\n\t\t\tVox_CloseIo( s );\n\t\t\ts->state = VOX_FREE;\n\t\t}",
     b"\t\tif( s->state == VOX_ORPHAN )\n\t\t\ts->state = VOX_FREE;"),
    ("a stopped sound's slot freed with its read still in flight", VOX,
     b"\t\ts->state = VOX_ORPHAN;\n\t\treturn;", b"\t\ts->state = VOX_FREE;\n\t\treturn;"),
    ("the end trim read from the head unit", VOX,
     b"\t\t\tp = s->tail + ( b0 - t->off );\n\t\telse if( dir < 0", b"\t\t\tp = s->buf + ( b0 - t->off );\n\t\telse if( dir < 0"),
    ("only a request's first sample made present (the mixer's and the mouth's ranges not covered)", VOX,
     b"\t\tif(( p = Vox_Window( s, b0, b0 + (uint32_t)num_samples )) != NULL )", b"\t\tif(( p = Vox_Window( s, b0, b0 + 1 )) != NULL )"),
    ("a loop's jump back copies one unit of two", VOX,
     b"\tmemcpy( s->buf, s->loop, 2 * s->unit );", b"\tmemcpy( s->buf, s->loop, s->unit );"),
    ("the LIST mark's truncation ignored", VOX,
     b"\t\t\t\tnsamples = loopstart + Vox_SrcLong( s, s->ptr + 24 ); /* samples in loop */", b"\t\t\t\t(void)0;"),
    ("a reused channel's slot never released (the memset leak)", VOX,
     b"\t\t\t\tVox_SlotRelease( s );\n\t\t\t}\n\t\t\telse Vox_Pump( s );", b"\t\t\t}\n\t\t\telse Vox_Pump( s );"),
    ("SND_GetChannelTimeLeft's later words left without their placeholder (FL_VOXWORD_IN_CACHE differs)", VOX,
     b"\t\tVox_Attach( h );\n\t\treturn HDR_WAV( h );", b"\t\treturn HDR_WAV( h );"),
    ("the mixer's reload of a freed cache not mirrored", VOX,
     b"\t\t\tVox_Attach( s->hdr );\n\t\treturn ch->data;", b"\t\treturn ch->data;"),
    ("the channel mixes from sfx->cache, never its window", VOXH,
     b"\treturn ch->engine_reserved[0] ? ch->data : ch->sfx->cache;", b"\treturn ch->sfx->cache;"),
    ("r3d_vox_crc: the stream's pass building no words says SAME (the old `return mode == 0 ? false : same;`)", VOX,
     b"\t\t\tst->load_fail++;\n\t\t\tst->differ++;\n\t\t\treturn false;\n\t\t}\n\t\t/* a word the other mode loaded",
     b"\t\t\treturn true;\n\t\t}\n\t\t/* a word the other mode loaded"),
    ("r3d_vox_crc: a word only one pass loaded not counted as a load failure", VOX,
     b"\t\tif( a->stop >= 0 && ( b->stop < 0 || a->stop < b->stop ))", b"\t\tif( 0 )"),
    ("r3d_vox_crc: a skipped sentence still says SAME", VOX,
     b"\tif( st->skipped || !st->sentences )\n\t\treturn \"INCOMPLETE\";", b"\tif( 0 )\n\t\treturn \"INCOMPLETE\";"),
    ("S_LoadSound returns a placeholder as samples", LOAD,
     b"\t\tif( !S_R3D_VoxIsHeader( sfx->cache )) // #177: a streamed sound's placeholder loads whole below\n", b""),
    ("the restore's seek dropped", MAIN,
     b"\tS_R3D_VoxSeek( target_chan ); // #177", b"\t// S_R3D_VoxSeek( target_chan ); // #177"),
    ("a word's placeholder freed as if it held samples", SVOX,
     b"\tif( S_R3D_VoxIsHeader( word->sfx->cache ))\n\t\tS_R3D_VoxDetach( word->sfx ); // #177: a placeholder holds no samples to free\n\telse\n",
     b"\tif( 0 )\n\t\tS_R3D_VoxDetach( word->sfx );\n\telse\n"),
]

CAUGHT = re.compile(rb"^(FAIL |xbox_vox_stream: exception )", re.M)


def run(out, mut, quick):
    bash = os.environ.get("VOX_BASH", "bash")
    cmd = [bash, "tools/host-tests/xbox_vox_stream.sh", out, mut] + (["quick"] if quick else [])
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=1800)
    except subprocess.TimeoutExpired:
        return -1, b"timeout"
    return p.returncode, p.stdout + p.stderr


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out, exist_ok=True)
    # the unmutated sources first: a test that fails anyway kills every mutant
    os.makedirs(os.path.join(out, "none"))
    rc, text = run(os.path.join(out, "baseline"), os.path.join(out, "none"), True)
    if rc != 0 or CAUGHT.search(text) or b" checks, 0 failures" not in text:
        print("mutants: the unmutated test fails - no mutant can be judged")
        print(text.decode(errors="replace")[-1500:])
        return 1
    gaps = 0
    for i, (name, path, old, new) in enumerate(MUTANTS):
        original = open(path, "rb").read()
        if original.count(old) != 1:
            print(f"mutants: pattern for '{name}' occurs {original.count(old)} times in {path} - update the mutant")
            gaps += 1
            continue
        mut = os.path.join(out, f"m{i}")
        os.makedirs(mut)
        base = os.path.basename(path)
        open(os.path.join(mut, base), "wb").write(original.replace(old, new))
        if path == VOXH:
            # s_mix.c, s_vox.c and s_main.c include it as platform/xbox/...,
            # xbox_vox_stream.c from its own directory: both see the mutant
            os.makedirs(os.path.join(mut, "platform", "xbox"))
            shutil.copy(os.path.join(mut, base), os.path.join(mut, "platform", "xbox", base))
            shutil.copy(VOX, os.path.join(mut, os.path.basename(VOX)))
        build = os.path.join(out, f"b{i}")
        rc, text = run(build, mut, True)
        if rc == 0:
            rc, text = run(build, mut, False)
        if rc != 0 and CAUGHT.search(text):
            first = CAUGHT.search(text)
            line = text[first.start():text.find(b"\n", first.start())].decode(errors="replace")
            print(f"mutants: killed '{name}' ({line[:110]})")
        elif b" checks, " not in text:
            print(f"mutants: '{name}' did not build or run - fix the mutant")
            print(text.decode(errors="replace")[-1500:])
            gaps += 1
        else:
            print(f"mutants: SURVIVED '{name}' - the host test does not catch it (exit {rc})")
            gaps += 1
    print(f"mutants: {len(MUTANTS) - gaps} of {len(MUTANTS)} killed")
    return 1 if gaps else 0


if __name__ == "__main__":
    sys.exit(main())
