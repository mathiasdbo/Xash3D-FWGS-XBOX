"""Frame-time recordings (host_frametime_analysis) -> fetch, summarise, HTML report.

    # 1. fetch the recordings off the console over FTP (read-only: LIST/RETR)
    python tools/frametime-report.py fetch --host <console IP> --out recordings/
    #    (the console gets its address from DHCP: read it on the dashboard)
    #    ... or off xemu's hard disk image (xemu stopped)
    python tools/frametime-report.py fetch-xemu --out recordings/
    # 2. the per-map table in the terminal
    python tools/frametime-report.py summary recordings/ft_000.bin
    # 3. the HTML report
    python tools/frametime-report.py report recordings/ft_000.bin -o ft_000.html
    #    (--include-flush keeps the frames that wrote the file in the statistics)

The file is written by engine/platform/xbox/xbox_frametime.c; its byte
layout is documented in engine/platform/xbox/xbox_frametime_util.h (and
parse() below follows it field by field). Python 3 standard library only.

What the numbers mean (the recorder's header comment has the detail):
  total   Host_Frame start to the next one: the frame as the player sees it
  cpu     the engine's own work in the frame (host minus the waits below)
  gpu     time the CPU was blocked on the GPU (pb_busy): the GPU's excess
          over the CPU, NOT the GPU's busy time - the NV2A exposes no
          GPU-side timer through pbkit. ~0 means the GPU kept up.
  vblank  pb_wait_for_vbl; swap: pb_finished (all back buffers queued)
  other   between host frames: the engine's frame limiter (fps_max)
  work    (format v2) the renderer's per-frame work counters: draws,
          BEGIN/END blocks, pushbuffer dwords, texture binds, vertices by
          source, lightmap texels rebuilt, models, decals, particles...
Recordings made in xemu time the emulator on the PC, not a Pentium III;
the work counters are counts, the same in xemu as on the console.

Frames that wrote the file (flag FLUSH: every 5 s, and at each map start)
include the write in their time. They are flagged and left out of the
statistics unless --include-flush is given.
"""
import argparse
import ftplib
import html
import json
import math
import os
import struct
import sys

MAGIC = b"R3DFTIME"
VERSIONS = (1, 2)           # 1: no work counters; 2: COUNTERS chunk + counters in each frame
HEADER_BYTES = 128
FRAME_BYTES = 32            # the fixed part of a frame record
COUNTER_NAME_BYTES = 16
CHUNK_FRAMES, CHUNK_MAP, CHUNK_CALIB, CHUNK_END, CHUNK_COUNTERS = 1, 2, 3, 4, 5

F_ACTIVE, F_MENU, F_FLUSH, F_SATURATED, F_OVERLAY = 0x1, 0x2, 0x4, 0x8, 0x10
MAP_F_NO_LOAD_START = 0x1

GUIDE_60_MS = 1000.0 / 60.0
GUIDE_30_MS = 1000.0 / 30.0
WARMUP_FRAMES = 30          # left out of a map's statistics (the overlay does the same)
GPU_BOUND_MS = 1.0          # a frame whose GPU wait exceeds this was held up by the GPU
VERTS = "verts"             # derived counter: the sum of every v_* counter
# The counters the per-map table shows, in order, with their headings.
WORK_TABLE = [("draws", "Draws"), ("prims", "Prims"), ("pb_dwords", "PB KiB"), ("tex_binds", "Tex binds"),
              (VERTS, "Verts"), ("v_studio", "Studio verts"), ("world_faces", "World faces"),
              ("studio_models", "Models"), ("lm_texels", "LM texels"), ("water_verts", "Water verts"),
              ("decals", "Decals"), ("particles", "Particles")]
PAGE_BYTES = 4096

REMOTE_ROOT = "/HDD0-E/UDATA"
REL_DIR = "valve/valve/frametime"   # E:\UDATA\<title id>\valve (the engine root) + valve (the game dir)


class FormatError(Exception):
    pass


# --------------------------------------------------------------------------
# Parsing
# --------------------------------------------------------------------------

def parse(data):
    """Parse a recording. Returns a dict; raises FormatError for a file that
    is not a recording. A file cut short (power-off mid-write) parses up to
    its last complete record, with 'truncated' set."""
    if len(data) < HEADER_BYTES:
        raise FormatError("file too short for a header (%d bytes)" % len(data))
    if data[:8] != MAGIC:
        raise FormatError("not a frame-time recording (magic %r)" % data[:8])
    version, header_bytes, frame_bytes, ring_frames = struct.unpack_from("<IIII", data, 8)
    if version not in VERSIONS:
        raise FormatError("unsupported version %d (this tool reads %s)" % (version, ", ".join(map(str, VERSIONS))))
    if header_bytes < HEADER_BYTES or frame_bytes < FRAME_BYTES or (frame_bytes - FRAME_BYTES) % 4:
        raise FormatError("header says %d-byte header, %d-byte frames" % (header_bytes, frame_bytes))
    # v2: the counters are the u32s after the fixed 32 bytes; a v1 record may
    # only be padded (nothing in it is a counter).
    ncount = (frame_bytes - FRAME_BYTES) // 4 if version >= 2 else 0
    tsc_hz, qpc_hz, tsc_open, qpc_open = struct.unpack_from("<QQQQ", data, 24)
    mode, = struct.unpack_from("<I", data, 56)
    build = data[64:128].split(b"\0", 1)[0].decode("latin-1")

    rec = {
        "version": version, "ring_frames": ring_frames, "tsc_hz_header": tsc_hz,
        "qpc_hz": qpc_hz, "tsc_open": tsc_open, "qpc_open": qpc_open, "mode": mode,
        "build": build, "frames": [], "maps": [], "calibs": [], "end": None,
        "truncated": False, "gaps": 0, "lost_frames": 0, "unknown_chunks": 0,
        "counters": [],
    }
    frames = rec["frames"]
    off = header_bytes
    next_seq = None
    while off < len(data):
        if off + 8 > len(data):
            rec["truncated"] = True
            break
        ctype, clen = struct.unpack_from("<II", data, off)
        body = off + 8
        if ctype == CHUNK_FRAMES:
            if body + 8 > len(data):
                rec["truncated"] = True
                break
            first, count = struct.unpack_from("<II", data, body)
            if clen != 8 + count * frame_bytes:
                raise FormatError("FRAMES chunk at %d: length %d for %d frames" % (off, clen, count))
            avail = (len(data) - body - 8) // frame_bytes
            if avail < count:
                rec["truncated"] = True
                count = avail
            if next_seq is not None and first != next_seq:
                rec["gaps"] += 1
                rec["lost_frames"] += (first - next_seq) & 0xFFFFFFFF
            p = body + 8
            wfmt = "<%dI" % ncount
            for _ in range(count):
                seq, total, host, gpu, vbl, swap, ovl, flags = struct.unpack_from("<IIIIIIIH", data, p)
                work = struct.unpack_from(wfmt, data, p + FRAME_BYTES) if ncount else ()
                frames.append((seq, total, host, gpu, vbl, swap, ovl, flags, work))
                p += frame_bytes
            next_seq = (first + count) & 0xFFFFFFFF
            if rec["truncated"]:
                break
        elif body + clen > len(data):
            rec["truncated"] = True
            break
        elif ctype == CHUNK_MAP:
            tsc, load, fb, fa, tp, flags = struct.unpack_from("<QQIIII", data, body)
            name = data[body + 32:body + 96].split(b"\0", 1)[0].decode("latin-1")
            rec["maps"].append({"tsc": tsc, "load_ticks": load, "free_before": fb, "free_after": fa,
                                "total_pages": tp, "flags": flags, "name": name, "frame": len(frames)})
        elif ctype == CHUNK_CALIB:
            rec["calibs"].append(struct.unpack_from("<QQ", data, body))
        elif ctype == CHUNK_END:
            tsc, lost = struct.unpack_from("<QI", data, body)
            rec["end"] = {"tsc": tsc, "lost": lost}
        elif ctype == CHUNK_COUNTERS:
            n, = struct.unpack_from("<I", data, body)
            if clen < 4 + n * COUNTER_NAME_BYTES:
                raise FormatError("COUNTERS chunk at %d: length %d for %d names" % (off, clen, n))
            q = body + 4
            rec["counters"] = [data[q + i * COUNTER_NAME_BYTES:q + (i + 1) * COUNTER_NAME_BYTES]
                               .split(b"\0", 1)[0].decode("latin-1") for i in range(n)]
        else:
            rec["unknown_chunks"] += 1
        off = body + clen

    # Names for every counter a record carries: the COUNTERS chunk's, then
    # placeholders if a record holds more than it names; extra names are
    # dropped (a record cut short of them carries nothing to name).
    names = rec["counters"][:ncount]
    names += ["c%d" % i for i in range(len(names), ncount)]
    rec["counters"] = names
    rec["tsc_hz"] = tsc_rate(rec)
    return rec


def tsc_rate(rec):
    """The TSC rate from the calibration pairs (the open pair and the last
    CALIB, the longest baseline); the header's estimate if there are none."""
    if rec["calibs"] and rec["qpc_hz"]:
        tsc, qpc = rec["calibs"][-1]
        if qpc > rec["qpc_open"] and tsc > rec["tsc_open"]:
            return (tsc - rec["tsc_open"]) * rec["qpc_hz"] / (qpc - rec["qpc_open"])
    return float(rec["tsc_hz_header"]) or 733333333.0


def load_file(path):
    with open(path, "rb") as f:
        return parse(f.read())


# --------------------------------------------------------------------------
# Statistics
# --------------------------------------------------------------------------

def percentile_ms(values_sorted, frac):
    """The value at rank ceil(n * frac) of an ascending list (1-based) - the
    frame time the slowest (1 - frac) of frames are at or above."""
    if not values_sorted:
        return 0.0
    rank = max(1, int(math.ceil(len(values_sorted) * frac - 1e-9)))
    return values_sorted[min(rank, len(values_sorted)) - 1]


def frame_ms(rec, fr):
    """(total, cpu, gpu, vblank+swap, other, overlay) in ms for one frame tuple."""
    k = 1000.0 / rec["tsc_hz"]
    seq, total, host, gpu, vbl, swap, ovl, flags = fr[:8]
    cpu = max(0, host - gpu - vbl - swap)
    other = max(0, total - host)
    return total * k, cpu * k, gpu * k, (vbl + swap) * k, other * k, ovl * k


def segments(rec):
    """Split the frames at each MAP chunk. Frames before the first map
    (menus, the first load) form a segment named '(before first map)'."""
    maps = rec["maps"]
    out = []
    starts = [m["frame"] for m in maps]
    if not maps or starts[0] > 0:
        out.append({"name": "(before first map)", "map": None, "start": 0,
                    "end": starts[0] if maps else len(rec["frames"])})
    for i, m in enumerate(maps):
        end = starts[i + 1] if i + 1 < len(maps) else len(rec["frames"])
        out.append({"name": m["name"], "map": m, "start": m["frame"], "end": end})
    return out


def work_names(rec):
    """The counter names, plus the derived VERTS (every v_* summed) when
    the file has vertex counters. Empty for a v1 file."""
    names = list(rec["counters"])
    if any(n.startswith("v_") for n in names):
        names.append(VERTS)
    return names


def frame_work(rec, fr):
    """One frame's counters in work_names() order."""
    w = list(fr[8]) if len(fr) > 8 else []
    if len(w) < len(rec["counters"]):
        w += [0] * (len(rec["counters"]) - len(w))
    if VERTS in work_names(rec):
        w.append(sum(v for n, v in zip(rec["counters"], w) if n.startswith("v_")))
    return w


def play_frames(rec, seg):
    """The segment's in-play frames - ACTIVE, not MENU, past the first
    WARMUP_FRAMES active ones - as (frame tuple, frame_ms(...)) pairs,
    flush frames included (the caller decides)."""
    active_seen = 0
    for fr in rec["frames"][seg["start"]:seg["end"]]:
        flags = fr[7]
        if not (flags & F_ACTIVE) or (flags & F_MENU):
            continue
        active_seen += 1
        if active_seen <= WARMUP_FRAMES:
            continue
        yield fr, frame_ms(rec, fr)


def _avg(values):
    return sum(values) / len(values) if values else None


def segment_stats(rec, seg, include_flush=False):
    """Per-map numbers over the segment's in-play frames: ACTIVE, not MENU,
    after the first WARMUP_FRAMES active frames, and - unless
    include_flush - not a frame that wrote the file (FLUSH): its time
    includes the write, which the game itself never does."""
    frames = rec["frames"][seg["start"]:seg["end"]]
    totals, cpu_s, gpu_s, wait_s, other_s, ovl = [], 0.0, 0.0, 0.0, 0.0, []
    gpu_bound = flushes = 0
    flush_ms = []
    names = work_names(rec)
    work, work_hi, work_lo = [], [], []
    for fr, (t, c, g, w, o, ov) in play_frames(rec, seg):
        flags = fr[7]
        if flags & F_FLUSH:
            flushes += 1
            flush_ms.append(t)
            if not include_flush:
                continue
        totals.append(t)
        cpu_s += c
        gpu_s += g
        wait_s += w
        other_s += o
        if g > GPU_BOUND_MS:
            gpu_bound += 1
        if flags & F_OVERLAY:
            ovl.append(ov)
        if names:
            fw = frame_work(rec, fr)
            work.append(fw)
            (work_hi if g > GPU_BOUND_MS else work_lo).append(fw)
    n = len(totals)
    m = seg["map"]
    st = {"map": seg["name"], "frames": n, "all_frames": len(frames),
          "flush_frames": flushes, "flush_frame_ms": _avg(flush_ms),
          "flush_worst_ms": max(flush_ms) if flush_ms else None, "flush_included": include_flush}
    if m:
        st["load_s"] = None if m["flags"] & MAP_F_NO_LOAD_START else m["load_ticks"] / rec["tsc_hz"]
        st["free_before"] = m["free_before"] * PAGE_BYTES / 2 ** 20 if m["free_before"] else None
        st["free_after"] = m["free_after"] * PAGE_BYTES / 2 ** 20
    if not n:
        return st
    secs = sum(totals) / 1000.0
    srt = sorted(totals)
    p99 = percentile_ms(srt, 0.99)
    # One-second windows of consecutive in-play frames, as item 12's report.
    wins, acc, cnt = [], 0.0, 0
    for t in totals:
        acc += t
        cnt += 1
        if acc >= 1000.0:
            wins.append(cnt * 1000.0 / acc)
            acc, cnt = 0.0, 0
    st.update({
        "seconds": secs,
        "avg_fps": n / secs if secs else 0.0,
        "avg_ms": sum(totals) / n,
        "min_fps": min(wins) if wins else None,
        "max_fps": max(wins) if wins else None,
        "low1_fps": 1000.0 / p99 if p99 else 0.0,
        "p99_ms": p99,
        "median_ms": percentile_ms(srt, 0.5),
        "worst_ms": srt[-1],
        "cpu_ms": cpu_s / n, "gpu_ms": gpu_s / n, "wait_ms": wait_s / n, "other_ms": other_s / n,
        "gpu_bound_pct": 100.0 * gpu_bound / n,
        "over60": sum(1 for t in totals if t > GUIDE_60_MS),
        "over30": sum(1 for t in totals if t > GUIDE_30_MS),
        "overlay_ms": sum(ovl) / len(ovl) if ovl else None,
    })
    if names:
        cols = list(zip(*work))
        st["work_avg"] = {k: sum(v) / n for k, v in zip(names, cols)}
        st["work_peak"] = {k: max(v) for k, v in zip(names, cols)}
        # The GPU-bound frames against the rest: what the frames that made
        # the CPU wait for the GPU submitted, next to the ones that did not.
        st["gpu_hi_frames"] = len(work_hi)
        st["work_gpu_hi"] = {k: sum(v) / len(work_hi) for k, v in zip(names, zip(*work_hi))} if work_hi else None
        st["work_gpu_lo"] = {k: sum(v) / len(work_lo) for k, v in zip(names, zip(*work_lo))} if work_lo else None
    return st


def pearson(xs, ys):
    """Pearson's r, or None when either side does not vary."""
    n = len(xs)
    if n < 3:
        return None
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    if sxx <= 0 or syy <= 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / math.sqrt(sxx * syy)


def work_correlation(rec, include_flush=False):
    """Over every in-play frame of the session: for each counter, its mean,
    peak and Pearson r against the frame's CPU ms and GPU-wait ms. On the
    console this says which work the time follows; in xemu the times are
    the emulator's, so only the counters themselves carry over."""
    names = work_names(rec)
    if not names:
        return []
    cpu, gpu, work = [], [], []
    for seg in segments(rec):
        for fr, (t, c, g, w, o, ov) in play_frames(rec, seg):
            if (fr[7] & F_FLUSH) and not include_flush:
                continue
            cpu.append(c)
            gpu.append(g)
            work.append(frame_work(rec, fr))
    out = []
    for i, k in enumerate(names):
        col = [fw[i] for fw in work]
        out.append({"name": k, "avg": _avg(col), "peak": max(col) if col else None,
                    "r_cpu": pearson(col, cpu), "r_gpu": pearson(col, gpu)})
    return out


def top_frames(rec, key="gpu", n=10, include_flush=False):
    """The n in-play frames with the most GPU wait (key 'gpu') or the
    longest total ('total'): seconds into the session, map, the times and
    the work counters."""
    k = 1000.0 / rec["tsc_hz"]
    start_s, acc = {}, 0.0
    for fr in rec["frames"]:
        start_s[fr[0]] = acc / 1000.0
        acc += fr[1] * k
    names = work_names(rec)
    cand = []
    for seg in segments(rec):
        for fr, (t, c, g, w, o, ov) in play_frames(rec, seg):
            if (fr[7] & F_FLUSH) and not include_flush:
                continue
            cand.append((g if key == "gpu" else t, fr, t, c, g, seg["name"]))
    cand.sort(key=lambda r: -r[0])
    return [{"seq": fr[0], "t": start_s[fr[0]], "map": name, "total": t, "cpu": c, "gpu": g,
             "work": dict(zip(names, frame_work(rec, fr))) if names else {}}
            for _, fr, t, c, g, name in cand[:n]]


def session_stats(rec, rows, include_flush=False):
    k = 1000.0 / rec["tsc_hz"]
    total_ms = sum(fr[1] for fr in rec["frames"]) * k
    play = [r for r in rows if r.get("frames")]
    pf = sum(r["frames"] for r in play)
    ps = sum(r["seconds"] for r in play)
    frees = [r["free_after"] for r in rows if r.get("free_after") is not None]
    loads = [r["load_s"] for r in rows if r.get("load_s") is not None]
    return {
        "minutes": total_ms / 60000.0, "frames": len(rec["frames"]), "loads": len(loads),
        "avg_fps": pf / ps if ps else 0.0,
        "free_min": min(frees) if frees else None, "free_max": max(frees) if frees else None,
        "load_min": min(loads) if loads else None, "load_max": max(loads) if loads else None,
        # A frame the ring overwrote shows up twice: as a sequence gap and in
        # the END chunk's count. The gaps are what the file actually misses.
        "lost": max(rec["lost_frames"], rec["end"]["lost"] if rec["end"] else 0),
        "clean_end": rec["end"] is not None, "truncated": rec["truncated"],
        "tsc_mhz": rec["tsc_hz"] / 1e6, "build": rec["build"],
        "version": rec["version"], "counters": work_names(rec),
        "flush_frames": sum(r.get("flush_frames", 0) for r in rows),
        "flush_included": include_flush,
    }


def timeline(rec):
    """Per-second windows over the whole session (every frame, in order):
    fps, mean cpu/gpu/wait/other ms, worst frame; None fps for a window
    holding a frame outside play (a load, a menu)."""
    names = work_names(rec)
    widx = [names.index(n) if n in names else None for n in ("draws", VERTS, "pb_dwords")]
    out = []
    t = 0.0
    cur = None
    for fr in rec["frames"]:
        total, cpu, gpu, wait, other, _ = frame_ms(rec, fr)
        fw = frame_work(rec, fr) if names else []
        sec = int(t // 1000.0)
        if cur is None or cur["s"] != sec:
            if cur:
                out.append(cur)
            while out and out[-1]["s"] < sec - 1:  # a long frame spans empty seconds
                out.append({"s": out[-1]["s"] + 1, "n": 0, "ms": 0.0, "cpu": 0.0, "gpu": 0.0,
                            "wait": 0.0, "other": 0.0, "worst": 0.0, "play": False, "w": [0, 0, 0]})
            cur = {"s": sec, "n": 0, "ms": 0.0, "cpu": 0.0, "gpu": 0.0, "wait": 0.0,
                   "other": 0.0, "worst": 0.0, "play": True, "w": [0, 0, 0]}
        cur["n"] += 1
        cur["ms"] += total
        cur["cpu"] += cpu
        cur["gpu"] += gpu
        cur["wait"] += wait
        cur["other"] += other
        cur["worst"] = max(cur["worst"], total)
        for j, wi in enumerate(widx):
            if wi is not None:
                cur["w"][j] += fw[wi]
        if not (fr[7] & F_ACTIVE) or (fr[7] & F_MENU):
            cur["play"] = False
        t += total
    if cur:
        out.append(cur)
    rows = []
    for w in out:
        n = w["n"]
        rows.append([
            w["s"],
            round(n * 1000.0 / w["ms"], 2) if n and w["play"] else None,
            round(w["cpu"] / n, 3) if n else 0, round(w["gpu"] / n, 3) if n else 0,
            round(w["wait"] / n, 3) if n else 0, round(w["other"] / n, 3) if n else 0,
            round(w["worst"], 2),
        ] + [round(w["w"][j] / n, 1) if n and widx[j] is not None and w["play"] else None for j in range(3)])
    return rows


def map_markers(rec):
    k = 1000.0 / rec["tsc_hz"]
    cum = [0.0]
    for fr in rec["frames"]:
        cum.append(cum[-1] + fr[1] * k)
    out = []
    for m in rec["maps"]:
        load = None if m["flags"] & MAP_F_NO_LOAD_START else m["load_ticks"] / rec["tsc_hz"]
        out.append({"t": cum[min(m["frame"], len(cum) - 1)] / 1000.0, "name": m["name"], "load": load})
    return out


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

def fmt(v, d=1, dash="-"):
    return dash if v is None else ("%.*f" % (d, v))


def summary_text(rec, rows, sess):
    lines = []
    lines.append("build %s, TSC %.2f MHz, %d frames, %.1f min, %s%s" % (
        sess["build"], sess["tsc_mhz"], sess["frames"], sess["minutes"],
        "clean end" if sess["clean_end"] else "NO END CHUNK (cut short)",
        ", truncated" if sess["truncated"] else ""))
    if sess["lost"]:
        lines.append("frames lost to a full ring: %d" % sess["lost"])
    hdr = "%-18s %6s %6s %6s %6s %6s %7s %6s %6s %5s %6s %6s %6s %6s %6s" % (
        "map", "played", "avg", "min", "max", "1%low", "worst", "cpu", "gpu", "gpu%", ">16.7", ">33.3", "load", "free0", "free1")
    lines.append(hdr)
    for r in rows:
        if not r.get("frames"):
            lines.append("%-18s %6s (no frames in play)" % (r["map"][:18], "-"))
            continue
        lines.append("%-18s %6.1f %6.1f %6s %6s %6.1f %7.1f %6.2f %6.2f %5.1f %5.1f%% %5.1f%% %6s %6s %6s" % (
            r["map"][:18], r["seconds"], r["avg_fps"], fmt(r["min_fps"]), fmt(r["max_fps"]), r["low1_fps"],
            r["worst_ms"], r["cpu_ms"], r["gpu_ms"], r["gpu_bound_pct"],
            100.0 * r["over60"] / r["frames"], 100.0 * r["over30"] / r["frames"],
            fmt(r.get("load_s"), 2), fmt(r.get("free_before")), fmt(r.get("free_after"))))
    lines.append("avg fps in play %.1f; played = s of in-play frames; min/max = slowest/fastest 1 s window; "
                 "1%%low = 1000 / 99th-percentile frame ms; cpu/gpu = ms per frame (gpu = CPU blocked on the GPU); "
                 "gpu%% = frames with >%.0f ms GPU wait; free0/free1 = MiB free at the last flush before the load / at map start"
                 % (sess["avg_fps"], GPU_BOUND_MS))
    fl = sess.get("flush_frames", 0)
    if fl:
        lines.append("%d flush frame(s) (the recorder writing its file) %s the statistics; --include-flush %s"
                     % (fl, "kept in" if sess.get("flush_included") else "left out of",
                        "is on" if sess.get("flush_included") else "keeps them"))
    if sess.get("counters"):
        lines.append("")
        lines.append("work per frame in play, avg/peak (counts: the same in xemu and on the console)")
        cols = [(k, h) for k, h in WORK_TABLE if k in sess["counters"]]
        lines.append("%-18s " % "map" + " ".join("%13s" % h[:13] for _, h in cols))
        for r in rows:
            if not r.get("frames") or "work_avg" not in r:
                continue
            cells = []
            for k, _ in cols:
                a, pk = r["work_avg"][k], r["work_peak"][k]
                if k == "pb_dwords":
                    a, pk = a * 4 / 1024.0, pk * 4 / 1024.0
                cells.append("%13s" % ("%s/%s" % (short(a), short(pk))))
            lines.append("%-18s " % r["map"][:18] + " ".join(cells))
    elif sess.get("version") == 1:
        lines.append("version-1 recording: no work counters")
    return "\n".join(lines)


def short(v):
    """812, 12.3k, 1.23M - a count in a few characters."""
    if v is None:
        return "-"
    if v < 1000:
        return "%.0f" % v if v >= 10 or v == int(v) else "%.1f" % v
    if v < 1e6:
        return "%.1fk" % (v / 1e3)
    return "%.2fM" % (v / 1e6)


HTML_TEMPLATE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__</title>
<style>
:root{
  --ground:#F2F3EF; --panel:#FAFBF8; --ink:#1B211C; --muted:#5F685F; --rule:#D7DBD3;
  --accent:#3B7236; --band:rgba(27,33,28,.035);
  --cpu:#3B8F4A; --gpu:#C98A12; --wait:#6C79A8; --other:#A9AFA6;
  --warn:#B8740F; --crit:#B2352E; --load:#6A5AA8; --g60:#1D86B0;
  --w1:#2F6FB0; --w2:#B0452F; --w3:#5E8F2E;
  --sans:system-ui,-apple-system,"Segoe UI",sans-serif; --mono:ui-monospace,Consolas,monospace;
}
@media (prefers-color-scheme: dark){
  :root:not([data-theme="light"]){
    --ground:#111411; --panel:#171B17; --ink:#E3E7E0; --muted:#98A294; --rule:#2B312A;
    --accent:#86C179; --band:rgba(227,231,224,.04);
    --cpu:#6CC47B; --gpu:#E6AE45; --wait:#98A5DA; --other:#5F665C;
    --warn:#E0A445; --crit:#E46A60; --load:#A596E6; --g60:#5CC3EA; color-scheme:dark;
    --w1:#79AEE6; --w2:#E68A72; --w3:#9CCB6A;
  }
}
:root[data-theme="dark"]{
  --ground:#111411; --panel:#171B17; --ink:#E3E7E0; --muted:#98A294; --rule:#2B312A;
  --accent:#86C179; --band:rgba(227,231,224,.04);
  --cpu:#6CC47B; --gpu:#E6AE45; --wait:#98A5DA; --other:#5F665C;
  --warn:#E0A445; --crit:#E46A60; --load:#A596E6; --g60:#5CC3EA; color-scheme:dark;
  --w1:#79AEE6; --w2:#E68A72; --w3:#9CCB6A;
}
*{box-sizing:border-box}
body{margin:0;background:var(--ground);color:var(--ink);font-family:var(--sans);font-size:15px;line-height:1.55}
.wrap{max-width:1120px;margin:0 auto;padding:32px 16px 64px;display:flex;flex-direction:column;gap:28px}
.eyebrow{font-family:var(--mono);font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted)}
h1{font-weight:600;font-size:clamp(26px,4vw,36px);line-height:1.15;margin:6px 0 8px}
h2{font-weight:600;font-size:19px;margin:0 0 4px}
p{margin:0;max-width:72ch}
.lede{color:var(--muted)}
.banner{border-left:3px solid var(--warn);padding:8px 12px;background:var(--band);font-size:14px}
.stats{display:flex;flex-wrap:wrap;gap:12px 32px;padding-top:6px}
.stat{display:flex;flex-direction:column}
.stat b{font-family:var(--mono);font-weight:500;font-size:24px;font-variant-numeric:tabular-nums}
.stat span{font-size:12px;color:var(--muted);letter-spacing:.04em;text-transform:uppercase}
.panel{background:var(--panel);border:1px solid var(--rule);border-radius:6px;padding:16px}
.chart-head{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:baseline;gap:8px;margin-bottom:8px}
.readout{font-family:var(--mono);font-size:12.5px;color:var(--muted);min-height:1.6em;font-variant-numeric:tabular-nums}
canvas{display:block;width:100%;height:260px;touch-action:pan-y}
.legend{display:flex;flex-wrap:wrap;gap:6px 18px;font-size:12.5px;color:var(--muted);margin-top:8px}
.key{display:inline-flex;align-items:center;gap:6px}
.sw{width:14px;height:10px;border-radius:2px;display:inline-block}
.ln{width:18px;height:2px;display:inline-block}
.table-box{overflow-x:auto}
table{border-collapse:collapse;width:100%;font-size:13px;min-width:980px}
th,td{padding:7px 8px;text-align:right;white-space:nowrap}
th{font-weight:500;font-size:11px;letter-spacing:.05em;text-transform:uppercase;color:var(--muted);border-bottom:1px solid var(--rule)}
td{font-family:var(--mono);font-variant-numeric:tabular-nums;border-bottom:1px solid var(--rule)}
th:first-child,td:first-child{text-align:left}
tbody tr{cursor:pointer}
tbody tr:hover td,tbody tr.on td{background:var(--band)}
.ok{color:var(--accent)} .warn{color:var(--warn)} .crit{color:var(--crit)}
.dist{display:flex;width:90px;height:9px;border-radius:2px;overflow:hidden;background:var(--rule)}
.dist i{display:block;height:100%}
.notes{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:20px}
.note h3{font-weight:600;font-size:15px;margin:0 0 4px}
.note p{color:var(--muted);font-size:14px}
code{font-family:var(--mono);font-size:12.5px}
.sub{font-weight:600;font-size:15px;margin:18px 0 6px}
table.small{min-width:0;width:auto}
[hidden]{display:none!important}
footer{font-size:12.5px;color:var(--muted);border-top:1px solid var(--rule);padding-top:12px}
</style>
</head>
<body>
<div class="wrap">
<header>
  <div class="eyebrow">Resonance3D &middot; frame-time recorder &middot; __SOURCE__</div>
  <h1>__TITLE__</h1>
  <p class="lede">__LEDE__</p>
  <div class="stats" id="stats"></div>
</header>
__BANNER__
<section class="panel">
  <div class="chart-head"><h2>Frame rate over the session</h2><div class="readout" id="ro1">Hover or tap the chart for a reading.</div></div>
  <canvas id="c1" height="260"></canvas>
  <div class="legend">
    <span class="key"><span class="ln" style="background:var(--ink)"></span>fps, each second of play</span>
    <span class="key"><span class="ln" style="background:var(--g60)"></span>60 fps (16.7 ms)</span>
    <span class="key"><span class="ln" style="background:var(--crit)"></span>30 fps (33.3 ms)</span>
    <span class="key"><span class="sw" style="background:var(--load);opacity:.35"></span>level load</span>
  </div>
</section>
<section class="panel">
  <div class="chart-head"><h2>Where each frame's time went</h2><div class="readout" id="ro2"></div></div>
  <canvas id="c2" height="260"></canvas>
  <div class="legend">
    <span class="key"><span class="sw" style="background:var(--cpu)"></span>CPU (engine work)</span>
    <span class="key"><span class="sw" style="background:var(--gpu)"></span>GPU wait (CPU blocked on the GPU)</span>
    <span class="key"><span class="sw" style="background:var(--wait)"></span>vblank / swap wait</span>
    <span class="key"><span class="sw" style="background:var(--other)"></span>between frames (frame limiter)</span>
    <span class="key"><span class="ln" style="background:var(--g60)"></span>16.7 ms</span>
    <span class="key"><span class="ln" style="background:var(--crit)"></span>33.3 ms</span>
  </div>
</section>
<section class="panel" id="workchart" hidden>
  <div class="chart-head"><h2>Work per frame over the session</h2><div class="readout" id="ro3"></div></div>
  <canvas id="c3" height="260"></canvas>
  <div class="legend">
    <span class="key"><span class="ln" style="background:var(--w1)"></span>draws</span>
    <span class="key"><span class="ln" style="background:var(--w2)"></span>vertices</span>
    <span class="key"><span class="ln" style="background:var(--w3)"></span>pushbuffer dwords</span>
    <span>each line is a per-second mean over frames in play, scaled to its own session peak (the readout gives the values)</span>
  </div>
</section>
<section class="panel">
  <h2>Per map</h2>
  <p class="lede" style="font-size:13.5px;margin-bottom:10px">In-play frames only (not loading, no menu or pause), leaving out a map's first __WARMUP__ frames. Min and max fps are the slowest and fastest one-second windows. 1% low is the frame rate the slowest 1% of frames fall below (1000 / the 99th-percentile frame time). CPU and GPU are milliseconds per frame. Free memory is sampled at the last flush before the load and at the first frame of the map. __FLUSHNOTE__ Click a row to mark it on the charts.</p>
  <div class="table-box"><table>
    <thead><tr><th>Map</th><th>Played</th><th>Avg fps</th><th>Min</th><th>Max</th><th>1% low</th><th>Worst ms</th><th>CPU ms</th><th>GPU ms</th><th>GPU-bound</th><th>&gt;16.7 ms</th><th>&gt;33.3 ms</th><th>Split</th><th>Flushes</th><th>Load s</th><th>MiB free before / at start</th></tr></thead>
    <tbody id="rows"></tbody>
  </table></div>
</section>
<section class="panel" id="work" hidden>
  <h2>Work per frame</h2>
  <p class="lede" style="font-size:13.5px;margin-bottom:10px">What the renderer submitted in each in-play frame, counted where ref/nv2a submits it: mean / peak per map. These are counts, not times, so <b>they are the same in xemu as on the console</b> &mdash; an xemu run measures them exactly. Draws are draw submissions (a world run, a 2D/TriAPI batch, a decal); prims the BEGIN/END blocks they split into; PB KiB the pushbuffer written in the frame; verts every vertex or index submitted (world, studio, TriAPI, 2D, decals); LM texels the lightmap texels rebuilt on the CPU.</p>
  <div class="table-box"><table id="worktab"></table></div>
  <h3 class="sub">What the frame time follows</h3>
  <p class="lede" style="font-size:13.5px;margin-bottom:10px">Over every in-play frame of the session: each counter's mean and peak, and Pearson's r against the frame's CPU time and its GPU wait. A strong r says the time rises and falls with that work. __CORRNOTE__</p>
  <div class="table-box"><table class="small" id="corrtab"></table></div>
  <h3 class="sub">GPU-bound frames against the rest</h3>
  <p class="lede" style="font-size:13.5px;margin-bottom:10px">Per map, the mean work of frames with more than __GPUMS__ ms of GPU wait next to the frames without: work that is much higher in the first column is what made the CPU wait for the GPU.</p>
  <div class="table-box"><table class="small" id="hitab"></table></div>
  <h3 class="sub">The 10 frames with the most GPU wait</h3>
  <div class="table-box"><table class="small" id="toptab"></table></div>
</section>
<section class="notes">
  <div class="note"><h3>CPU and GPU</h3><p>Each frame is timed with the Pentium III's time-stamp counter, calibrated against the kernel's performance counter (<code>__TSC__</code> MHz in this file). <b>CPU</b> is the engine's own work: <code>Host_Frame</code> minus the renderer's waits. <b>GPU wait</b> is the time the CPU spun on <code>pb_busy()</code> because the GPU still had work queued. The NV2A runs in parallel with the CPU and pbkit exposes no GPU-side timestamp, so this is the GPU's excess over the CPU, not its busy time: near zero means the GPU kept up and the frame was CPU-bound; a large value means the GPU held the frame. <b>GPU-bound</b> counts frames with more than __GPUMS__ ms of GPU wait.</p></div>
  <div class="note"><h3>Waits and the rest</h3><p><b>vblank / swap</b> is time in <code>pb_wait_for_vbl()</code> and in <code>pb_finished()</code> while all three back buffers were queued for display. <b>Between frames</b> is the time outside <code>Host_Frame</code>: the engine's frame limiter (<code>fps_max</code>) and the platform loop. A frame's total is start to next start, so the timeline is wall-clock time.</p></div>
  <div class="note"><h3>Work counters</h3><p>__WORKNOTE__</p></div>
  <div class="note"><h3>Recording</h3><p>__RECNOTE__</p></div>
</section>
<footer>Generated by <code>tools/frametime-report.py</code> from <code>__FILES__</code>. File format: <code>engine/platform/xbox/xbox_frametime_util.h</code>.</footer>
</div>
<script>
const SESSION=__SESSION__;const ROWS=__ROWS__;const TL=__TIMELINE__;const MARKS=__MARKS__;
const WT=__WORKTABLE__;const CORR=__CORR__;const TOPF=__TOP__;
(function(){
  const css=n=>getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  const f=(x,d=1)=>x==null?'\u2013':x.toFixed(d);
  const mmss=s=>Math.floor(s/60)+':'+String(Math.floor(s%60)).padStart(2,'0');
  const st=[[f(SESSION.minutes)+' min','session'],[SESSION.loads,'map loads'],[f(SESSION.avg_fps),'avg fps in play'],
    [SESSION.frames.toLocaleString('en-US'),'frames recorded'],
    [SESSION.free_min==null?'\u2013':f(SESSION.free_min)+'\u2013'+f(SESSION.free_max),'MiB free at map start'],
    [SESSION.load_min==null?'\u2013':f(SESSION.load_min)+'\u2013'+f(SESSION.load_max)+' s','load times']];
  document.getElementById('stats').innerHTML=st.map(([v,l])=>`<div class="stat"><b>${v}</b><span>${l}</span></div>`).join('');
  const cls=(v,g,b)=>v>=g?'ok':v>=b?'warn':'crit';
  let sel=-1;
  const tb=document.getElementById('rows');
  ROWS.forEach((r,k)=>{
    const tr=document.createElement('tr');
    if(!r.frames){tr.innerHTML=`<td>${r.map}</td><td colspan="15" style="text-align:left;color:var(--muted)">no frames in play</td>`;tb.appendChild(tr);return;}
    const p60=100*(r.over60-r.over30)/r.frames, p30=100*r.over30/r.frames, pok=100-p60-p30;
    tr.innerHTML=`<td>${r.map}</td><td>${mmss(r.seconds)}</td><td class="${cls(r.avg_fps,59,45)}">${f(r.avg_fps)}</td><td>${f(r.min_fps)}</td><td>${f(r.max_fps)}</td>
      <td class="${cls(r.low1_fps,55,29)}">${f(r.low1_fps)}</td><td>${f(r.worst_ms)}</td><td>${f(r.cpu_ms,2)}</td><td>${f(r.gpu_ms,2)}</td><td>${f(r.gpu_bound_pct)}%</td>
      <td>${f(100*r.over60/r.frames)}%</td><td>${f(100*r.over30/r.frames)}%</td>
      <td><span class="dist" title="${f(pok)}% under 16.7 ms, ${f(p60)}% 16.7-33.3 ms, ${f(p30)}% over 33.3 ms"><i style="width:${pok}%;background:var(--accent)"></i><i style="width:${p60}%;background:var(--warn)"></i><i style="width:${p30}%;background:var(--crit)"></i></span></td>
      <td title="frames that wrote the file: mean / worst ms">${r.flush_frames?r.flush_frames+' &middot; '+f(r.flush_frame_ms)+' / '+f(r.flush_worst_ms)+' ms':'0'}</td>
      <td>${f(r.load_s,2)}</td><td>${f(r.free_before)} / ${f(r.free_after)}</td>`;
    tr.onclick=()=>{sel=sel===k?-1:k;[...tb.children].forEach((e,j)=>e.classList.toggle('on',j===sel));draw();};
    tb.appendChild(tr);
  });
  // Segment k of the table spans [MARKS[k'].t, next) - the row order equals the marker order,
  // offset by one when the file starts before its first map.
  const off=ROWS.length-MARKS.length;
  const T=TL.length?TL[TL.length-1][0]+1:1;
  function span(k){const m=k-off;if(m<0)return[0,MARKS.length?MARKS[0].t:T];return[MARKS[m].t,m+1<MARKS.length?MARKS[m+1].t:T];}
  const sh=v=>v==null?'\u2013':v<1000?(v>=10||v===Math.round(v)?v.toFixed(0):v.toFixed(1)):v<1e6?(v/1e3).toFixed(1)+'k':(v/1e6).toFixed(2)+'M';
  const wv=(k,v)=>k==='pb_dwords'&&v!=null?v*4/1024:v;
  if(SESSION.counters.length){
    document.getElementById('work').hidden=false;document.getElementById('workchart').hidden=false;
    const cols=WT.filter(([k])=>SESSION.counters.includes(k));
    let h='<thead><tr><th>Map</th>'+cols.map(([,t])=>`<th>${t}</th>`).join('')+'</tr></thead><tbody>';
    ROWS.forEach(r=>{if(!r.work_avg)return;h+=`<tr><td>${r.map}</td>`+cols.map(([k])=>`<td>${sh(wv(k,r.work_avg[k]))} / ${sh(wv(k,r.work_peak[k]))}</td>`).join('')+'</tr>';});
    document.getElementById('worktab').innerHTML=h+'</tbody>';
    const rc=v=>v==null?'\u2013':`<span class="${Math.abs(v)>=.5?'crit':Math.abs(v)>=.3?'warn':''}">${v.toFixed(2)}</span>`;
    h='<thead><tr><th>Counter</th><th>Mean</th><th>Peak</th><th>r vs CPU</th><th>r vs GPU wait</th></tr></thead><tbody>';
    CORR.forEach(c=>{h+=`<tr><td>${c.name}</td><td>${sh(c.avg)}</td><td>${sh(c.peak)}</td><td>${rc(c.r_cpu)}</td><td>${rc(c.r_gpu)}</td></tr>`;});
    document.getElementById('corrtab').innerHTML=h+'</tbody>';
    const hk=['draws','prims','pb_dwords','tex_binds','verts','v_studio','lm_texels'].filter(k=>SESSION.counters.includes(k));
    h='<thead><tr><th>Map</th><th>GPU-bound frames</th>'+hk.map(k=>`<th>${k==='pb_dwords'?'PB KiB':k}<br>bound / rest</th>`).join('')+'</tr></thead><tbody>';
    ROWS.forEach(r=>{if(!r.work_avg)return;h+=`<tr><td>${r.map}</td><td>${r.gpu_hi_frames} of ${r.frames}</td>`+hk.map(k=>`<td>${r.work_gpu_hi?sh(wv(k,r.work_gpu_hi[k])):'\u2013'} / ${r.work_gpu_lo?sh(wv(k,r.work_gpu_lo[k])):'\u2013'}</td>`).join('')+'</tr>';});
    document.getElementById('hitab').innerHTML=h+'</tbody>';
    const tk=['draws','prims','pb_dwords','tex_binds','verts','studio_models','lm_texels'].filter(k=>SESSION.counters.includes(k));
    h='<thead><tr><th>At</th><th>Map</th><th>Frame ms</th><th>CPU</th><th>GPU wait</th>'+tk.map(k=>`<th>${k==='pb_dwords'?'PB KiB':k}</th>`).join('')+'</tr></thead><tbody>';
    TOPF.forEach(t=>{h+=`<tr><td>${mmss(t.t)}</td><td>${t.map}</td><td>${f(t.total)}</td><td>${f(t.cpu,2)}</td><td>${f(t.gpu,2)}</td>`+tk.map(k=>`<td>${sh(wv(k,t.work[k]))}</td>`).join('')+'</tr>';});
    document.getElementById('toptab').innerHTML=h+'</tbody>';
  }
  function setup(c){const dpr=window.devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight||260;c.width=w*dpr;c.height=h*dpr;const g=c.getContext('2d');g.setTransform(dpr,0,0,dpr,0,0);return[g,w,h];}
  const L=44,R=10,TOP=10,B=24;
  function frame(g,w,h,ymax,ylab,div=4){
    g.clearRect(0,0,w,h);g.font='11px '+css('--mono');g.fillStyle=css('--muted');g.strokeStyle=css('--rule');g.lineWidth=1;
    for(let i=0;i<=div;i++){const v=ymax*i/div,y=TOP+(h-TOP-B)*(1-i/div);g.beginPath();g.moveTo(L,y);g.lineTo(w-R,y);g.stroke();g.fillText(ylab(v),4,y+4);}
    const step=T>1800?300:T>600?120:T>180?60:15;
    for(let s=0;s<=T;s+=step){const x=L+(w-L-R)*s/T;g.fillText(mmss(s),x-12,h-6);}
    MARKS.forEach(m=>{const x=L+(w-L-R)*m.t/T;if(m.load){const x0=L+(w-L-R)*Math.max(0,m.t-m.load)/T;g.fillStyle=css('--load');g.globalAlpha=.18;g.fillRect(x0,TOP,Math.max(1,x-x0),h-TOP-B);g.globalAlpha=1;}
      g.strokeStyle=css('--load');g.beginPath();g.moveTo(x,TOP);g.lineTo(x,h-B);g.stroke();g.fillStyle=css('--load');g.fillText(m.name,x+3,TOP+10);});
    if(sel>=0){const[a,b]=span(sel);g.fillStyle=css('--ink');g.globalAlpha=.07;g.fillRect(L+(w-L-R)*a/T,TOP,(w-L-R)*(b-a)/T,h-TOP-B);g.globalAlpha=1;}
  }
  function hline(g,w,h,v,ymax,col){const y=TOP+(h-TOP-B)*(1-v/ymax);g.strokeStyle=col;g.setLineDash([5,4]);g.beginPath();g.moveTo(L,y);g.lineTo(w-R,y);g.stroke();g.setLineDash([]);}
  function draw(){
    let [g,w,h]=setup(document.getElementById('c1'));
    const fmax=Math.max(80,Math.ceil(Math.max(...TL.map(r=>r[1]||0))/20)*20);
    frame(g,w,h,fmax,v=>v.toFixed(0));
    hline(g,w,h,60,fmax,css('--g60'));hline(g,w,h,30,fmax,css('--crit'));
    g.strokeStyle=css('--ink');g.lineWidth=1.5;g.beginPath();let pen=false;
    TL.forEach(r=>{if(r[1]==null){pen=false;return;}const x=L+(w-L-R)*(r[0]+.5)/T,y=TOP+(h-TOP-B)*(1-r[1]/fmax);if(pen)g.lineTo(x,y);else g.moveTo(x,y);pen=true;});g.stroke();g.lineWidth=1;
    [g,w,h]=setup(document.getElementById('c2'));
    const mmax=50;frame(g,w,h,mmax,v=>v.toFixed(0)+'ms',5);
    const cols=[css('--cpu'),css('--gpu'),css('--wait'),css('--other')];
    const bw=Math.max(1,(w-L-R)/T);
    TL.forEach(r=>{let y=h-B;const x=L+(w-L-R)*r[0]/T;for(let j=0;j<4;j++){const v=r[2+j],hh=Math.min(y-TOP,(h-TOP-B)*v/mmax);g.fillStyle=cols[j];g.fillRect(x,y-hh,bw+.5,hh);y-=hh;}});
    hline(g,w,h,1000/60,mmax,css('--g60'));hline(g,w,h,1000/30,mmax,css('--crit'));
    if(!SESSION.counters.length)return;
    [g,w,h]=setup(document.getElementById('c3'));
    frame(g,w,h,100,v=>v.toFixed(0)+'%');
    const wc=[css('--w1'),css('--w2'),css('--w3')];
    for(let j=0;j<3;j++){
      const pk=Math.max(0,...TL.map(r=>r[7+j]||0));if(!pk)continue;
      g.strokeStyle=wc[j];g.lineWidth=1.5;g.beginPath();let pen=false;
      TL.forEach(r=>{const v=r[7+j];if(v==null){pen=false;return;}const x=L+(w-L-R)*(r[0]+.5)/T,y=TOP+(h-TOP-B)*(1-v/pk);if(pen)g.lineTo(x,y);else g.moveTo(x,y);pen=true;});
      g.stroke();g.lineWidth=1;
    }
  }
  function hover(id,ro,fn){const c=document.getElementById(id);const h=e=>{const r=c.getBoundingClientRect();const x=(e.touches?e.touches[0].clientX:e.clientX)-r.left;const s=Math.floor((x-L)/(r.width-L-R)*T);const row=TL.find(q=>q[0]===s);let m='';MARKS.forEach(k=>{if(k.t<=s)m=k.name;});document.getElementById(ro).textContent=row?fn(row,m):'';};c.addEventListener('mousemove',h);c.addEventListener('touchstart',h,{passive:true});}
  hover('c1','ro1',(r,m)=>`${mmss(r[0])}  ${m}  ${r[1]==null?'load/menu':r[1].toFixed(1)+' fps'}  worst ${r[6].toFixed(1)} ms`);
  hover('c2','ro2',(r,m)=>`${mmss(r[0])}  ${m}  cpu ${r[2].toFixed(1)}  gpu ${r[3].toFixed(1)}  wait ${r[4].toFixed(1)}  other ${r[5].toFixed(1)} ms`);
  hover('c3','ro3',(r,m)=>r[7]==null?`${mmss(r[0])}  ${m}  load/menu`:`${mmss(r[0])}  ${m}  draws ${sh(r[7])}  verts ${sh(r[8])}  pushbuffer ${sh(r[9])} dwords per frame`);
  draw();window.addEventListener('resize',draw);
  if(window.matchMedia)matchMedia('(prefers-color-scheme: dark)').addEventListener('change',draw);
})();
</script>
</body>
</html>
"""


def render_html(rec, rows, sess, files, title=None, source="xbox", lede=None, include_flush=False):
    if source == "xemu":
        banner = ('<div class="banner"><b>Recorded in xemu.</b> The times are the emulator\'s on a PC, not a '
                  'Pentium III and an NV2A: xemu caps presentation at the 60 Hz refresh, its TSC is not the '
                  'console\'s clock, and its GPU wait says nothing about the real GPU. The <b>work counters</b> '
                  '(draws, vertices, pushbuffer, texture binds, lightmap texels...) are counts, not times: they '
                  'are the same as on the console, so they can drive an optimisation decision from an xemu run; '
                  'the times and their correlation with the counters need the console.</div>')
        src = "xemu (emulator timings)"
    else:
        banner = ""
        src = "original Xbox hardware"
    title = title or "Frame times: %s" % ", ".join(os.path.basename(f) for f in files)
    lede = lede or ("%d frames over %.1f minutes, %d map loads. Every frame's time, split into CPU work and the "
                    "renderer's waits, recorded to the hard drive by <code>host_frametime_analysis</code>."
                    % (sess["frames"], sess["minutes"], sess["loads"]))
    recnote = "Build <code>%s</code>. %s %s Ring of %d frames, flushed every 5 s." % (
        html.escape(sess["build"]),
        "The file ends with a clean END chunk." if sess["clean_end"] else
        "<b>The file has no END chunk</b>: recording was cut by a power-off, a crash or a reboot; everything up to the last flush is here.",
        ("%d frames were lost to a full ring." % sess["lost"]) if sess["lost"] else "No frames were lost.",
        rec["ring_frames"])
    fl = sess.get("flush_frames", 0)
    flushnote = ("Frames that wrote the recording to the disk (every 5 s and at each map start; their time includes "
                 "the write) are %s: %d in this file, the Flushes column gives their mean / worst ms."
                 % ("<b>kept</b> in the statistics (<code>--include-flush</code>)" if include_flush else
                    "<b>left out</b> of the statistics (<code>--include-flush</code> keeps them)", fl))
    if sess.get("counters"):
        worknote = ("Version-2 file: %d counters per frame (%s), added up by ref/nv2a where it submits the work "
                    "and stored with each frame. Counts carry over from xemu to the console; times do not."
                    % (len(rec["counters"]), html.escape(", ".join(rec["counters"]))))
    else:
        worknote = "Version-1 file: recorded before the work counters existed, so only the times are here."
    corrnote = ("In xemu the times are the emulator's, so these r values describe xemu, not the console."
                if source == "xemu" else "")
    out = HTML_TEMPLATE
    for k, v in {
        "__TITLE__": html.escape(title), "__SOURCE__": src, "__LEDE__": lede, "__BANNER__": banner,
        "__WARMUP__": str(WARMUP_FRAMES), "__TSC__": "%.2f" % sess["tsc_mhz"], "__GPUMS__": "%g" % GPU_BOUND_MS,
        "__RECNOTE__": recnote, "__FILES__": html.escape(", ".join(os.path.basename(f) for f in files)),
        "__SESSION__": json.dumps(sess), "__ROWS__": json.dumps(rows),
        "__TIMELINE__": json.dumps(timeline(rec), separators=(",", ":")),
        "__MARKS__": json.dumps(map_markers(rec)),
        "__FLUSHNOTE__": flushnote, "__WORKNOTE__": worknote, "__CORRNOTE__": corrnote,
        "__WORKTABLE__": json.dumps(WORK_TABLE),
        "__CORR__": json.dumps(work_correlation(rec, include_flush)),
        "__TOP__": json.dumps(top_frames(rec, "gpu", 10, include_flush)),
    }.items():
        out = out.replace(k, v)
    return out


def analyse(path, include_flush=False):
    rec = load_file(path)
    rows = [segment_stats(rec, s, include_flush) for s in segments(rec)]
    return rec, rows, session_stats(rec, rows, include_flush)


# --------------------------------------------------------------------------
# Fetching
# --------------------------------------------------------------------------

def ftp_fetch(host, user, password, remote_dir, out_dir, timeout=8.0):
    """Download every ft_*.bin from the console. Read-only: LIST/NLST/CWD/RETR."""
    os.makedirs(out_dir, exist_ok=True)
    try:
        ftp = ftplib.FTP(host, timeout=timeout)
        ftp.login(user, password)
    except (OSError, ftplib.Error) as e:
        raise SystemExit("frametime-report: cannot reach the console's FTP at %s: %s" % (host, e))
    dirs = [remote_dir] if remote_dir else []
    if not dirs:
        try:
            titles = [t.rsplit("/", 1)[-1] for t in ftp.nlst(REMOTE_ROOT)]
        except ftplib.Error as e:
            raise SystemExit("frametime-report: cannot list %s: %s" % (REMOTE_ROOT, e))
        dirs = ["%s/%s/%s" % (REMOTE_ROOT, t, REL_DIR) for t in titles]
    got = []
    for d in dirs:
        try:
            names = [n.rsplit("/", 1)[-1] for n in ftp.nlst(d)]
        except ftplib.Error:
            continue
        for n in sorted(names):
            if not (n.startswith("ft_") and n.endswith(".bin")):
                continue
            dst = os.path.join(out_dir, n)
            with open(dst, "wb") as f:
                ftp.retrbinary("RETR %s/%s" % (d, n), f.write)
            got.append(dst)
            print("fetched %s/%s -> %s (%d bytes)" % (d, n, dst, os.path.getsize(dst)))
    ftp.quit()
    if not got:
        raise SystemExit("frametime-report: no ft_*.bin under %s" % ", ".join(dirs))
    return got


def xemu_fetch(qcow2, out_dir):
    import importlib.util
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("xemu_hdd", os.path.join(here, "xemu_hdd.py"))
    hdd = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hdd)
    os.makedirs(out_dir, exist_ok=True)
    fx = hdd.open_e(qcow2)
    got = []
    for title, is_dir, _ in fx.listdir("UDATA"):
        if not is_dir:
            continue
        d = "UDATA/%s/%s" % (title, REL_DIR)
        try:
            entries = fx.listdir(d)
        except hdd.HddError:
            continue
        for n, is_dir, size in entries:
            if is_dir or not (n.startswith("ft_") and n.endswith(".bin")):
                continue
            dst = os.path.join(out_dir, n)
            with open(dst, "wb") as f:
                f.write(fx.readfile(d + "/" + n))
            got.append(dst)
            print("copied E:/%s/%s -> %s (%d bytes)" % (d, n, dst, size))
    if not got:
        raise SystemExit("frametime-report: no ft_*.bin under E:/UDATA/*/%s in %s" % (REL_DIR, qcow2))
    return got


def main(argv=None):
    ap = argparse.ArgumentParser(description="Frame-time recordings: fetch, summary, HTML report.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("fetch", help="download ft_*.bin from the console over FTP (read-only)")
    p.add_argument("--host", required=True,
                   help="the console's IP address (it changes with DHCP; the dashboard shows it)")
    p.add_argument("--user", default="xbox")
    p.add_argument("--password", default="xbox")
    p.add_argument("--remote-dir", help="default: every %s/<title id>/%s" % (REMOTE_ROOT, REL_DIR))
    p.add_argument("--out", default="frametime-recordings")
    p = sub.add_parser("fetch-xemu", help="copy ft_*.bin off xemu's hard disk image (stop xemu first)")
    p.add_argument("--qcow2", default=r"G:\Games\Xemu\xbox_hdd.qcow2")
    p.add_argument("--out", default="frametime-recordings")
    p = sub.add_parser("summary", help="print the per-map table")
    p.add_argument("file")
    p.add_argument("--include-flush", action="store_true",
                   help="keep the frames that wrote the file in the statistics (left out by default)")
    p = sub.add_parser("report", help="write the HTML report")
    p.add_argument("file")
    p.add_argument("--include-flush", action="store_true",
                   help="keep the frames that wrote the file in the statistics (left out by default)")
    p.add_argument("-o", "--output")
    p.add_argument("--title")
    p.add_argument("--lede", help="the paragraph under the title (HTML allowed)")
    p.add_argument("--source", choices=("xbox", "xemu"), default="xbox",
                   help="where it was recorded; xemu adds the emulator caveat")
    a = ap.parse_args(argv)

    if a.cmd == "fetch":
        ftp_fetch(a.host, a.user, a.password, a.remote_dir, a.out)
    elif a.cmd == "fetch-xemu":
        xemu_fetch(a.qcow2, a.out)
    else:
        try:
            rec, rows, sess = analyse(a.file, a.include_flush)
        except (FormatError, OSError) as e:
            print("frametime-report: %s: %s" % (a.file, e), file=sys.stderr)
            return 1
        if a.cmd == "summary":
            print(summary_text(rec, rows, sess))
        else:
            out = a.output or os.path.splitext(a.file)[0] + ".html"
            with open(out, "w", encoding="utf-8") as f:
                f.write(render_html(rec, rows, sess, [a.file], a.title, a.source, a.lede, a.include_flush))
            print(summary_text(rec, rows, sess))
            print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
