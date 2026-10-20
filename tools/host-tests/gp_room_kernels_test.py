"""GP kernels of the room effect (docs/r3d/divergences.md #180), run in
nxdk-ex's DSP56300 simulator (3rdparty/nxdk-ex/lib/openal/tools/dsp56sim.py)
inside the library's own frame loop, loaded the way apu_gp.c loads a custom
kernel.

gp_hlprobe: the checked-in image is the source assembled now; mode 0 puts the
input on the output as the codec takes it (top 16 bits), mode 1 the low 8
bits a 16-bit source must leave at zero.

gp_hldsp, the room effect: the checked-in image is the source assembled now,
and frame by frame the codec's output equals what the CPU path makes with the
real s_dsp.c (built with gcc into s_dsp_gp_host.c's program, which also runs
the kernel's driver, SX_R3D_GpParams, on its own copy): every preset of both
tables entered from silence, the longest delay with its feedback, a reverb
crossfading to 31 samples back, random room changes (water, room_off, the
menu, the table, the master volume, segments down to one frame) and loud
input on a loud dry mix. About 2.5 minutes; HLDSP_FULL=1 runs every preset
longer, more long delays and more changes (about 11 minutes); HLDSP_ONLY=<case>
runs one case.

  python tools/host-tests/gp_room_kernels_test.py      (gcc on PATH)"""
import os
import random
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OPENAL = os.path.join(ROOT, '3rdparty', 'nxdk-ex', 'lib', 'openal')
APU = os.path.join(ROOT, 'engine', 'platform', 'xbox', 'apu')
sys.path.insert(0, os.path.join(OPENAL, 'tools'))
sys.dont_write_bytecode = True   # no __pycache__ inside the submodule
import dsp56sim  # noqa: E402

LOOP = open(os.path.join(OPENAL, 'dsp', 'gp_loop.asm'), encoding='utf-8').read()
COPY_DESC = [0x4000, 1 | 2 | (1 << 10) | (32 << 14), (32 << 4) | 1, 0x1400, 0, 0, 0]   # apu_gp.c

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('FAIL:', what)


def image_words(header):
    """The image array of a dsp56asm.py --image header."""
    text = open(header, encoding='utf-8').read()
    body = text[text.index('_image['):]
    body = body[body.index('{') + 1:body.index('}')]
    return [int(w.strip().rstrip('u'), 16) for w in body.split(',') if w.strip()]


def assemble(name):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, name + '.h')
        subprocess.run([sys.executable, os.path.join(OPENAL, 'tools', 'dsp56asm.py'),
                        os.path.join(APU, name + '.asm'), '-o', out, '--image'], check=True)
        return image_words(out)


def new_gp(source):
    gp = dsp56sim.Gp(LOOP, extra=(source,))
    for i, v in enumerate(COPY_DESC):
        gp.x[i] = v
    gp.run_until_wait()
    return gp


def load_custom(gp, params, scratch_bytes):
    """What apu_gp.c does for a custom ABI 1 kernel (nxdk-ex tests/test_gp_programs.py, test_legacy)."""
    s = gp.symbols
    for a in range(s['KX_BASE'], 0xC00):
        gp.x[a] = 0
    gp.x[s['KIN']] = 0x1400 + 32 * 2
    gp.x[s['KSCR']] = 0x2000
    gp.x[s['KSCRN']] = scratch_bytes
    for i, v in enumerate(params):
        gp.x[s['KPARAM'] + i] = v & 0xFFFFFF
    gp.x[s['K_RHEAD']] = 0
    gp.x[s['K_WHEAD']] = s['FIFO2']
    for i, v in enumerate([0x4000, 1 | 2 | (1 << 10) | (32 << 14), (32 << 4) | 1, s['OUTL'], 0, 0, 0]):
        gp.x[s['FIFO2'] + i] = v
    gp.x[s['FLAGS']] = 0x10   # an ABI 1 kernel at P:$100 (apu_gp.c FLAG_LEGACY, nxdk-ex #50)


def probe_frame(gp, left, right, dry=0):
    """One frame: left/right (24-bit words) on bins 2/3, dry on bins 0/1; returns (L, R) of the FIFO."""
    fifo = gp.frame({0: [dry] * 32, 1: [dry] * 32, 2: left, 3: right})
    return fifo[0::2], fifo[1::2]


def sx24(v):
    v &= 0xFFFFFF
    return v - (1 << 24) if v & 0x800000 else v


def test_probe():
    src_path = os.path.join(APU, 'gp_hlprobe.asm')
    check(assemble('gp_hlprobe') == image_words(os.path.join(APU, 'gp_hlprobe.h')),
          'gp_hlprobe.h is not gp_hlprobe.asm assembled now')
    source = open(src_path, encoding='utf-8').read()
    rng = random.Random(180)
    for mode in (0, 1):
        gp = new_gp(source)
        load_custom(gp, [mode], 0x1000)
        for frame in range(40):
            if frame < 20:   # 16-bit samples as a stereo send at 1.0 delivers them
                l16 = [rng.randint(-32768, 32767) for _ in range(32)]
                r16 = [rng.randint(-32768, 32767) for _ in range(32)]
                if frame == 0:
                    l16[:4] = [-32768, 32767, 0, -1]
                left = [(v << 8) & 0xFFFFFF for v in l16]
                right = [(v << 8) & 0xFFFFFF for v in r16]
            else:            # any 24-bit word: what an inexact path would deliver
                left = [rng.randint(0, 0xFFFFFF) for _ in range(32)]
                right = [rng.randint(0, 0xFFFFFF) for _ in range(32)]
            got_l, got_r = probe_frame(gp, left, right, dry=0x123400)
            if mode == 0:
                want_l = [sx24(v) >> 8 for v in left]
                want_r = [sx24(v) >> 8 for v in right]
            else:
                want_l = [v & 0xFF for v in left]
                want_r = [v & 0xFF for v in right]
            check(got_l == want_l and got_r == want_r,
                  'probe mode %d frame %d: %s / %s, want %s / %s' % (mode, frame, got_l[:4], got_r[:4],
                                                                     want_l[:4], want_r[:4]))


HOST_C = os.path.join(ROOT, 'tools', 'host-tests')
OUT = os.path.join(os.environ.get('SCRATCH', os.path.join(ROOT, '.sweep')), 'host-tests')


def build_host():
    """The reference room effect and the kernel's driver (s_dsp_gp_host.c, two copies of s_dsp.c)."""
    os.makedirs(OUT, exist_ok=True)
    flags = ['-O2', '-Wall', '-std=gnu99', '-DXASH_XBOX=1', '-I' + os.path.join(HOST_C, 's_dsp_stub'),
             '-I' + os.path.join(ROOT, 'engine', 'client', 'sound')]
    objs = []
    for tu in ('ref', 'drv'):
        o = os.path.join(OUT, 's_dsp_gp_%s.o' % tu)
        subprocess.run(['gcc'] + flags + ['-DTU=' + tu, '-c', os.path.join(HOST_C, 's_dsp_gp_tu.c'), '-o', o], check=True)
        objs.append(o)
    exe = os.path.join(OUT, 's_dsp_gp_host')
    subprocess.run(['gcc'] + flags + [os.path.join(HOST_C, 's_dsp_gp_host.c')] + objs + ['-lm', '-o', exe], check=True)
    return exe


def run_host(exe, blocks):
    """blocks: (control, room[64], dry[64]); control = (room, water, table, off, active, gain).
    -> [(params[16], reference out[64])]"""
    lines = [str(len(blocks))]
    for ctl, room, dry in blocks:
        lines.append('%g %d %g %g %d %d' % ctl)
        lines.append(' '.join(map(str, room)))
        lines.append(' '.join(map(str, dry)))
    out = subprocess.run([exe], input='\n'.join(lines) + '\n', capture_output=True, text=True, check=True).stdout
    rows = out.split('\n')
    return [([int(v) for v in rows[2 * i].split()], [int(v) for v in rows[2 * i + 1].split()])
            for i in range(len(blocks))]


def run_kernel(gp, rows, blocks, what):
    """Frame m: the driver's parameters of block m in the mailbox, block m's mixes on the bins; the
    output against the CPU path's (which applies them a block later). -> the most instructions a frame took"""
    kp = gp.symbols['KPARAM']
    bad = steps = 0
    for m, ((params, want), (_, room, dry)) in enumerate(zip(rows, blocks)):
        for i, v in enumerate(params):
            gp.x[kp + i] = v & 0xFFFFFF
        fifo = gp.frame({0: [v << 8 for v in dry[0::2]], 1: [v << 8 for v in dry[1::2]],
                         2: [v << 8 for v in room[0::2]], 3: [v << 8 for v in room[1::2]]})
        steps = max(steps, gp.frame_steps)
        if fifo != want:
            bad += 1
            if bad <= 3:
                k = next(i for i in range(64) if fifo[i] != want[i])
                print('FAIL: %s frame %d sample %d (%s): got %s, want %s; params %s' % (
                    what, m, k // 2, 'LR'[k % 2], fifo[k:k + 6], want[k:k + 6], params))
    check(bad == 0, '%s: %d of %d frames differ' % (what, bad, len(blocks)))
    return steps


def hl_signal(rng, n, loud=False):
    """n blocks of room mix: bursts of noise, tones and silence (the zero tests of s_dsp.c matter)."""
    out = []
    mode, left = 0, 0
    for _ in range(n):
        if left <= 0:
            mode, left = rng.choice((0, 1, 1, 2, 3)), rng.randint(2, 40)
        left -= 1
        blk = []
        for i in range(32):
            if mode == 0:
                l = r = 0
            elif mode == 1:
                amp = 32767 if loud else rng.choice((300, 3000, 12000))
                l, r = rng.randint(-amp, amp), rng.randint(-amp, amp)
            elif mode == 2:
                l, r = rng.randint(-32768, 32767), 0
            else:
                l = r = rng.choice((-32768, 32767)) if loud else rng.randint(-20000, 20000)
            blk += [l, r]
        out.append(blk)
    return out


def hl_case(exe, source, what, controls, seed, loud=False, dry_amp=0):
    rng = random.Random(seed)
    rooms = hl_signal(rng, len(controls), loud)
    blocks = []
    for ctl, room in zip(controls, rooms):
        dry = [rng.randint(-dry_amp, dry_amp) for _ in range(64)]
        blocks.append((ctl, room, dry))
    rows = run_host(exe, blocks)
    gp = new_gp(source)
    load_custom(gp, rows[0][0], 0x20F00)
    return run_kernel(gp, rows, blocks, what)


def test_hldsp():
    src_path = os.path.join(APU, 'gp_hldsp.asm')
    check(assemble('gp_hldsp') == image_words(os.path.join(APU, 'gp_hldsp.h')),
          'gp_hldsp.h is not gp_hldsp.asm assembled now')
    source = open(src_path, encoding='utf-8').read()
    exe = build_host()
    only = os.environ.get('HLDSP_ONLY')
    full = os.environ.get('HLDSP_FULL')
    cases = []
    # every preset of both tables, entered from silence
    for table in (0, 1):
        for room in range(29):
            cases.append(('table %d room %d' % (table, room), [(room, 0, table, 0, 1, 256)] * (120 if full else 24),
                          1000 * table + room, False, 0))
    # past the longest delays (0.38 s: 570 frames) with their feedback
    for table, room in ((0, 22), (0, 25), (0, 16), (1, 19), (0, 28)) if full else ((0, 22),):
        cases.append(('long table %d room %d' % (table, room), [(room, 0, table, 0, 1, 256)] * 700,
                      5000 + 100 * table + room, False, 0))
    # a reverb crossfading to room 28's 0.001 s: its second line ends 31 samples back
    for table in (0, 1):
        cases.append(('room 25 to 28, table %d' % table, [(25, 0, table, 0, 1, 256)] * 40 + [(28, 0, table, 0, 1, 256)] * 60,
                      6000 + table, False, 0))
    # room changes: presets, both tables, water, room_off, the menu, the master volume; segments down to
    # one frame (crossfades started over each other, lines freed and set up again)
    for seed in range(3 if full else 1):
        rng = random.Random(7000 + seed)
        ctl, room, table, water, off, active, gain = [], 0, 0, 0, 0, 1, 256
        length = 1500 if full else 600
        while len(ctl) < length:
            ev = rng.random()
            if ev < 0.55:
                room = rng.randint(0, 28)
            elif ev < 0.65:
                water = 1 - water
            elif ev < 0.72:
                table = 1 - table
            elif ev < 0.78:
                off = 1 - off
            elif ev < 0.86:
                active = 1 - active
            else:
                gain = rng.choice((0, 1, 77, 255, 256))
            ctl += [(room, water, table, off, active, gain)] * rng.choice((1, 1, 2, 3, 10, 30, 60, 120))
        cases.append(('changes %d' % seed, ctl[:length], 7100 + seed, False, 0))
    # loud: clipping in every stage and at the output
    cases.append(('loud', [(r, 0, 0, 0, 1, 256) for r in (19, 7, 25, 16, 2) for _ in range(60)], 9000, True, 32760))
    steps = 0
    for what, ctl, seed, loud, dry_amp in cases:
        if only and only != what:
            continue
        steps = max(steps, hl_case(exe, source, what, ctl, seed, loud, dry_amp))
    print('gp_hldsp: at most %d instructions a frame (host simulator)' % steps)


test_probe()
test_hldsp()
if failures:
    print('gp_room_kernels: %d failure(s)' % failures)
    sys.exit(1)
print('gp_room_kernels: all passed')
