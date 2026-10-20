"""Mutation check for tools/host-tests/xbox_snd_music_voice_host_test.c
(divergence #183): each mutant below is one plausible mistake in
engine/platform/xbox/xbox_snd_music_voice.c, and the host test must fail
on every one of them. A mutant the test passes is a gap in the test,
reported and exit 1.

    python tools/host-tests/xbox_snd_music_voice_mutants.py <out_dir>

Each mutant is built by tools/host-tests/xbox_snd_music_voice.sh with
MV_SRC pointing at it (run.sh passes the shell in MV_BASH). Every pattern
must occur exactly once in the source. (Mv_Queue's reset of the next block
to queue for a new stream in the old one's memory has no mutant: the queue
takes whatever is pending, so the stale count only reaches the gap
statistic and the no-buffer fallback - an equivalent mutant for what
plays.)
"""
import os
import subprocess
import sys

SRC = "engine/platform/xbox/xbox_snd_music_voice.c"

MUTANTS = [
    ("flush: the source left stopped, so a timed start never acts", b"\talSourceRewind( s_src );\n", b""),
    ("raw end: counted from what was read, not from where the APU is", b"rawend = (uint)snd.soundtime + Mv_ToMix( s_q_frames - (uint32_t)played );", b"rawend = s_rawend_set + Mv_ToMix( queued );"),
    ("ring: a buffer runs past the ring's end", b"n = Q_min( n, RING_BLOCKS - slot );", b"(void)slot;"),
    ("raw channel: given the music volume, so the CPU mixes it too", b"\tch->master_vol = 0;\n", b"\tch->master_vol = vol;\n"),
    ("paint: a pause does not stop it", b"if( !paused && ch && ch->s_rawend == s_rawend_set )", b"if( ch && ch->s_rawend == s_rawend_set )"),
    ("start: at the raw end even when the paint time is later", b"Q_max( (int)ch->s_rawend, snd.paintedtime )", b"(int)ch->s_rawend"),
    ("gain: set once, not as the music volume changes", b"if( vol != s_gain )", b"if( s_gain == 0 && vol != s_gain )"),
    ("reap: finished buffers never given back", b"\talGetSourcei( s_src, AL_BUFFERS_PROCESSED, &done );\n", b""),
    ("sink: blocks of another stream kept pending", b"if( ctx != s_owner || block != s_pend_block + ( r_pend - r_queued ))", b"if( block != s_pend_block + ( r_pend - r_queued ))"),
    ("buffers: static (a tail page and a wbinvd each)", b"\t\talBufferData( b, s_format,", b"\t\talBufferDataStatic( b, s_format,"),
    ("off: the stream keeps its sink when the voices go", b"\t\tXbox_MusicStream_SetSink( ms, NULL, NULL );\n\t\ts_n_dropped++;", b"\t\ts_n_dropped++;"),
]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    sh = os.environ.get("MV_BASH", "bash")
    os.makedirs(out, exist_ok=True)
    original = open(SRC, "rb").read()
    gaps = 0
    for i, (name, old, new) in enumerate(MUTANTS):
        if original.count(old) != 1:
            print(f"mutants: pattern for '{name}' occurs {original.count(old)} times in {SRC} - update the mutant")
            gaps += 1
            continue
        src = os.path.join(out, f"mv_mutant_{i}.c")
        open(src, "wb").write(original.replace(old, new))
        env = dict(os.environ, MV_SRC=src)
        try:
            r = subprocess.run([sh, "tools/host-tests/xbox_snd_music_voice.sh", os.path.join(out, f"build_{i}")],
                               env=env, capture_output=True, timeout=600)
            rc, text = r.returncode, r.stdout.decode(errors="replace")
        except subprocess.TimeoutExpired:
            rc, text = -1, ""  # a hang is a failure the test caught
        if rc == 0:
            print(f"mutants: SURVIVED '{name}' - the host test does not catch it")
            gaps += 1
        elif "checks failed" not in text and rc != -1:
            print(f"mutants: '{name}' did not build or run - fix the mutant")
            gaps += 1
        else:
            print(f"mutants: killed '{name}'")
    print(f"mutants: {len(MUTANTS) - gaps} of {len(MUTANTS)} killed")
    return 1 if gaps else 0


if __name__ == "__main__":
    sys.exit(main())
