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
            # The largest own-time spans of each stall (only a file with spans has them).
            if source == "flag":
                self.assertEqual(st[0]["top_spans"], [("k_save_sav", 400.0), ("k_save_ents", 100.0), ("k_save", 30.0)])
                # First-use loads (2026-10-06): 250 kilocycles at 1 MHz is 250 ms.
                self.assertEqual(st[0]["loads"], {"snd": (2, 300, 250.0), "tex": (6, 4096), "parse": 50.0})
                # NOTES (2026-10-06): the frame's named events, slowest first.
                self.assertEqual(st[0]["notes"], [("message", "svc_sound", 200.0, 40),
                                                  ("sound", "fvox/flatline.wav", 120.0, 131072)])
                self.assertEqual(rec["notes_dropped"], 1)
            else:
                self.assertEqual(st[0]["top_spans"], [])
                self.assertEqual(st[0]["loads"], {})
                self.assertEqual(st[0]["notes"], [])
            self.assertEqual(st[1]["notes"], [])
            self.assertEqual(st[1]["loads"], {})
            self.assertEqual(st[1]["top_spans"], [])
            st = fr.stall_list(rec, include_level_change=True)
            self.assertEqual([s["seq"] for s in st], [35, 38, 83])
            self.assertTrue(st[1]["level_change"])
            self.assertFalse(any(s["opens_menu"] for s in st))
            # The changelevel frame flushed the rest of a pending save (#153).
            self.assertEqual(st[1]["loads"], {"save": (20, 30.0)} if source == "flag" else {})

    def test_load_stages(self):
        # Load stages (2026-10-08, #162): the c1a0e load holds the
        # changelevel frame's stage and the accumulated one written at
        # its first frame; c1a0, timed from no load note, has none.
        for source, rec in self._recs():
            loads = fr.load_stage_rows(rec)
            if source != "flag":
                self.assertEqual(loads, [])
                continue
            self.assertEqual(len(loads), 1)
            ld = loads[0]
            self.assertEqual(ld["map"], "c1a0e")
            self.assertAlmostEqual(ld["load_ms"], 1500.0)
            self.assertEqual(ld["msgs"], [])
            a, b = ld["stages"]
            self.assertEqual((a["stage"], a["io"], a["opens"], a["reads"], a["seeks"], a["probes"], a["writes"], a["meta"],
                              a["count"]), ("sv_changelevel", 100, 2, 10, 3, 4, 5, 6, 0))
            self.assertAlmostEqual(a["ms"], 480.0)
            self.assertAlmostEqual(a["io_ms"], 100.0)
            self.assertAlmostEqual(a["cpu_ms"], 380.0)
            self.assertAlmostEqual(a["kib"], 2048.0)
            self.assertEqual((b["stage"], b["count"], b["opens"]), ("acc_studio", 5, 5))
            self.assertAlmostEqual(b["cpu_ms"], 40.0)
            # the load notes stay out of the generic notes list
            self.assertEqual(sorted(n["name"] for n in fr.notes_list(rec)), ["fvox/flatline.wav", "svc_sound"])
        self.assertIsNone(fr.parse_load_name("svc_sound"))
        self.assertEqual(fr.parse_load_name("x y 1,2,3,4,5,6,8,7"),
                         {"stage": "x y", "io": 1, "opens": 2, "reads": 3, "seeks": 4, "probes": 5, "writes": 6, "meta": 8,
                          "count": 7})
        # the first build's labelled form (b64de841) still reads, meta 0
        self.assertEqual(fr.parse_load_name("x y i1 o2 r3 s4 p5 w6 n7"),
                         {"stage": "x y", "io": 1, "opens": 2, "reads": 3, "seeks": 4, "probes": 5, "writes": 6, "meta": 0,
                          "count": 7})

    def test_save_pump_runs(self):
        # The deferred .sav write (2026-10-06, #153): seq 36-38 wrote it,
        # the last of them in a flush.
        for source, rec in self._recs():
            runs = fr.save_pump_runs(rec)
            if source != "flag":
                self.assertEqual(runs, [])
                continue
            self.assertEqual(len(runs), 1)
            r = runs[0]
            self.assertEqual((r["map"], r["frames"], r["kib"]), ("c1a0", 3, 124))
            self.assertAlmostEqual(r["t"], (35 * 20 + 600) / 1000.0)
            self.assertAlmostEqual(r["pump_max"], 8.0)
            self.assertAlmostEqual(r["pump_mean"], 14.0 / 3)
            self.assertAlmostEqual(r["flush_max"], 30.0)
            self.assertAlmostEqual(r["frame_max"], 500.0)
            rows = [fr.segment_stats(rec, s) for s in fr.segments(rec)]
            text = fr.summary_text(rec, rows, fr.session_stats(rec, rows))
            self.assertIn("deferred savegame writes: 1", text)
            self.assertRegex(text, r"c1a0 +3 +124 +8\.0 +4\.7 +30\.0 +500\.0")

    def test_stall_that_opens_the_menu(self):
        # A frame's flags are the state it starts in: with the frame after
        # seq 83 starting in a menu, seq 83 is the frame that opened it.
        for source, rec in self._recs():
            f = rec["frames"][84]
            rec["frames"][84] = f[:7] + (f[7] | fr.F_MENU,) + f[8:]
            st = fr.stall_list(rec)
            self.assertEqual([(s["seq"], s["opens_menu"]) for s in st], [(35, False), (83, True)], source)
            rows = [fr.segment_stats(rec, s) for s in fr.segments(rec)]
            text = fr.summary_text(rec, rows, fr.session_stats(rec, rows))
            self.assertRegex(text, r"\n +[0-9.]+ +c1a0e .*opens-menu")

    def test_summary_and_page(self):
        for source, rec in self._recs():
            rows = [fr.segment_stats(rec, s) for s in fr.segments(rec)]
            s = fr.session_stats(rec, rows)
            self.assertEqual(len(s["level_change"]), 1)
            self.assertEqual(len(s["stalls"]), 2)
            text = fr.summary_text(rec, rows, s)
            self.assertIn("level-change frames: 1", text)
            self.assertIn("stalls: 2 in-play frame(s) over 250 ms", text)
            if source == "flag":
                self.assertIn("save_sav 400, save_ents 100, save 30", text)
                self.assertIn("sounds loaded 2 (300 KiB, 250 ms); textures loaded 6 (4096 KiB); messages parsed in 50 ms", text)
                self.assertIn("message svc_sound 200 ms, sound fvox/flatline.wav 120 ms", text)
                self.assertIn("notes: 2 named events (sounds loaded in play, server messages over 5 ms, the menu font restore at a map load; "
                              "the load stages have their own table), 1 dropped", text)
                # The load stages (2026-10-08, #162), in their own table.
                self.assertIn("level loads by stage", text)
                self.assertIn("c1a0e: load 1500 ms", text)
                self.assertRegex(text, r"sv_changelevel +480\.0 +100\.0 +380\.0 +2048 +2 +10 +3 +4 +5 +6 +\n")
                self.assertRegex(text, r"acc_studio +60\.0 +20\.0 +40\.0 +512 +5 +6 +7 +8 +0 +1 +5\n")
                self.assertRegex(text, r"c1a0 +message +svc_sound +200\.0 +0\.0")
                self.assertRegex(text, r"c1a0 +sound +fvox/flatline\.wav +120\.0 +128\.0")
            else:
                self.assertNotIn("notes:", text)
            self.assertIn("c1a0 -> c1a0e", text)
            self.assertIn("LOAD flag" if source == "flag" else "shape (unflagged file)", text)
            page = fr.render_html(rec, rows, s, ["lc.bin"])
            self.assertEqual(re.findall(r"__[A-Z]+__", page), [])
            self.assertIn("Level changes and stalls", page)
            self.assertIn("c1a0 &rarr; c1a0e", page)
            if source == "flag":
                self.assertIn("save_sav 400, save_ents 100, save 30", page)
                self.assertIn("sounds loaded 2 (300 KiB, 250 ms)", page)
                self.assertIn("message svc_sound 200 ms", page)
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
    statistics use seq 30..49 (the first 30 in play are the warm-up).
    sv_game (2026-10-05): sv_start 1 + sv_pstep 2 (seq 40: 8) + 1 own;
    g_thinks 30, g_moves 200, g_worst_kc 1 (seq 40: 6), g_worst_cls 2,
    g_worst_ent 57; a CLASSES chunk closing the map."""

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
        self.assertEqual(len(r["counters"]), 241)
        self.assertEqual(r["counters"][20], "span_calls")
        self.assertEqual(len(r["spans"]), 73)
        # Counts after the spans (2026-10-02, R5) are plain counters.
        self.assertEqual(r["counters"][94:97], ["world_walked", "water_gpu", "vp_loads"])
        self.assertEqual(r["counters"][97], "g_ents")
        self.assertEqual(r["counters"][113], "g_push_replays")
        self.assertEqual(r["counters"][114:122], ["snd_loads", "snd_load_kb", "snd_load_kc", "tex_loads", "tex_load_kb",
                                                  "cl_parse_kc", "save_pump_kb", "save_flush_kc"])
        # Player movement (2026-10-06): server/client twins, then the times.
        self.assertEqual(r["counters"][122:126], ["pm_sv_cmds", "pm_cl_cmds", "pm_cl_redo", "pm_cl_solids"])
        self.assertEqual(r["counters"][126:128], ["pm_sv_physents", "pm_cl_physents"])
        self.assertEqual(r["counters"][142], "pm_sv_setup_kc")
        self.assertEqual(r["counters"][150:153], ["pm_cl_post_kc", "pm_sv_culls", "pm_cl_culls"])
        # Client entities (2026-10-07): the counts, then the times.
        self.assertEqual(r["counters"][153:158], ["ce_ents", "ce_deltas", "ce_nochange", "ce_fields", "ce_visents"])
        self.assertEqual(r["counters"][158:], ["ce_delta_kc", "ce_fields_kc", "ce_process_kc", "ce_link_kc",
                                               "ce_add_kc", "ce_tents_kc", "ce_events_kc", "ce_pitch_kc",
                                               "ce_solid_skips", "tex_fast", "part_kc", "part_think_kc",
                                               "beam_kc", "beam_noise_kc", "beam_geom_kc", "beam_ents", "beam_prims",
                                               "beam_pieces", "tracers", "tracer_kc", "tracer_fails",
                                               "chrome_meshes", "chrome_verts", "chrome_kc", "chrome_gpu",
                                               "music_reads", "music_read_kb", "music_async", "music_waits",
                                               "music_io_kc", "music_read_kc",
                                               "snd_thr_kc", "snd_thr_runs", "snd_bufs", "snd_underruns",
                                               "snd_dma_kc", "snd_rate", "snd_backend",
                                               "vox_starts", "vox_slots", "vox_opens", "vox_open_kc", "vox_reads",
                                               "vox_read_kb", "vox_async", "vox_sync", "vox_waits", "vox_wait_kc",
                                               "vox_seeks", "vox_io_kc", "vox_first_kc", "vox_pf_hits", "vox_small",
                                               "vox_fallbacks", "vox_window_kb",
                                               "br_ents", "br_culled", "br_trans", "br_moved", "br_surfs",
                                               "br_faces", "br_back", "br_binds", "br_face_kc", "br_decal_kc",
                                               "br_trans_kc", "br_moved_kc", "br_worst_kc", "br_worst_mdl",
                                               "st_ents", "st_culled", "st_parts", "st_parts_cpu", "st_verts_cpu",
                                               "st_xform_kc", "st_submit_kc", "st_light_kc", "st_bbox_kc",
                                               "st_worst_kc", "st_worst_mdl", "st_consts_kc", "st_skin_kc",
                                               "st_mesh_kc"])
        self.assertNotIn("ce_ents", fr.span_names(r))
        self.assertNotIn("pm_sv_cmds", fr.span_names(r))
        self.assertNotIn("snd_loads", fr.span_names(r))
        self.assertNotIn("world_walked", fr.span_names(r))
        self.assertNotIn("vp_loads", fr.span_names(r))
        self.assertEqual(r["frames"][0][8][r["counters"].index("world_walked")], 500)
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
        # The server game frame's breakdown (2026-10-05).
        self.assertEqual(parent["k_sv_pstep"], "k_sv_game")
        self.assertEqual(parent["k_sv_pother"], "k_sv_game")
        self.assertEqual(fr.span_depth(r)["k_sv_start"], 2)
        self.assertLess(order.index("k_sv_game"), order.index("k_sv_start"))
        self.assertLess(order.index("k_sv_pother"), order.index("k_sv_send"))
        # The savegame's breakdown (2026-10-05): under k_cl_begin and k_scr.
        self.assertEqual(parent["k_save"], "k_cl_begin")
        self.assertEqual(parent["k_save_sav"], "k_save")
        self.assertEqual(parent["k_scrshot"], "k_scr")
        self.assertEqual(parent["k_shot_write"], "k_scrshot")
        self.assertEqual(fr.span_depth(r)["k_save_ents"], 2)
        self.assertEqual(fr.span_depth(r)["k_shot_read"], 3)
        self.assertLess(order.index("k_cl_wpacket"), order.index("k_save"))
        self.assertLess(order.index("k_save_sav"), order.index("k_sv"))
        self.assertLess(order.index("k_r_end"), order.index("k_scrshot"))
        # The menu and the music (2026-10-06): k_ui under k_scr, its two
        # services under it; k_snd_music under k_snd.
        self.assertEqual(parent["k_ui"], "k_scr")
        self.assertEqual(parent["k_ui_file"], "k_ui")
        self.assertEqual(parent["k_ui_pic"], "k_ui")
        self.assertEqual(parent["k_snd_music"], "k_snd")
        self.assertEqual(fr.span_depth(r)["k_ui_pic"], 3)
        self.assertLess(order.index("k_shot_write"), order.index("k_ui"))
        self.assertLess(order.index("k_ui_pic"), order.index("k_snd"))
        self.assertLess(order.index("k_snd"), order.index("k_snd_music"))
        # cl_parse_kc (2026-10-06) is a time counter, not a span.
        self.assertNotIn("cl_parse_kc", fr.span_names(r))
        # The deferred savegame write (2026-10-06, #153): a top-level span
        # and two counters.
        self.assertEqual(parent["k_save_pump"], "")
        self.assertEqual(fr.span_depth(r)["k_save_pump"], 0)
        self.assertNotIn("save_pump_kb", fr.span_names(r))
        self.assertNotIn("save_flush_kc", fr.span_names(r))

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
        self.assertAlmostEqual(ms["k_sv_pstep"], (19 * 2 + 8) / 20.0)
        self.assertAlmostEqual(own["k_sv_game"], 1.0)
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
        self.assertIn("g_moves", names)
        self.assertFalse([n for n in names if n.startswith("k_")])
        # The g_* times and labels are not counts either, nor is snd_load_kc.
        self.assertFalse([n for n in names if n.endswith("_kc") or n.startswith("g_worst") or n.endswith("_worst_mdl")])
        self.assertIn("snd_loads", names)

    def test_classes_chunk(self):
        r = self.rec
        self.assertEqual(len(r["classes"]), 1)
        self.assertEqual(r["classes"][0]["frame"], 50)
        self.assertEqual(fr.class_names(r), ["func_tracktrain", "monster_scientist", "info_target"])
        row = r["classes"][0]["rows"][1]
        self.assertEqual((row["movetype"], row["runs"], row["thinks"], row["moves"]), (4, 100, 20, 400))
        self.assertEqual((row["phys_kc"], row["think_kc"], row["worst_kc"]), (106, 90, 8))
        # Per frame over the 50 frames that started in play, warm-up included;
        # the class nothing ran is left out; most time first.
        a = self.row
        self.assertEqual(a["class_frames"], 50)
        self.assertEqual([c["name"] for c in a["classes"]], ["monster_scientist", "func_tracktrain"])
        sci = a["classes"][0]
        self.assertAlmostEqual(sci["phys_ms"], 106 / 50.0)
        self.assertAlmostEqual(sci["think_ms"], 90 / 50.0)
        self.assertAlmostEqual(sci["worst_ms"], 8.0)
        self.assertAlmostEqual(sci["runs_pf"], 2.0)
        self.assertAlmostEqual(sci["moves_pf"], 8.0)

    def test_classes_need_their_segment(self):
        # A chunk at a segment's start belongs to the segment before it.
        seg = {"start": 50, "end": 60}
        self.assertEqual(fr.segment_classes(self.rec, seg), [])
        self.assertEqual(len(fr.segment_classes(self.rec, {"start": 0, "end": 50})), 2)

    def test_worst_entity(self):
        top = fr.top_frames(self.rec, "k_sv_game", 1)
        self.assertEqual(top[0]["seq"], 40)
        self.assertEqual(fr.worst_entity(self.rec, top[0]), "monster_scientist #57 6.00")

    def test_summary_and_page(self):
        s = fr.session_stats(self.rec, self.rows)
        text = fr.summary_text(self.rec, self.rows, s)
        self.assertIn("CPU spans", text)
        self.assertIn("unattributed", text)
        self.assertIn("top frames by k_sv_game", text)
        self.assertRegex(text, r"\n  sv_game +4\.30/1\.00 +26%")       # depth 1: two spaces
        self.assertRegex(text, r"\n    sv_pstep +2\.30/2\.30 +14%")     # depth 2
        self.assertIn("monster_scientist #57 6.00", text)
        self.assertIn("sv_game work per in-play frame", text)
        self.assertRegex(text, r"c1a0c: top classnames by time in SV_Physics_Entity, over 50 frames in play")
        self.assertRegex(text, r"\nmonster_scientist +step +2\.0 +0\.40 +8\.0 +2\.120 +1\.800 +8\.00")
        # Player movement: server/client per column, then the times.
        self.assertIn("player movement per in-play frame", text)
        self.assertRegex(text, r"\nc1a0c +1/1 +5/40 +6/50 +8/9 +40/360 +2/300 +1/1 +5/40 +3/3 +1/0 +0 +2\n")
        self.assertIn("c1a0c              sv 1.000 2.000 (1.000) 1.000 0.000 | cl 1.000 3.000 (2.000) 1.000", text)
        # Client entities: the counts, then where k_cl_pkt_ents and k_cl_emit went.
        self.assertIn("client entities per in-play frame", text)
        self.assertRegex(text, r"\nc1a0c +120 +30 +2 +150 +5\.0 +40 +2/2\n")
        self.assertIn("c1a0c              pkt 1.000: deltas 2.000 (fields 1.000) process 1.000 | "
                      "emit 0.000: link 3.000 (add 2.000) tents 1.000 events 0.000 pitch 1.000", text)
        # Particles: 40 drawn, 2 ms in CL_DrawParticles of which 0.5 thinking.
        self.assertIn("particles per in-play frame", text)
        self.assertRegex(text, r"\nc1a0c +40/40 +0\.000 +2\.000 +0\.500 +1\.500 +50\.00\n")
        # Beams and tracers: 6 counted, 2 of them entity submissions, so 4
        # drawn; 4 ms in
        # CL_DrawBeams (noise 1, geometry 2, the rest 1), 200 primitives
        # cut into 250, 10 tracers in 1 ms.
        self.assertIn("beams and tracers per in-play frame", text)
        self.assertRegex(text, r"\nc1a0c +4/2 +4\.000 +1\.000 +2\.000 +1\.000 +200 +250 +1000\.00 +10 +1\.000 +100\.00(\n|$)")
        # The audio output (#173): 48 kHz, a callback and a buffer a
        # frame, no underrun, the audio thread 0.5 ms a frame.
        self.assertIn("audio output per in-play frame", text)
        self.assertRegex(text, r"\nc1a0c +48000 +sdl +1\.000 +1\.000 +0 +0\.500 +0\.000 ")
        # The dialogue streams (#177), totals over the frames the map's
        # statistics cover (seq 30-49, after the warm-up): 2 starts, both
        # on a prefetch, 1 fallback, 2 opens, 40 reads of 160 KiB (20
        # queued, 20 at once), no wait, no jump; 4 ms of starts, 2 of opens,
        # 20 of file calls; 3 slots at most, 64 KiB.
        self.assertIn("dialogue per map", text)
        self.assertRegex(text, r"\nc1a0c +2 +2 +0 +1 +2 +40 +160 +20 +20 +0 +0 +4\.0 +2\.0 +0\.0 +20\.0 +3 +64\n")
        self.assertIn("sounds S_LoadSound loaded in play, by caller", text)
        self.assertRegex(text, r"\nc1a0c +1/4\.0/1 +1/2\.0/1 +- +- +- +- +-(\n|$)")
        self.assertEqual(fr.parse_sound_name("vox/hello w:1500,1,3,0,2"),
                         {"sound": "vox/hello", "tag": "w", "io": 1500, "opens": 1, "reads": 3, "seeks": 0, "probes": 2})
        self.assertEqual(fr.parse_sound_name("fvox/flatline.wav"), {"sound": "fvox/flatline.wav", "tag": None})
        self.assertEqual(fr.NOTE_KINDS[6], "vox")
        page = fr.render_html(self.rec, self.rows, s, ["spans.bin"], source="xemu")
        self.assertEqual(re.findall(r"__[A-Z]+__", page), [])
        self.assertIn('id="spans"', page)
        self.assertIn("CPU by subsystem", page)
        self.assertIn("Recorded in xemu:</b> these are the emulator", page)
        self.assertIn("The ten frames with the most sv_game", page)
        self.assertIn("sv_game by classname: c1a0c", page)
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


SESSIONS = os.environ.get("FRAMETIME_SESSIONS", "")
FIXTURE_SINGLE = os.environ.get("FRAMETIME_FIXTURE_SINGLE", "")


class Sessions(unittest.TestCase):
    """Format v3 (2026-10-07): write_session in the C test wrote one
    recording as the recorder's four segments (FRAMETIME_SESSIONS, label
    "pr190-clents") and as the single file the recorder wrote before
    segments (FRAMETIME_FIXTURE_SINGLE), plus a second session, "pr189
    base", 5 ms slower per frame in play. Segments: 000 the menu frames
    0..9, 001 c1a0 (10..63: the changelevel frame 57, the loading frames
    59..63), 002 c1a0e 64..99 (seq 80 lost) closed by the size cap, 003
    c1a0e continued, 100..139."""

    @classmethod
    def setUpClass(cls):
        if not SESSIONS or not os.path.isdir(SESSIONS):
            raise unittest.SkipTest("FRAMETIME_SESSIONS not set - run tools/host-tests/run.sh")
        cls.single = fr.parse(_read(FIXTURE_SINGLE, "FRAMETIME_FIXTURE_SINGLE"))
        cls.found = fr.collect([SESSIONS])
        cls.names = sorted(os.path.basename(p) for p, _ in cls.found)
        cls.segs = fr.select_session(cls.found, label="pr190-clents")
        cls.seg = {r["session"]["seq"]: r for r in cls.segs}

    def test_names(self):
        self.assertEqual(len(self.names), 8)
        for n in self.names:
            self.assertLessEqual(len(n), 42)                 # FATX
            label, short, seq = fr.parse_segment_name(n)
            self.assertIn(label, ("pr190-clents", "pr189-base"))   # "pr189 base", sanitised as the C did
            self.assertEqual(n, fr.segment_name(label, short, seq))
        r = self.seg[2]
        self.assertEqual(os.path.basename(r["path"]), "ft_pr190-clents_%s_002.bin" % r["session"]["short"])
        self.assertIsNone(fr.parse_segment_name("ft_000.bin"))
        self.assertEqual(fr.parse_segment_name("ft_a_b_c_0abcdef1_012.bin"), ("a_b_c", "0abcdef1", 12))
        # The Python sanitiser is the C one (the C test's cases).
        for raw, want in (("PR #190: clents!", "PR-190-clents"), ("  --x9 perf.saves__ ", "x9-perf-saves"),
                          ("", "untagged"), (None, "untagged"), (" .:/ ", "untagged"),
                          ("abcdefghijklmnopqrstuvwxyz", "abcdefghijklmnopqrstuv"),
                          ("abcdefghijklmnopqrstu_vwxyz", "abcdefghijklmnopqrstu"), ("pr189 base", "pr189-base")):
            self.assertEqual(fr.sanitise_label(raw), want, raw)
        self.assertEqual(len(fr.segment_name("x" * 40, "0abcdef1", 5000)), 42)

    def test_fetch_filters(self):
        n = self.names[0]
        self.assertTrue(fr.name_wanted("ft_000.bin"))
        self.assertTrue(fr.name_wanted(n))
        self.assertFalse(fr.name_wanted("ft_000.bin", label="pr190-clents"))   # single files carry no label
        self.assertFalse(fr.name_wanted("notes.txt"))
        short = self.seg[0]["session"]["short"]
        self.assertTrue(fr.name_wanted("ft_pr190-clents_%s_003.bin" % short, label="pr190 clents"))
        self.assertTrue(fr.name_wanted("ft_pr190-clents_%s_003.bin" % short, session=short[:4].upper()))
        self.assertFalse(fr.name_wanted("ft_pr190-clents_%s_003.bin" % short, label="pr189-base"))
        self.assertFalse(fr.name_wanted("ft_pr190-clents_%s_003.bin" % short, label="pr190-clents", session="ffff"))
        # Only a v3 name of the same size counts as fetched.
        path = self.seg[3]["path"]
        size = os.path.getsize(path)
        self.assertTrue(fr.already_fetched(path, os.path.basename(path), size))
        self.assertFalse(fr.already_fetched(path, os.path.basename(path), size + 1))
        self.assertFalse(fr.already_fetched(path, os.path.basename(path), None))
        self.assertFalse(fr.already_fetched(FIXTURE_SINGLE, "ft_000.bin", os.path.getsize(FIXTURE_SINGLE)))

    def test_v3_header(self):
        r = self.seg[2]
        s = r["session"]
        self.assertEqual(r["version"], 3)
        self.assertEqual((s["seq"], s["reason"], s["first_frame"], s["label"]), (2, fr.SEG_MAP, 64, "pr190-clents"))
        self.assertEqual(len(s["id"]), 36)
        self.assertEqual(s["id"][14], "4")                   # a version-4 UUID
        self.assertTrue(s["id"].startswith(s["short"]))
        self.assertEqual(fr.systime_date(s["systime"]).year, 2022)
        self.assertEqual(r["end"], {"tsc": r["end"]["tsc"], "lost": 1, "next": fr.SEG_SIZE})
        self.assertEqual(len(r["counters"]), 241)            # every segment has its own COUNTERS ...
        self.assertEqual(len(r["spans"]), 73)                # ... and SPANS
        self.assertEqual(self.seg[3]["session"]["reason"], fr.SEG_SIZE)
        self.assertEqual(self.seg[3]["end"]["next"], fr.SEG_SESSION)
        self.assertEqual(self.seg[0]["end"]["next"], fr.SEG_MAP)

    def _strip(self, sess):
        return {k: v for k, v in sess.items() if k not in ("session", "version", "continues")}

    def test_stitched_equals_single_file(self):
        st = fr.stitch(self.segs)
        self.assertEqual(st["session"]["segments"], [0, 1, 2, 3])
        self.assertEqual(st["session"]["issues"], [])
        self.assertEqual(st["frames"], self.single["frames"])
        self.assertEqual([(m["name"], m["frame"]) for m in st["maps"]], [("c1a0", 10), ("c1a0e", 64)])
        self.assertEqual(st["gaps"], self.single["gaps"])
        self.assertEqual(st["lost_frames"], 1)
        for flush in (False, True):
            for lc in (False, True):
                _, rows_s, sess_s = fr.analyse_rec(fr.stitch(self.segs), flush, lc)
                _, rows_1, sess_1 = fr.analyse_rec(fr.parse(_read(FIXTURE_SINGLE, "")), flush, lc)
                self.assertEqual(rows_s, rows_1)
                self.assertEqual(self._strip(sess_s), self._strip(sess_1))
                # A single file's session dict (and so its page) has no v3 keys.
                self.assertNotIn("session", sess_1)
                self.assertNotIn("continues", sess_1)
                text_s = fr.summary_text(st, rows_s, sess_s).splitlines()
                text_1 = fr.summary_text(self.single, rows_1, sess_1).splitlines()
                self.assertEqual([l for l in text_s if not l.startswith("session ")], text_1)
        # The per-map attribution the single file has: the changelevel frame
        # in c1a0 (its segment), the stall and the notes in their maps, and
        # c1a0e's classes summed over its two segments.
        lc = fr.level_change_list(st)
        self.assertEqual([(r["seq"], r["map"], r["next"]) for r in lc], [(57, "c1a0", "c1a0e")])
        self.assertEqual([(r["seq"], r["map"]) for r in fr.stall_list(st)], [(50, "c1a0"), (120, "c1a0e")])
        self.assertEqual(sorted((n["seq"], n["map"]) for n in fr.notes_list(st)), [(50, "c1a0"), (120, "c1a0e")])
        row = [r for r in rows_s if r["map"] == "c1a0e"][0]
        door = [c for c in row["classes"] if c["name"] == "func_door"][0]
        self.assertEqual((door["runs"], door["phys_kc"], door["worst_kc"]), (100, 800, 9))

    def test_segment_alone(self):
        # 003 alone: the continued map, no warm-up, its own note, a clean end.
        rec, rows, sess = fr.analyse_rec(self.seg[3])
        self.assertEqual([r["map"] for r in rows], ["c1a0e (cont.)"])
        self.assertIsNone(rows[0]["load_s"])
        self.assertEqual(rows[0]["frames"], 40)              # seq 100..139, warm-up already past
        self.assertEqual([n["seq"] for n in rec["notes"]], [120])
        self.assertEqual(sess["continues"], 0)
        text = fr.summary_text(rec, rows, sess)
        self.assertIn("segment 003 alone (began at a size cap)", text)
        self.assertIn("--session %s" % rec["session"]["short"], text)
        # 002 alone: closed by the size cap, the session goes on.
        rec, rows, sess = fr.analyse_rec(self.seg[2])
        self.assertEqual(sess["continues"], fr.SEG_SIZE)
        self.assertIn("this segment closed at a size cap; the session continues in segment 003",
                      fr.summary_text(rec, rows, sess))
        self.assertEqual(sess["lost"], 1)
        # 001 alone: c1a0 with its changelevel frame (no next map in it).
        rec, rows, sess = fr.analyse_rec(self.seg[1])
        self.assertEqual([r["map"] for r in rows], ["c1a0"])
        self.assertEqual([(r["seq"], r["next"]) for r in fr.level_change_list(rec)], [(57, None)])
        self.assertAlmostEqual(rows[0]["load_s"], 2.0)
        # Each segment's statistics for c1a0 are the session's.
        _, rows_s, _ = fr.analyse_rec(fr.stitch(self.segs))
        self.assertEqual(rows[0], [r for r in rows_s if r["map"] == "c1a0"][0])

    def test_seams(self):
        # A missing segment: listed, its frames counted lost, and the
        # continued map kept (the map before it is another).
        st = fr.stitch([self.seg[0], self.seg[1], self.seg[3]])
        self.assertIn("segment(s) 002-002 missing", st["session"]["issues"])
        self.assertEqual(st["lost_frames"], 100 - 64)
        self.assertEqual([m["name"] for m in fr.segments(st)][-1], "c1a0e (cont.)")
        # The session's start missing, and its end not fetched yet.
        st = fr.stitch([self.seg[1], self.seg[2]])
        self.assertEqual(st["session"]["issues"],
                         ["segment(s) 000-000 missing: the session's start",
                          "segment 002 says segment 003 follows (size cap): not fetched yet?"])
        rows = [fr.segment_stats(st, s) for s in fr.segments(st)]
        self.assertIn("WARNING: segment 002 says segment 003 follows", fr.summary_text(st, rows, fr.session_stats(st, rows)))
        # Two sessions, a file given twice: refused.
        other = fr.select_session(self.found, label="pr189-base")
        with self.assertRaises(fr.FormatError):
            fr.stitch(self.segs + other)
        with self.assertRaises(fr.FormatError):
            fr.stitch([self.seg[1], self.seg[1]])
        with self.assertRaises(fr.FormatError):
            fr.stitch([self.single])

    def test_select(self):
        short = self.seg[0]["session"]["short"]
        self.assertEqual(len(fr.select_session(self.found, token=short[:3])), 4)
        self.assertEqual(fr.select_session(self.found, token="pr189 base")[0]["session"]["label"], "pr189-base")
        with self.assertRaises(fr.FormatError):
            fr.select_session(self.found, token="nothing")
        # A folder with two sessions needs a filter; one file is read alone.
        with self.assertRaises(fr.FormatError):
            fr.resolve([SESSIONS])
        rec, files = fr.resolve([SESSIONS], session=short)
        self.assertEqual(len(files), 4)
        rec, files = fr.resolve([self.seg[2]["path"]])
        self.assertFalse(rec["session"]["stitched"])

    def test_compare(self):
        a = fr.analyse_rec(fr.stitch(fr.select_session(self.found, token="pr189-base")))
        b = fr.analyse_rec(fr.stitch(self.segs))
        rows = fr.compare_rows(a[1], b[1])
        self.assertEqual([k for k, _, _ in rows], ["c1a0", "c1a0e"])
        # c1a0e in play past the warm-up: seq 94..139 but the lost 80 - 45
        # frames, one of them the 300 ms seq 120, the rest 25 ms (A) or 20 ms (B).
        self.assertAlmostEqual(rows[1][1]["avg_fps"], 45 / 1.400)
        self.assertAlmostEqual(rows[1][2]["avg_fps"], 45 / 1.180)
        text = fr.compare_text("pr189-base", a[2], a[1], "pr190-clents", b[2], b[1])
        self.assertRegex(text, r"\nc1a0e +\d+/\d+ +32\.1 +38\.1 +\+18\.6 ")
        self.assertIn("label pr189-base segments 4", text)
        # A map played twice is two rows.
        twice = fr.compare_rows(a[1] + [r for r in a[1] if r["map"] == "c1a0e"], b[1])
        self.assertEqual([k for k, _, _ in twice], ["c1a0", "c1a0e", "c1a0e #2"])
        self.assertIsNone(twice[2][2])

    def test_cli(self):
        tool = os.path.join(_HERE, "frametime-report.py")

        def run(*args):
            return subprocess.run([sys.executable, tool] + list(args), capture_output=True, text=True, timeout=60)
        out = run("summary", SESSIONS, "--label", "pr190-clents")
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("segments 000-003 stitched (4 files)", out.stdout)
        out = run("summary", SESSIONS, "--compare", "pr189-base", "pr190-clents")
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("compare: A = pr189-base, B = pr190-clents", out.stdout)
        out = run("summary", SESSIONS)
        self.assertEqual(out.returncode, 1)
        self.assertIn("pick one with --label or --session", out.stderr)
        out = run("sessions", SESSIONS)
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertEqual(len(out.stdout.splitlines()), 2)
        out = run("summary", self.seg[3]["path"])
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("c1a0e (cont.)", out.stdout)
        with tempfile.TemporaryDirectory() as d:
            page = os.path.join(d, "s.html")
            out = run("report", SESSIONS, "--session", self.seg[0]["session"]["short"], "-o", page)
            self.assertEqual(out.returncode, 0, out.stderr)
            with open(page, encoding="utf-8") as f:
                html_page = f.read()
            self.assertEqual(re.findall(r"__[A-Z]+__", html_page), [])
            self.assertIn("pr190-clents", html_page)
            run_page_script(html_page)


class RouteCheck(unittest.TestCase):
    """`check` (tools/xemu-route-*.sh): a recording that does not show the
    route ran must fail, whatever else is in it. Synthetic records with
    only the fields route_problems reads."""

    COUNTERS = ["draws", "vox_starts"]

    def rec(self, maps, starts=None, end=True, truncated=False, counters=None):
        """`maps` [(name, frames, flags)]; starts {map: vox_starts per frame}."""
        starts = starts or {}
        frames, mlist = [], []
        for name, n, flags in maps:
            mlist.append({"name": name, "frame": len(frames), "flags": flags})
            for _ in range(n):
                frames.append((len(frames), 0, 0, 0, 0, 0, 0, 0, [1, starts.get(name, 0)]))
        return {"maps": mlist, "frames": frames, "counters": list(counters or self.COUNTERS),
                "end": {} if end else None, "truncated": truncated}

    def good(self):
        return self.rec([("c1a0", 10, 0), ("c1a0e", 10, 0), ("c1a0e", 5, 0), ("c1a1", 10, 0)],
                        starts={"c1a0": 1, "c1a0e": 1})

    MAPS = ["c1a0", "c1a0e", "c1a1"]

    def test_route_that_ran(self):
        r = self.good()
        self.assertEqual(fr.map_sequence(r), self.MAPS)
        self.assertEqual(fr.counter_totals(r, "vox_starts"), {"c1a0": 10, "c1a0e": 15, "c1a1": 0})
        mins = [fr.parse_min("c1a0:vox_starts:1"), fr.parse_min("c1a0e:vox_starts:15")]
        # the end marker: a session with no map of its own (the map was
        # already loaded when it started)
        marker = {"maps": [], "frames": [(0, 0, 0, 0, 0, 0, 0, 0, [1, 0])] * 30, "counters": list(self.COUNTERS),
                  "end": {}, "truncated": False}
        self.assertEqual(fr.route_problems(r, self.MAPS, mins, end_rec=marker), [])

    def test_stalled_route(self):
        r = self.rec([("c1a0", 10, 0)], starts={"c1a0": 1})
        p = fr.route_problems(r, self.MAPS)
        self.assertEqual(len(p), 1)
        self.assertIn("maps played c1a0, expected c1a0 -> c1a0e -> c1a1", p[0])

    def test_wrong_order_and_extra_map(self):
        r = self.rec([("c1a0", 1, 0), ("c1a1", 1, 0), ("c1a0e", 1, 0)])
        self.assertTrue(fr.route_problems(r, self.MAPS))
        r = self.rec([("c1a0", 1, 0), ("c1a0e", 1, 0), ("c1a1", 1, 0), ("c2a1", 1, 0)])
        self.assertTrue(fr.route_problems(r, self.MAPS))

    def test_cut_short_and_truncated(self):
        r = self.rec([("c1a0", 1, 0), ("c1a0e", 1, 0), ("c1a1", 1, 0)], end=False)
        self.assertIn("no END chunk", fr.route_problems(r, self.MAPS)[0])
        r = self.rec([("c1a0", 1, 0), ("c1a0e", 1, 0), ("c1a1", 1, 0)], truncated=True)
        self.assertIn("truncated", fr.route_problems(r, self.MAPS)[0])

    def test_counter_minimum(self):
        r = self.good()
        p = fr.route_problems(r, self.MAPS, [fr.parse_min("c1a1:vox_starts:1")])
        self.assertEqual(p, ["vox_starts on c1a1: 0, expected at least 1"])
        p = fr.route_problems(r, self.MAPS, [fr.parse_min("c3a1:vox_starts:1")])
        self.assertEqual(p, ["vox_starts on c3a1: 0, expected at least 1"])

    def test_missing_counter(self):
        r = self.rec([("c1a0", 1, 0), ("c1a0e", 1, 0), ("c1a1", 1, 0)], counters=["draws"])
        p = fr.route_problems(r, self.MAPS, [fr.parse_min("c1a0:vox_starts:1")])
        self.assertIn("no vox_starts counter", p[0])
        # optional: only from a build that has it
        self.assertEqual(fr.route_problems(r, self.MAPS, [fr.parse_min("c1a0:vox_starts:1?")]), [])
        r = self.good()
        self.assertTrue(fr.route_problems(r, self.MAPS, [fr.parse_min("c1a1:vox_starts:1?")]))

    def test_end_marker(self):
        r = self.good()
        self.assertIn("no end-marker", fr.route_problems(r, self.MAPS, end_rec=False)[0])
        empty = {"maps": [], "frames": [], "counters": [], "end": {}, "truncated": False}
        self.assertIn("end-marker recording is empty", fr.route_problems(r, self.MAPS, end_rec=empty)[0])
        cut = self.rec([("c1a1", 3, 0)], end=False)
        self.assertIn("end-marker recording is empty or cut short", fr.route_problems(r, self.MAPS, end_rec=cut)[0])

    def test_continued_map_is_not_a_step(self):
        r = self.rec([("c1a0", 2, 0), ("c1a0", 2, fr.MAP_F_CONTINUED), ("c1a0e", 1, 0), ("c1a1", 1, 0)])
        self.assertEqual(fr.map_sequence(r), self.MAPS)

    def test_parse_min(self):
        self.assertEqual(fr.parse_min("c1a0:vox_starts:3"), ("c1a0", "vox_starts", 3, False))
        self.assertEqual(fr.parse_min("c1a0:vox_starts:3?"), ("c1a0", "vox_starts", 3, True))
        for bad in ("c1a0:vox_starts", ":vox_starts:1", "c1a0::1", "c1a0:vox_starts:x"):
            with self.assertRaises(ValueError):
                fr.parse_min(bad)


class BrushStudio(unittest.TestCase):
    """The brush and studio split (x9-perf-cpuprof): the two summary
    sections, from rows segment_stats would build."""

    def test_sections(self):
        rec = {"counters": ["br_ents", "br_faces", "br_face_kc", "st_ents", "st_light_kc", "st_bbox_kc",
                            "st_worst_kc", "st_worst_mdl"], "tsc_hz": 1e9, "notes": []}
        row = {"map": "c1a2", "frames": 4,
               "work_avg": {"br_ents": 84.0, "br_faces": 200.0, "br_face_kc": 1000.0,
                            "st_ents": 50.0, "st_light_kc": 500.0, "st_bbox_kc": 100.0},
               "work_peak": {"br_ents": 90.0, "st_ents": 97.0},
               "span_ms": {"k_brush": 2.0, "k_client_studio": 5.0, "k_studio_points": 1.5},
               "st_worst": [[12, 3, 6000.0, 2500.0], [7, 1, 900.0, 900.0]],
               "st_names": {12: "models/scientist.mdl"}}
        brush = fr.brush_text(rec, [row])
        self.assertEqual(brush[2].split()[0], "map")
        cols = brush[3].split()
        self.assertEqual(cols[:2], ["c1a2", "84/90"])
        self.assertIn("1.000", cols)                       # the face loops, 1000 kc at 1 GHz
        self.assertEqual(cols[-2], "5.0")                  # us per face
        studio = fr.studio_text(rec, [row])
        line = studio[3]
        self.assertIn("50/97", line)
        self.assertIn(" 2.900 ", line)                     # bones: 5.0 - 1.5 - 0.5 - 0.1
        self.assertIn("scientist 75% 2.00, #7 25% 0.90", line)
        self.assertEqual(fr.brush_text({"counters": ["draws"], "tsc_hz": 1e9}, [row]), [])


if __name__ == "__main__":
    res = unittest.main(exit=False, verbosity=1).result
    # A skipped fixture is a broken run, not a pass.
    if res.skipped or not res.wasSuccessful():
        sys.exit(1)
