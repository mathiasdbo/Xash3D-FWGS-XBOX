#!/usr/bin/env python3
"""Does the candidate XBE load the saved games players have, in a real (emulated) boot? (docs/r3d/fork-plan.md, XS.4, live level)

    R3D_SAVE_FIXTURES=<folder> python tools/save_live_gate.py run --xbe <default.xbe> --stage <disc tree> --out <folder>
    python tools/save_live_gate.py verify <default.xbe> <evidence file>

`run` puts every fixture `.sav` into a COPY of the disc tree (`--stage`, a folder holding default.xbe and valve/, never
the repository's own stage), puts the candidate XBE in as default.xbe, and writes a cfg that does, for each fixture:
`load <slot>`, wait, stop the frame-time recorder. It packs an ISO, boots xemu on it once, closes xemu gracefully (never
a kill: a killed xemu can leave the disk image half written), copies the recorder's sessions off xemu's disk, and for each
fixture asks one thing: did the map the save was written on play? A save the engine refused (Host_Error) returns to the
menu and records no frames on that map, so it fails here. Nothing is written to a console.

It writes `<out>/live-gate.txt`: the SHA-256 of the XBE it ran, the SHA-256 of every fixture and PASS/FAIL for each. `verify`
(used by tools/packager/make_payload.py) accepts only evidence for exactly the XBE being packaged, every fixture in
tools/save-fixtures-expected.txt that is a `.sav`, and no FAIL: the evidence is bound to the bytes, not to a checkout.

xemu timing is not validation (AGENTS.md): this says the save loads and plays, not that it plays well on a console.
"""

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import save_fixture_gate as host  # noqa: E402

XEMU = os.environ.get("XEMU", "G:/Games/Xemu/xemu.exe")
EXTRACT = os.environ.get("EXTRACT_XISO") or os.path.join(REPO, "deps", "nxdk", "tools", "extract-xiso", "build", "extract-xiso.exe")
WAIT_FRAMES = 480      # 8 s at 60 fps after each `load`
MIN_PLAYED = 2.0       # seconds of in-play frames on the save's map
CFG = os.path.join("valve", "userconfig.d", "90-xs4-live.cfg")
EVIDENCE = "live-gate.txt"
COMMITTED = os.path.join(HERE, "save-live-evidence")  # <xbe sha256>.txt, made by `run`, holds no save content


class LiveError(Exception):
    pass


def header_map(data):
    """the map a save was written on: GameHeader's mapName, read as the engine does (engine/server/sv_save.c)"""
    try:
        tag, ver, size, tc, ts = struct.unpack("<5i", data[:20])
        body = data[20:20 + size + ts]
        pos, toks = 0, []
        for _ in range(tc):
            e = body.index(b"\0", pos)
            toks.append(body[pos:e].decode("latin1"))
            pos = e + 1
        n, ti = struct.unpack("<hh", body[pos:pos + 4])
        pos += 4
        cnt = body[pos]
        pos += n
        for _ in range(cnt):
            n, ti = struct.unpack("<hh", body[pos:pos + 4])
            pos += 4
            if toks[ti] == "mapName":
                return body[pos:pos + n].split(b"\0")[0].decode("latin1")
            pos += n
    except (struct.error, ValueError, IndexError):
        pass
    return None


def sha(data):
    return hashlib.sha256(data).hexdigest()


def xemu_running():
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        "(Get-Process xemu -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ' '"],
                       capture_output=True, text=True)
    return r.stdout.strip()


def winpath(p):
    r = subprocess.run(["cygpath", "-w", p], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else p


def slots(folder):
    """(slot, sha, map, data) for every .sav fixture the expected file lists and the folder holds"""
    with open(host.EXPECTED, encoding="utf-8") as f:
        expected = host.parse_expected(f.read())
    found = host.fixtures_in(folder)
    out = []
    for h, (name, _) in sorted(expected.items(), key=lambda kv: kv[1][0]):
        if not name.lower().endswith(".sav"):
            continue
        if h not in found:
            raise LiveError("fixture %s (%s) is not in %s" % (h[:12], name, folder))
        m = header_map(found[h][1])
        if not m:
            raise LiveError("%s: cannot read the map from its header" % name)
        out.append((name, h, m, found[h][1]))
    if not out:
        raise LiveError("no .sav fixture to load")
    return out


def played(summary_text, mapname):
    """seconds of in-play frames on the map, from the summary's per-map table (the lines under its `map  played ...`
    header, up to the legend): the best of the map's rows, because a map can have a row with no frames in play (the
    load's own frames) and another for the play. Later tables name maps too, with other numbers, and are not read."""
    best, intable = 0.0, False
    for line in summary_text.splitlines():
        parts = line.split()
        if parts[:2] == ["map", "played"]:
            intable = True
            continue
        if intable and (not parts or parts[0] == "avg"):
            break
        if intable and len(parts) > 2 and parts[0] == mapname:
            try:
                best = max(best, float(parts[1]))
            except ValueError:
                pass
    return best


def run(a):
    folder = os.environ.get(host.ENV)
    if not folder or not os.path.isdir(folder):
        raise LiveError("%s is not set to a folder" % host.ENV)
    stage = os.path.abspath(a.stage)
    if not os.path.isfile(os.path.join(stage, "default.xbe")) or not os.path.isdir(os.path.join(stage, "valve")):
        raise LiveError("%s is not a disc tree (default.xbe and valve/)" % stage)
    if os.path.realpath(stage).startswith(os.path.realpath(REPO) + os.sep):
        raise LiveError("the stage must be a copy outside the repository")
    if not os.path.isfile(EXTRACT):
        raise LiveError("no extract-xiso at %s" % EXTRACT)
    if xemu_running():
        raise LiveError("an xemu is already running (%s): not mine, leaving it alone" % xemu_running())
    with open(a.xbe, "rb") as f:
        xbe = f.read()
    host.require(folder)  # the pointers read through the engine's reader, and the reader is committed, before any boot
    items = slots(folder)
    if a.only:
        want = {x.lower() for x in a.only.split(",")}
        items = [i for i in items if i[0][:-4].lower() in want]
        if not items:
            raise LiveError("--only matches no fixture")
    tag = "x4%08x" % int(time.time())
    os.makedirs(a.out, exist_ok=True)
    savedir = os.path.join(stage, "valve", "save")
    cfgpath = os.path.join(stage, CFG)
    if os.path.exists(cfgpath):
        raise LiveError("%s is already there" % cfgpath)
    ours = {"%s_%02d.sav" % (tag, i) for i in range(len(items))}
    stray = [n for n in (os.listdir(savedir) if os.path.isdir(savedir) else []) if n.lower() not in ours]
    if stray:
        raise LiveError("%s holds files that are not fixtures (%s): use a stage with no saves" % (savedir, ", ".join(stray[:5])))
    iso = os.path.join(a.out, "save-live.iso")
    labels = []
    try:
        shutil.copyfile(a.xbe, os.path.join(stage, "default.xbe"))
        os.makedirs(savedir, exist_ok=True)
        os.makedirs(os.path.dirname(cfgpath), exist_ok=True)
        lines = ["// tools/save_live_gate.py: remove after the run"]
        for i, (name, h, m, data) in enumerate(items):
            # a unique slot name: xemu's E: drive keeps saves of its own (autosave, quick, ...) and the engine reads the
            # writable drive before the disc, so a fixture under a common name would be shadowed
            slot = "%s_%02d" % (tag, i)
            with open(os.path.join(savedir, slot + ".sav"), "wb") as f:
                f.write(data)
            label = "%s_%02d" % (tag, i)
            labels.append(label)
            # out of any game first: a load that fails leaves the engine where it was, and the previous fixture's map
            # (often the same one) would otherwise count as this fixture playing
            lines += ["disconnect", "wait 120", 'host_frametime_label "%s"' % label, "host_frametime_analysis 2", "load %s" % slot,
                      "wait %d" % WAIT_FRAMES, "host_frametime_analysis 0", "wait 10"]
        lines.append("echo save-live-gate: done")
        with open(cfgpath, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        r = subprocess.run([EXTRACT, "-c", winpath(stage), winpath(iso)], capture_output=True, text=True)
        if r.returncode:
            raise LiveError("extract-xiso failed:\n" + r.stdout + r.stderr)
    finally:
        shutil.rmtree(savedir, ignore_errors=True)
        os.makedirs(savedir, exist_ok=True)
        if os.path.exists(cfgpath):
            os.remove(cfgpath)

    for label in labels:  # a label already on the disk would be read back as this run's session
        pre = subprocess.run([sys.executable, os.path.join(HERE, "frametime-report.py"), "fetch-xemu", "--out",
                              os.path.join(a.out, ".precheck"), "--label", label], capture_output=True, text=True, cwd=REPO)
        if pre.returncode == 0:
            raise LiveError("xemu's disk already has a session labelled %s: not starting" % label)
    shutil.rmtree(os.path.join(a.out, ".precheck"), ignore_errors=True)

    proc = subprocess.Popen([XEMU, "-dvd_path", winpath(iso)], stdout=open(os.path.join(a.out, "xemu.out"), "w"),
                            stderr=subprocess.STDOUT)
    time.sleep(15)
    pids = xemu_running()
    if not pids:
        raise LiveError("xemu did not start (see %s)" % os.path.join(a.out, "xemu.out"))
    budget = 25 + len(items) * (WAIT_FRAMES / 60.0 + 17)
    print("save_live_gate: xemu %s loading %d saves, about %d s" % (pids, len(items), budget))
    end = time.time() + budget
    while time.time() < end and xemu_running() == pids:
        time.sleep(5)
    if xemu_running() != pids:
        raise LiveError("xemu %s went away: not reading the disk" % pids)
    ids = ",".join(pids.split())
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-Process -Id %s | ForEach-Object { [void]$_.CloseMainWindow() }; "
                    "Get-Process -Id %s -ErrorAction SilentlyContinue | ForEach-Object { [void]$_.WaitForExit(60000) }" % (ids, ids)])
    if xemu_running():
        raise LiveError("xemu did not close within 60 s: not reading the disk")
    proc.wait()

    results = []
    for label, (name, h, m, data) in zip(labels, items):
        d = os.path.join(a.out, label)
        r = subprocess.run([sys.executable, os.path.join(HERE, "frametime-report.py"), "fetch-xemu", "--out", d, "--label", label],
                           capture_output=True, text=True, cwd=REPO)
        secs = 0.0
        if r.returncode == 0:
            s = subprocess.run([sys.executable, os.path.join(HERE, "frametime-report.py"), "summary", d, "--label", label],
                               capture_output=True, text=True, cwd=REPO)
            secs = played(s.stdout, m)
        results.append((name, h, m, secs, "PASS" if secs >= MIN_PLAYED else "FAIL"))
        print("  %-18s %-8s %5.1f s  %s" % (name, m, secs, results[-1][4]))
    with open(os.path.join(a.out, EVIDENCE), "w", encoding="utf-8", newline="\n") as f:
        f.write("# save_live_gate evidence: the XBE, then every fixture it loaded in xemu and whether its map played\n")
        f.write("xbe %s\n" % sha(xbe))
        for name, h, m, secs, v in results:
            f.write("fixture %s %s %s %.1f %s\n" % (h, name.replace(" ", "_"), m, secs, v))
    bad = [r for r in results if r[4] != "PASS"]
    if bad:
        raise LiveError("%d of %d saves did not load and play: %s" % (len(bad), len(results), ", ".join(r[0] for r in bad)))
    print("save_live_gate: all %d saves loaded and played; evidence %s" % (len(results), os.path.join(a.out, EVIDENCE)))


def evidence_problems(xbe_bytes, evidence_path, expected_path=None):
    expected_path = expected_path or host.EXPECTED
    try:
        with open(evidence_path, encoding="utf-8") as f:
            lines = [l.split() for l in f if l.strip() and not l.startswith("#")]
        with open(expected_path, encoding="utf-8") as f:
            expected = host.parse_expected(f.read())
    except OSError as e:
        return ["cannot read the live evidence: %s" % e]
    xbes = [l[1] for l in lines if l[0] == "xbe" and len(l) == 2]
    if xbes != [sha(xbe_bytes)]:
        return ["the live evidence is for another XBE (%s), not this one (%s)" % (",".join(x[:12] for x in xbes) or "none", sha(xbe_bytes)[:12])]
    got = {l[1]: l for l in lines if l[0] == "fixture" and len(l) == 6}
    out = []
    for h, (name, _) in expected.items():
        if not name.lower().endswith(".sav"):
            continue
        if h not in got:
            out.append("%s (%s) was not loaded in the live run" % (name, h[:12]))
        elif got[h][5] != "PASS":
            out.append("%s did not load and play in the live run" % name)
    return out


def require(xbe_bytes, evidence=None):
    """For the release path: SystemExit unless the live evidence is for exactly these XBE bytes and every save passed. The
    evidence is the file named by R3D_SAVE_LIVE_EVIDENCE, else tools/save-live-evidence/<sha256 of the XBE>.txt."""
    evidence = evidence or os.environ.get("R3D_SAVE_LIVE_EVIDENCE") or os.path.join(COMMITTED, sha(xbe_bytes) + ".txt")
    p = evidence_problems(xbe_bytes, evidence)
    if p:
        raise SystemExit("The live save gate failed, so this build must not be published (docs/r3d/fork-plan.md, XS.4). "
                         "Run tools/save_live_gate.py run on this XBE (it needs xemu and the fixture saves) and commit its "
                         "evidence as tools/save-live-evidence/%s.txt:" % sha(xbe_bytes)
                         + "".join("\n  - " + x for x in p))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--xbe", required=True)
    r.add_argument("--stage", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--only", help="comma separated slot names, for diagnosis (no evidence is valid for a subset)")
    v = sub.add_parser("verify")
    v.add_argument("xbe")
    v.add_argument("evidence")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "run":
            run(a)
        else:
            with open(a.xbe, "rb") as f:
                require(f.read(), a.evidence)
            print("live evidence ok")
    except LiveError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
