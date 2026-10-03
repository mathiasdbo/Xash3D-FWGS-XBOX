"""Tests for tools/frametime-report.py.

Run through tools/host-tests/run.sh, which first builds and runs
tools/host-tests/xbox_frametime_util_host_test.c: that test writes two
recordings through the recorder's own encoders (xbox_frametime_util.c) -
a version-2 file with work counters to FRAMETIME_FIXTURE and the same
session as a version-1 file to FRAMETIME_FIXTURE_V1 - and every
expectation below is derived from what that C test wrote (see its
write_fixture), not from this parser.

    FRAMETIME_FIXTURE=.sweep/host-tests/frametime_fixture.bin \
    FRAMETIME_FIXTURE_V1=.sweep/host-tests/frametime_fixture_v1.bin \
    FRAMETIME_FIXTURE_LC=.sweep/host-tests/frametime_fixture_lc.bin \
    FRAMETIME_FIXTURE_LC_V1=.sweep/host-tests/frametime_fixture_lc_v1.bin python tools/test_frametime_report.py

The level-change fixtures (class LevelChange) are the same session written
with the recorder's LOAD flag (v2) and without it (v1, as recorders before
2026-10-01 wrote it); see write_fixture_level_change in the C test.

FRAMETIME_FIXTURE is a version-2 file as a recorder from before the CPU
spans wrote it (20 counters, no SPANS chunk). FRAMETIME_FIXTURE_SPANS
(class Spans) is a current one: every counter, the SPANS chunk and known
span times; see write_fixture_spans in the C test.
"""
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SPEC = importlib.util.spec_from_file_location("frametime_report", os.path.join(_HERE, "frametime-report.py"))
fr = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(fr)

FIXTURE = os.environ.get("FRAMETIME_FIXTURE", "")
FIXTURE_V1 = os.environ.get("FRAMETIME_FIXTURE_V1", "")
FIXTURE_LC = os.environ.get("FRAMETIME_FIXTURE_LC", "")
FIXTURE_LC_V1 = os.environ.get("FRAMETIME_FIXTURE_LC_V1", "")
FIXTURE_SPANS = os.environ.get("FRAMETIME_FIXTURE_SPANS", "")


def _read(path, var):
    if not path or not os.path.isfile(path):
        raise unittest.SkipTest("%s not set - run tools/host-tests/run.sh" % var)
    with open(path, "rb") as f:
        return f.read()


def _fixture_bytes():
    return _read(FIXTURE, "FRAMETIME_FIXTURE")


def _fixture_v1_bytes():
    return _read(FIXTURE_V1, "FRAMETIME_FIXTURE_V1")


# A minimal DOM for running the report's own script under node: enough
# for every call it makes, recording what it writes into each element.
_DOM_STUB = r"""
const els = {};
function mk(id) {
  return els[id] || (els[id] = { id, innerHTML: '', hidden: true, children: [], textContent: '',
    appendChild(c) { this.children.push(c); }, classList: { toggle() {} },
    clientWidth: 800, clientHeight: 260, width: 0, height: 0,
    getContext() { return new Proxy({}, { get: (t, k) => k in t ? t[k] : (() => {}), set: (t, k, v) => { t[k] = v; return true; } }); },
    addEventListener() {}, getBoundingClientRect() { return { left: 0, width: 800 }; } });
}
var document = { getElementById: mk, createElement: () => ({ innerHTML: '', classList: { toggle() {} } }), documentElement: {} };
var window = { devicePixelRatio: 1, addEventListener() {}, matchMedia: null };
function getComputedStyle() { return { getPropertyValue: () => '#000' }; }
"""


def run_page_script(page):
    """Runs the page's <script> under node against _DOM_STUB and returns
    {element id: innerHTML} as JSON-decoded by node's stdout; skips the
    test when node is not installed."""
    node = shutil.which("node")
    if not node:
        raise unittest.SkipTest("node not installed")
    js = re.search(r"<script>(.*)</script>", page, re.S).group(1)
    js = _DOM_STUB + js + "\nconsole.log(JSON.stringify(Object.fromEntries(Object.entries(els).map(([k, v]) => [k, {html: v.innerHTML, hidden: v.hidden}]))));"
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False, encoding="utf-8") as f:
        f.write(js)
        path = f.name
    try:
        out = subprocess.run([node, path], capture_output=True, text=True, timeout=60)
    finally:
        os.unlink(path)
    if out.returncode != 0:
        raise AssertionError("the report's script failed under node:\n" + out.stderr)
    import json
    return json.loads(out.stdout.strip().splitlines()[-1])


class ParseFixtureV1(unittest.TestCase):
    """The version-1 file: no counters, no flush frames - the times and
    the statistics exactly as before version 2."""
    def setUp(self):
        self.data = _fixture_v1_bytes()
        self.rec = fr.parse(self.data)

    def test_header_and_calibration(self):
        r = self.rec
        self.assertEqual(r["version"], 1)
        self.assertEqual(r["counters"], [])
        self.assertEqual(fr.work_names(r), [])
        self.assertEqual(r["frames"][0][8], ())
        self.assertEqual(r["build"], "fixture")
        self.assertEqual(r["ring_frames"], 1024)
        self.assertEqual(r["mode"], 1)
        self.assertEqual(len(r["calibs"]), 2)
        self.assertAlmostEqual(r["tsc_hz"], 1e6)
        self.assertFalse(r["truncated"])
        self.assertEqual(r["end"], {"tsc": 6000000, "lost": 1})

    def test_frames_maps_and_gap(self):
        r = self.rec
        self.assertEqual(len(r["frames"]), 49)          # 0..19, 21..39, 40..49
        self.assertEqual(r["gaps"], 1)
        self.assertEqual(r["lost_frames"], 1)            # seq 20
        self.assertEqual([m["name"] for m in r["maps"]], ["c1a0", "c1a0e"])
        self.assertEqual([m["frame"] for m in r["maps"]], [0, 39])
        self.assertEqual(r["frames"][20][0], 21)

    def test_map_stats(self):
        segs = fr.segments(self.rec)
        self.assertEqual([s["name"] for s in segs], ["c1a0", "c1a0e"])
        a = fr.segment_stats(self.rec, segs[0])
        # 39 active frames, the first 30 left out: seq 31..39, one of them 50 ms.
        self.assertEqual(a["frames"], 9)
        self.assertAlmostEqual(a["avg_fps"], 9 / 0.210)
        self.assertAlmostEqual(a["worst_ms"], 50.0)
        self.assertAlmostEqual(a["low1_fps"], 20.0)      # rank ceil(9 * .99) = 9: the 50 ms frame
        self.assertEqual(a["over60"], 9)
        self.assertEqual(a["over30"], 1)
        self.assertAlmostEqual(a["cpu_ms"], (8 * 16 + 46) / 9)
        self.assertAlmostEqual(a["gpu_ms"], 3.0)
        self.assertAlmostEqual(a["other_ms"], 1.0)
        self.assertAlmostEqual(a["gpu_bound_pct"], 100.0)
        self.assertAlmostEqual(a["load_s"], 2.5)
        self.assertAlmostEqual(a["free_before"], 6000 * 4096 / 2 ** 20)
        self.assertAlmostEqual(a["free_after"], 5500 * 4096 / 2 ** 20)
        self.assertIsNone(a["min_fps"])                  # 210 ms of play: no full one-second window
        # The second map: 10 frames, all inside the warm-up (one is a menu frame).
        b = fr.segment_stats(self.rec, segs[1])
        self.assertEqual(b["frames"], 0)
        self.assertEqual(b["all_frames"], 10)

    def test_session_and_timeline(self):
        rows = [fr.segment_stats(self.rec, s) for s in fr.segments(self.rec)]
        s = fr.session_stats(self.rec, rows)
        self.assertEqual(s["frames"], 49)
        self.assertEqual(s["loads"], 2)
        self.assertEqual(s["lost"], 1)                   # the gap and the END count are the same frame
        self.assertTrue(s["clean_end"])
        self.assertAlmostEqual(s["minutes"], (38 * 20 + 50 + 10 * 10) / 60000.0)
        tl = fr.timeline(self.rec)
        self.assertEqual(len(tl), 1)
        self.assertIsNone(tl[0][1])                      # a menu frame in the window: not "play"
        marks = fr.map_markers(self.rec)
        self.assertAlmostEqual(marks[1]["t"], 0.81)

    def test_report_renders(self):
        rows = [fr.segment_stats(self.rec, s) for s in fr.segments(self.rec)]
        s = fr.session_stats(self.rec, rows)
        page = fr.render_html(self.rec, rows, s, ["fixture.bin"], source="xemu")
        self.assertEqual(re.findall(r"__[A-Z]+__", page), [])
        self.assertIn("Recorded in xemu", page)
        self.assertIn("c1a0e", page)
        self.assertIn("Version-1 file", page)
        text = fr.summary_text(self.rec, rows, s)
        self.assertIn("c1a0", text)
        self.assertIn("no work counters", text)
        self.assertEqual(fr.work_correlation(self.rec), [])
        els = run_page_script(page)
        self.assertTrue(els.get("work", {"hidden": True})["hidden"])  # no counters: the work section is never shown

    def test_cut_short_keeps_complete_frames(self):
        # Cut 10 bytes into the 6th frame of the first FRAMES chunk:
        # header 128 + MAP 104 + chunk head 16 + 5 frames.
        cut = 128 + 104 + 16 + 5 * 32 + 10
        r = fr.parse(self.data[:cut])
        self.assertTrue(r["truncated"])
        self.assertEqual(len(r["frames"]), 5)
        self.assertIsNone(r["end"])
        rows = [fr.segment_stats(r, s) for s in fr.segments(r)]
        self.assertFalse(fr.session_stats(r, rows)["clean_end"])


# The version-2 fixture: the v1 session plus, for frame seq i, counter j =
# 100 * (j + 1) + i; frame 35 has 10x the draws; frame 33 is a flush frame.
MEASURED = [31, 32, 34, 35, 36, 37, 38, 39]     # c1a0's in-play frames past the warm-up, 33 (flush) left out


class ParseFixtureV2(unittest.TestCase):
    def setUp(self):
        self.data = _fixture_bytes()
        self.rec = fr.parse(self.data)
        self.segs = fr.segments(self.rec)

    def test_header_and_counters(self):
        r = self.rec
        self.assertEqual(r["version"], 2)
        self.assertEqual(len(r["counters"]), 20)
        self.assertEqual(r["counters"][0], "draws")
        self.assertEqual(r["counters"][2], "pb_dwords")
        self.assertEqual(r["counters"][-1], "sprites")
        self.assertEqual(fr.work_names(r)[-1], fr.VERTS)
        self.assertEqual(len(r["frames"]), 49)
        f21 = r["frames"][20]
        self.assertEqual(f21[0], 21)
        self.assertEqual(f21[8][0], 121)
        self.assertEqual(f21[8][19], 2021)
        self.assertEqual(r["end"], {"tsc": 6000000, "lost": 1})

    def test_flush_frames_left_out_by_default(self):
        a = fr.segment_stats(self.rec, self.segs[0])
        self.assertEqual(a["frames"], 8)
        self.assertEqual(a["flush_frames"], 1)
        self.assertAlmostEqual(a["flush_frame_ms"], 20.0)
        self.assertAlmostEqual(a["avg_fps"], 8 / 0.190)
        self.assertAlmostEqual(a["worst_ms"], 50.0)
        b = fr.segment_stats(self.rec, self.segs[0], include_flush=True)
        self.assertEqual(b["frames"], 9)
        self.assertEqual(b["flush_frames"], 1)
        self.assertAlmostEqual(b["avg_fps"], 9 / 0.210)
        self.assertAlmostEqual(b["work_avg"]["draws"], (2297 + 133) / 9)
        s = fr.session_stats(self.rec, [a], False)
        self.assertEqual(s["flush_frames"], 1)
        self.assertFalse(s["flush_included"])

    def test_work_per_map(self):
        a = fr.segment_stats(self.rec, self.segs[0])
        # draws: 100 + seq, frame 35 ten times that.
        self.assertAlmostEqual(a["work_avg"]["draws"], (131 + 132 + 134 + 1350 + 136 + 137 + 138 + 139) / 8)
        self.assertEqual(a["work_peak"]["draws"], 1350)
        self.assertAlmostEqual(a["work_avg"]["prims"], sum(200 + i for i in MEASURED) / 8)
        self.assertEqual(a["work_peak"]["sprites"], 2039)
        # verts = the five v_* counters (j = 4..8): 3500 + 5 * seq.
        self.assertAlmostEqual(a["work_avg"][fr.VERTS], sum(3500 + 5 * i for i in MEASURED) / 8)
        # Every frame waited 3 ms on the GPU: all of them are GPU-bound.
        self.assertEqual(a["gpu_hi_frames"], 8)
        self.assertIsNone(a["work_gpu_lo"])
        self.assertAlmostEqual(a["work_gpu_hi"]["draws"], a["work_avg"]["draws"])

    def test_correlation_and_top_frames(self):
        corr = {c["name"]: c for c in fr.work_correlation(self.rec)}
        self.assertEqual(len(corr), 21)
        # The 50 ms frame (46 ms CPU) is the one with 10x the draws.
        self.assertGreater(corr["draws"]["r_cpu"], 0.9)
        self.assertIsNone(corr["draws"]["r_gpu"])       # the GPU wait never varies
        self.assertEqual(corr["draws"]["peak"], 1350)
        top = fr.top_frames(self.rec, "total", 3)
        self.assertEqual(top[0]["seq"], 35)
        self.assertAlmostEqual(top[0]["total"], 50.0)
        self.assertEqual(top[0]["work"]["draws"], 1350)
        self.assertAlmostEqual(top[0]["t"], (34 * 20) / 1000.0)  # 34 frames of 20 ms before it (seq 20 lost)
        self.assertEqual(len(fr.top_frames(self.rec, "gpu", 10)), 8)
        self.assertEqual(len(fr.top_frames(self.rec, "gpu", 10, include_flush=True)), 9)

    def test_timeline_work_columns(self):
        tl = fr.timeline(self.rec)
        self.assertEqual(len(tl[0]), 10)
        self.assertIsNone(tl[0][7])                      # the window holds a menu frame: not play

    def test_report_renders(self):
        rows = [fr.segment_stats(self.rec, s) for s in self.segs]
        s = fr.session_stats(self.rec, rows)
        page = fr.render_html(self.rec, rows, s, ["fixture.bin"], source="xemu")
        self.assertEqual(re.findall(r"__[A-Z]+__", page), [])
        self.assertIn("Work per frame", page)
        self.assertIn("they are the same in xemu as on the console", page)
        self.assertIn("<b>left out</b> of the statistics", page)
        page = fr.render_html(self.rec, rows, s, ["fixture.bin"], include_flush=True)
        self.assertIn("<b>kept</b> in the statistics", page)
        text = fr.summary_text(self.rec, rows, s)
        self.assertIn("work per frame", text)
        self.assertIn("1 flush frame(s)", text)

    def test_page_script_runs(self):
        # The page's own script, run: a name clash or a typo in it blanks
        # the whole report in a browser while every string check passes.
        rows = [fr.segment_stats(self.rec, s) for s in self.segs]
        page = fr.render_html(self.rec, rows, fr.session_stats(self.rec, rows), ["fixture.bin"])
        els = run_page_script(page)
        self.assertFalse(els["work"]["hidden"])
        self.assertIn("c1a0", els["worktab"]["html"])
        self.assertIn("1.4k", els["worktab"]["html"])      # the draws peak, 1350
        self.assertIn("draws", els["corrtab"]["html"])
        self.assertEqual(els["toptab"]["html"].count("<tr>"), 9)  # header + 8 frames

    def test_cut_short_keeps_complete_frames(self):
        # header 128 + COUNTERS (8 + 4 + 20 x 16) + MAP 104 + chunk head 16
        # + 5 frames of 112 bytes, then 10 bytes into the 6th.
        cut = 128 + 332 + 104 + 16 + 5 * 112 + 10
        r = fr.parse(self.data[:cut])
        self.assertTrue(r["truncated"])
        self.assertEqual(len(r["frames"]), 5)
        self.assertEqual(r["frames"][4][8][1], 204)

    def test_fewer_names_than_counters(self):
        # A COUNTERS chunk naming 19 of the 20 counters: the last one gets a
        # placeholder name rather than being dropped or misread.
        data = bytearray(self.data)
        data[128 + 8] = 19
        r = fr.parse(bytes(data))
        self.assertEqual(len(r["counters"]), 20)
        self.assertEqual(r["counters"][18], "beams")
        self.assertEqual(r["counters"][19], "c19")


class FailurePaths(unittest.TestCase):
    def test_not_a_recording(self):
        with self.assertRaises(fr.FormatError):
            fr.parse(b"x" * 200)
        with self.assertRaises(fr.FormatError):
            fr.parse(b"R3DFTIME")

    def test_wrong_version(self):
        data = bytearray(_fixture_bytes())
        data[8] = 99
        with self.assertRaises(fr.FormatError):
            fr.parse(bytes(data))

    def test_bad_frame_size(self):
        data = bytearray(_fixture_bytes())
        data[16] = 113                                   # not 32 + a whole number of counters
        with self.assertRaises(fr.FormatError):
            fr.parse(bytes(data))

    def test_percentile(self):
        self.assertEqual(fr.percentile_ms([], 0.99), 0.0)
        v = sorted([10.0] * 99 + [40.0])
        self.assertEqual(fr.percentile_ms(v, 0.99), 10.0)   # rank 99
        self.assertEqual(fr.percentile_ms(v, 0.995), 40.0)  # rank 100


class LevelChange(unittest.TestCase):
    """write_fixture_level_change: c1a0 seq 0..39 in play at 20 ms; seq 35
    a 600 ms stall inside Host_Frame; seq 38 the changelevel frame (500 ms,
    490 between host frames; LOAD-flagged in the v2 file); seq 39 the last
    frame starting in play (100 ms, 90 between - under the shape rule);
    seq 40..44 loading; c1a0e seq 45..84 at 10 ms, seq 83 300 ms between
    host frames with no load after it. 1 MHz TSC: a tick is a microsecond."""

    def _recs(self):
        return [("flag", fr.parse(_read(FIXTURE_LC, "FRAMETIME_FIXTURE_LC"))),
                ("shape", fr.parse(_read(FIXTURE_LC_V1, "FRAMETIME_FIXTURE_LC_V1")))]

    def test_versions(self):
        (_, flagged), (_, older) = self._recs()
        self.assertEqual(flagged["version"], 2)
        self.assertEqual(older["version"], 1)
        self.assertEqual(flagged["frames"][38][7], fr.F_ACTIVE | fr.F_LOAD)
        self.assertEqual(older["frames"][38][7], fr.F_ACTIVE)

    def test_detected_by_flag_and_by_shape(self):
        for source, rec in self._recs():
            lc = fr.level_change_frames(rec)
            self.assertEqual(sorted(lc), [38], source)
            self.assertEqual(lc[38]["source"], source)
            self.assertEqual((lc[38]["map"], lc[38]["next"]), ("c1a0", "c1a0e"))
            rows = fr.level_change_list(rec)
            self.assertEqual(len(rows), 1)
            r = rows[0]
            self.assertAlmostEqual(r["total"], 500.0)
            self.assertAlmostEqual(r["cpu"], 7.0)          # host 10 - GPU 3
            self.assertAlmostEqual(r["other"], 490.0)
            self.assertAlmostEqual(r["t"], (37 * 20 + 600) / 1000.0)

    def test_flag_decides_when_present(self):
        (_, flagged), _ = self._recs()
        # A file with LOAD flags is trusted alone: flag a mid-map frame
        # and it is a level change, whatever its shape...
        flagged["frames"][20] = flagged["frames"][20][:7] + (flagged["frames"][20][7] | fr.F_LOAD,) + flagged["frames"][20][8:]
        self.assertEqual(sorted(fr.level_change_frames(flagged)), [20, 38])
        # ... and with every flag cleared, the shape rule finds the same frame.
        (_, again), _ = self._recs()
        again["frames"] = [f[:7] + (f[7] & ~fr.F_LOAD,) + f[8:] for f in again["frames"]]
        self.assertEqual(sorted(fr.level_change_frames(again)), [38])

    def test_left_out_of_play_stats(self):
        for source, rec in self._recs():
            seg = fr.segments(rec)[0]
            a = fr.segment_stats(rec, seg)
            # In play past the 30-frame warm-up: seq 30..39, minus 38.
            self.assertEqual(a["frames"], 9, source)
            self.assertEqual(a["lc_frames"], 1)
            self.assertAlmostEqual(a["lc_ms"], 500.0)
            self.assertAlmostEqual(a["worst_ms"], 600.0)    # the stall stays: it is play
            b = fr.segment_stats(rec, seg, include_level_change=True)
            self.assertEqual(b["frames"], 10)
            self.assertAlmostEqual(b["seconds"], a["seconds"] + 0.5)
            top = fr.top_frames(rec, "total", 3)
            self.assertNotIn(38, [t["seq"] for t in top])
            self.assertIn(38, [t["seq"] for t in fr.top_frames(rec, "total", 3, include_level_change=True)])

    def test_stalls(self):
        for source, rec in self._recs():
            st = fr.stall_list(rec)
            self.assertEqual([s["seq"] for s in st], [35, 83], source)
            self.assertAlmostEqual(st[0]["cpu"], 596.0)    # host 599 - GPU 3: inside Host_Frame
            self.assertAlmostEqual(st[0]["cpu_pct"], 100.0 * 596 / 600)
            self.assertEqual(st[0]["map"], "c1a0")
            self.assertAlmostEqual(st[1]["cpu"], 9.5)      # host 10 - GPU 0.5
            self.assertAlmostEqual(st[1]["other"], 290.0)  # between frames, no load after it
            self.assertEqual(st[1]["map"], "c1a0e")
            self.assertFalse(st[1]["level_change"])
            st = fr.stall_list(rec, include_level_change=True)
            self.assertEqual([s["seq"] for s in st], [35, 38, 83])
            self.assertTrue(st[1]["level_change"])

    def test_summary_and_page(self):
        for source, rec in self._recs():
            rows = [fr.segment_stats(rec, s) for s in fr.segments(rec)]
            s = fr.session_stats(rec, rows)
            self.assertEqual(len(s["level_change"]), 1)
            self.assertEqual(len(s["stalls"]), 2)
            text = fr.summary_text(rec, rows, s)
            self.assertIn("level-change frames: 1", text)
            self.assertIn("stalls: 2 in-play frame(s) over 250 ms", text)
            self.assertIn("c1a0 -> c1a0e", text)
            self.assertIn("LOAD flag" if source == "flag" else "shape (unflagged file)", text)
            page = fr.render_html(rec, rows, s, ["lc.bin"])
            self.assertEqual(re.findall(r"__[A-Z]+__", page), [])
            self.assertIn("Level changes and stalls", page)
            self.assertIn("c1a0 &rarr; c1a0e", page)
            run_page_script(page)

    def test_cli(self):
        for path in (FIXTURE_LC, FIXTURE_LC_V1):
            _read(path, "FRAMETIME_FIXTURE_LC")
            out = subprocess.run([sys.executable, os.path.join(_HERE, "frametime-report.py"), "summary", path,
                                  "--include-level-change"], capture_output=True, text=True, timeout=60)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("kept in the statistics; --include-level-change is on", out.stdout)


class Spans(unittest.TestCase):
    """write_fixture_spans: 1 MHz TSC (a kilocycle is a millisecond), map
    c1a0c, seq 0..49 at 20 ms (CPU 16) with spans input 1, cl_begin 1,
    sv 6 (sv_read 1, sv_game 4, sv_send 1), cl 7 (cl_read 1, scr 5 (r_scene
    4 (world 1, client_studio 2 (studio_points 1)), r_end 1), snd 1): top
    level 15, 1 ms unattributed. seq 40: sv_game 10, sv 12, CPU 22. seq 41:
    client_studio 5, studio_points 3, r_scene 7, scr 8, cl 10, CPU 19. The
    statistics use seq 30..49 (the first 30 in play are the warm-up)."""

    @classmethod
    def setUpClass(cls):
        cls.data = _read(FIXTURE_SPANS, "FRAMETIME_FIXTURE_SPANS")
        cls.rec = fr.parse(cls.data)
        cls.segs = fr.segments(cls.rec)
        cls.rows = [fr.segment_stats(cls.rec, s) for s in cls.segs]
        cls.row = [r for r in cls.rows if r["map"] == "c1a0c"][0]

    def test_spans_chunk(self):
        r = self.rec
        self.assertEqual(r["version"], 2)
        self.assertEqual(len(r["counters"]), 72)
        self.assertEqual(r["counters"][20], "span_calls")
        self.assertEqual(len(r["spans"]), 50)
        # A count after the spans (2026-10-02) is a plain counter.
        self.assertEqual(r["counters"][-1], "world_walked")
        self.assertNotIn("world_walked", fr.span_names(r))
        self.assertEqual(r["frames"][0][8][-1], 500)
        self.assertEqual(r["spans"][0], ("k_input", ""))
        parent = dict(r["spans"])
        self.assertEqual(parent["k_sv_game"], "k_sv")
        self.assertEqual(parent["k_studio_points"], "k_client_studio")
        self.assertEqual(parent["k_client_studio"], "k_r_scene")
        self.assertEqual(parent["k_scr"], "k_cl")
        self.assertEqual(parent["k_sv_emit_ents"], "k_sv_send")
        self.assertEqual(parent["k_cl_predmove"], "k_cl_cmd")
        self.assertEqual(fr.span_depth(r)["k_cl_predmove"], 2)
        # Tree order: the appended round-trip spans sit under their parents.
        order = fr.span_tree_order(r)
        self.assertEqual(sorted(order), sorted(fr.span_names(r)))
        self.assertLess(order.index("k_sv_send"), order.index("k_sv_emit_ents"))
        self.assertLess(order.index("k_sv_emit_ents"), order.index("k_cl"))
        self.assertEqual(order.index("k_cl_cmd") + 1, order.index("k_cl_predmove"))
        # The mixer's two sets: under k_snd and under k_snd_extra.
        self.assertEqual(parent["k_snd_mix"], "k_snd")
        self.assertEqual(parent["k_sndx_mix"], "k_snd_extra")
        self.assertEqual(fr.span_depth(r)["k_sndx_dsp"], 3)
        self.assertEqual(order.index("k_snd_extra") + 1, order.index("k_sndx_mix"))
        self.assertLess(order.index("k_snd"), order.index("k_snd_spatial"))
        # The scene's breakdown: k_world's parts and k_r_scene's own.
        self.assertEqual(parent["k_world_walk"], "k_world")
        self.assertEqual(parent["k_warp_brush"], "k_brush")
        self.assertEqual(parent["k_brush"], "k_r_scene")
        self.assertEqual(fr.span_depth(r)["k_warp_brush"], 4)
        self.assertLess(order.index("k_world"), order.index("k_world_walk"))
        self.assertLess(order.index("k_world_walk"), order.index("k_client_studio"))
        self.assertEqual(r["unknown_chunks"], 0)
        self.assertEqual(fr.span_depth(r)["k_skin_read"], 5)

    def test_span_averages(self):
        a = self.row
        self.assertEqual(a["frames"], 20)
        self.assertAlmostEqual(a["cpu_ms"], (18 * 16 + 22 + 19) / 20.0)
        ms = a["span_ms"]
        self.assertAlmostEqual(ms["k_sv_game"], (19 * 4 + 10) / 20.0)
        self.assertAlmostEqual(ms["k_sv"], (19 * 6 + 12) / 20.0)
        self.assertAlmostEqual(ms["k_client_studio"], (19 * 2 + 5) / 20.0)
        self.assertAlmostEqual(ms["k_studio_points"], (19 * 1 + 3) / 20.0)
        self.assertAlmostEqual(ms["k_lm"], 0.0)
        own = a["span_own_ms"]
        self.assertAlmostEqual(own["k_sv"], 0.0)
        self.assertAlmostEqual(own["k_r_scene"], 1.0)     # 4 = world 1 + client_studio 2 + 1 (7 = 1 + 5 + 1)
        self.assertAlmostEqual(own["k_client_studio"], 1.05)
        self.assertAlmostEqual(own["k_scr"], 0.0)
        # The invariant the recorder is built on: top level + unattributed = CPU.
        self.assertAlmostEqual(a["span_top_ms"] + a["unattributed_ms"], a["cpu_ms"])
        self.assertAlmostEqual(a["unattributed_ms"], 1.0)
        self.assertAlmostEqual(a["span_calls"], 300.0)

    def test_own_clamps_at_zero(self):
        incl = dict.fromkeys(fr.span_names(self.rec), 0.0)
        incl.update({"k_studio_points": 1.0, "k_skin_read": 2.0})
        own = fr.span_own(self.rec, incl)
        self.assertEqual(own["k_studio_points"], 0.0)
        self.assertEqual(own["k_skin_read"], 2.0)

    def test_top_frames_by_span(self):
        top = fr.top_frames(self.rec, "k_sv_game", 3)
        self.assertEqual(top[0]["seq"], 40)
        self.assertAlmostEqual(top[0]["spans"]["k_sv_game"], 10.0)
        self.assertAlmostEqual(top[0]["cpu"], 22.0)
        self.assertEqual(fr.top_frames(self.rec, "k_client_studio", 1)[0]["seq"], 41)
        self.assertEqual(fr.top_frames(self.rec, "k_studio_points", 1)[0]["seq"], 41)

    def test_spans_are_not_in_the_count_correlation(self):
        names = [c["name"] for c in fr.work_correlation(self.rec)]
        self.assertIn("studio_models", names)
        self.assertIn("span_calls", names)
        self.assertFalse([n for n in names if n.startswith("k_")])

    def test_summary_and_page(self):
        s = fr.session_stats(self.rec, self.rows)
        text = fr.summary_text(self.rec, self.rows, s)
        self.assertIn("CPU spans", text)
        self.assertIn("unattributed", text)
        self.assertIn("top frames by k_sv_game", text)
        self.assertRegex(text, r"\n  sv_game +4\.30/4\.30 +26%")       # depth 1: two spaces
        page = fr.render_html(self.rec, self.rows, s, ["spans.bin"], source="xemu")
        self.assertEqual(re.findall(r"__[A-Z]+__", page), [])
        self.assertIn('id="spans"', page)
        self.assertIn("CPU by subsystem", page)
        self.assertIn("Recorded in xemu:</b> these are the emulator", page)
        self.assertIn("The ten frames with the most sv_game", page)
        run_page_script(page)

    def test_without_spans_chunk(self):
        # The same file with its SPANS chunk cut out reads as a reader that
        # does not know the chunk sees it: the k_* are plain counters.
        off = 128
        ctype, clen = fr.struct.unpack_from("<II", self.data, off)
        self.assertEqual(ctype, fr.CHUNK_COUNTERS)
        off += 8 + clen
        ctype, clen = fr.struct.unpack_from("<II", self.data, off)
        self.assertEqual(ctype, fr.CHUNK_SPANS)
        r = fr.parse(self.data[:off] + self.data[off + 8 + clen:])
        self.assertEqual(r["spans"], [])
        self.assertIn("k_sv_game", r["counters"])
        self.assertEqual(len(r["frames"]), 50)
        st = fr.segment_stats(r, fr.segments(r)[0])
        self.assertNotIn("span_ms", st)
        self.assertEqual(fr.spans_section_html(r, [st]), "")

    def test_pre_span_v2_file(self):
        r = fr.parse(_fixture_bytes())
        self.assertEqual(r["spans"], [])
        self.assertEqual(len(r["counters"]), 20)


if __name__ == "__main__":
    res = unittest.main(exit=False, verbosity=1).result
    # A skipped fixture is a broken run, not a pass.
    if res.skipped or not res.wasSuccessful():
        sys.exit(1)
