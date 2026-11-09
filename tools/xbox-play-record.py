#!/usr/bin/env python3
"""Record a normal play session on the console: the frame-time recorder on, nothing else changed.

  stage:   python tools/xbox-play-record.py stage --host <ip> [--label play-1109] [--overlay]
  (boot the game from the dashboard and play; quit to the dashboard when done)
  fetch:   python tools/xbox-play-record.py fetch --host <ip>
  report:  python tools/xbox-play-record.py report
  restore: python tools/xbox-play-record.py restore --host <ip>

stage writes one file, valve/userconfig.d/90-playthrough.cfg: a recorder label, host_frametime_analysis (1,
or 2 with --overlay: the frame-time graph over the HUD) and cl_showfps 1 (the fps counter). It loads no map and
changes no other file; the game's default.xbe is whatever is on the console. restore deletes that file.

report prints the per-map table of the session (average and 1% low fps, CPU and GPU ms, share of frames over
16.7 and 33.3 ms, load time, free memory) and writes the HTML report next to the recordings.

Recordings are written every 5 seconds and at each map start, so a crash costs at most the last 5 seconds.
The recorder is not saved to config.cfg on purpose: delete the cfg and the next boot records nothing.
FTP is unavailable while the game runs; the console takes one connection at a time.
"""
import argparse
import datetime
import importlib.util
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_ROOT = os.path.join(os.path.dirname(HERE), ".sweep", "play-record")
CFG = "90-playthrough.cfg"
STATE = "90-playthrough.state"


def _load(name, file):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, file))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


smoke = _load("xbox_smoke_transitions", "xbox-smoke-transitions.py")  # the console FTP helpers


def paths(f, a):
    game = smoke.game_dir(f, a)
    return game + "/valve/userconfig.d/"


def stage(f, a):
    d = paths(f, a)
    there = smoke.listing(f, d)
    if "90-smoke.cfg" in there:
        smoke.die("a smoke-test chain is staged (90-smoke.cfg): `xbox-smoke-transitions.py restore` first, "
                  "or the game would run it instead of starting at the menu")
    label = a.label or "play-" + datetime.datetime.now().strftime("%m%d-%H%M")
    if len(label) > 22 or not all(c.isalnum() or c in "-_" for c in label):
        smoke.die("label %r: letters, digits, - and _ only, at most 22 characters" % label)
    lines = ["// tools/xbox-play-record.py (%s): remove after the session" % label,
             'host_frametime_label "%s"' % label,
             "host_frametime_analysis %d" % (2 if a.overlay else 1),
             "cl_showfps 1"]
    smoke.put_text(f, d + CFG, lines)
    smoke.put_text(f, d + STATE, ["// " + json.dumps(dict(label=label))], quiet=True)
    smoke.say("staged %s. Boot the game and play; quit to the dashboard when done, then `fetch`." % label)


def restore(f, a):
    d = paths(f, a)
    there = smoke.listing(f, d)
    for n in (CFG, STATE):
        if n in there:
            f.delete(d + n)
            smoke.say("removed " + n)


def fetch(a):
    f = smoke.connect(a)
    try:
        held = smoke.get_text(f, paths(f, a) + STATE)
    finally:
        try:
            f.quit()
        except Exception:
            f.close()
    label = a.label or (json.loads(held[0][3:])["label"] if held else None)
    if not label:
        smoke.die("no staged label on the console: pass --label")
    os.makedirs(a.out, exist_ok=True)
    r = subprocess.run([sys.executable, os.path.join(HERE, "frametime-report.py"), "fetch", "--host", a.host,
                        "--out", a.out, "--label", label], capture_output=True, text=True)
    if r.returncode:
        smoke.die("fetch failed: " + (r.stderr or r.stdout).strip()[:300])
    smoke.say((r.stdout.strip().splitlines() or ["nothing fetched"])[-1][-150:])
    with open(os.path.join(a.out, "last-label.txt"), "w") as fh:
        fh.write(label + "\n")
    smoke.say("fetched %s into %s. Next: report" % (label, a.out))


def report(a):
    p = os.path.join(a.out, "last-label.txt")
    label = a.label or (open(p).read().strip() if os.path.exists(p) else None)
    if not label:
        smoke.die("no label: pass --label, or `fetch` first")
    rc = subprocess.call([sys.executable, os.path.join(HERE, "frametime-report.py"), "report", a.out, "--label", label,
                          "-o", os.path.join(a.out, label + ".html"), "--title", "Play session %s" % label])
    sys.exit(rc)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("action", choices=["stage", "restore", "fetch", "report"])
    p.add_argument("--host", default="192.168.68.56")
    p.add_argument("--user", default="xbox")
    p.add_argument("--password", default="xbox")
    p.add_argument("--timeout", type=float, default=60.0)
    p.add_argument("--game", help="the game folder on the console (default: found under /HDD0-F, /HDD0-E)")
    p.add_argument("--label", help="stage: the session's label; fetch/report: which session")
    p.add_argument("--overlay", action="store_true", help="stage: also draw the frame-time graph over the HUD")
    p.add_argument("--out", default=OUT_ROOT)
    a = p.parse_args()
    if a.action == "fetch":
        fetch(a)
    elif a.action == "report":
        report(a)
    else:
        f = smoke.connect(a)
        try:
            {"stage": stage, "restore": restore}[a.action](f, a)
        finally:
            try:
                f.quit()
            except Exception:
                f.close()


if __name__ == "__main__":
    main()
