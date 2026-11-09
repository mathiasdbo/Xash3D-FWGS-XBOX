#!/usr/bin/env python3
"""Smoke test of the campaign's level transitions on a real Xbox.

Walks the Half-Life maps in game order with `changelevel2 <next> <landmark>`,
staying DWELL seconds on each, with the frame-time recorder running, and says
from the recording which map the game died or hung on, if any. The list of
steps is tools/smoke/hl-campaign.txt (tools/smoke/make-map-list.py writes it
from the retail maps).

  stage:   python tools/xbox-smoke-transitions.py stage --host <ip> [--dwell 30] [--from c1a2]
  (boot the game from the dashboard, let it run; it ends by itself and quits)
  fetch:   python tools/xbox-smoke-transitions.py fetch --host <ip>
  check:   python tools/xbox-smoke-transitions.py check
  status:  python tools/xbox-smoke-transitions.py status --host <ip>
  restore: python tools/xbox-smoke-transitions.py restore --host <ip>

How the chain runs. A stage writes valve/userconfig.d/90-smoke.cfg (recorder on,
cheats, god and notarget so nobody dies, `map <first>`) and, for every map that
has a next step, valve/maps/<map>_load.cfg. The server runs that file when the
map loads (the mechanism tools/xbox-ftp-route-bmi.py uses for its end point): it
waits DWELL seconds worth of frames, then changes level. The last map stops the
recorder, records a short second session (<label>-end) as the marker that the
whole chain ran, and quits to the dashboard.

After a crash or a hang, power-cycle, then `fetch` and `check`: the report names
the map. `stage --from <next map>` continues the walk past it (the label changes,
so each attempt is a recording of its own); `check --all` joins the attempts.

FTP is unavailable while the game runs, and the console takes one connection at
a time. The console's default.xbe is whatever is on it: put the build under test
there first (the packager, or tools/xbox-ftp-route-bmi.py stage --xbe ...).

Limits, on purpose: the player never walks to the trigger, so it arrives offset
from the landmark; this tests loading and the transition and restore code, not
gameplay. A step whose list line has landmark "-" is a plain `map` load (the walk
had to jump back), reported as "direct".
"""
import argparse
import datetime
import ftplib
import importlib.util
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LIST = os.path.join(HERE, "smoke", "hl-campaign.txt")
OUT_ROOT = os.path.join(os.path.dirname(HERE), ".sweep", "smoke-transitions")
GAME_CANDIDATES = ["/HDD0-F/Games/Half-Life", "/HDD0-E/Games/Half-Life", "/HDD0-E/Games/R3D-X9K"]
CFG_NAME = "90-smoke.cfg"
OWNED = "90-smoke.maps"  # lists the maps/*_load.cfg a stage wrote (not a .cfg: never executed)
STATE = "90-smoke.state"  # the label and options of the last stage
DISABLE = ["00-c1a0.cfg", "91-frametime.cfg"]  # would load another map / set another label
CONFLICT = "90-route-bmi.cfg"  # the Black Mesa route: its own `map`, never run both
ARRIVE_WAIT = 240  # frames before god/notarget after a plain `map` load (the player has spawned)
FPS = 60


def say(*a):
    print(*a, flush=True)


def die(msg):
    sys.exit("xbox-smoke-transitions: " + msg)


# --------------------------------------------------------------------------
# The list and the plan
# --------------------------------------------------------------------------

def read_list(path):
    """-> (start map, [(from, to, landmark or '-')])"""
    start, steps = None, []
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            w = line.split()
            if w[0] == "start" and len(w) == 2:
                start = w[1]
            elif len(w) == 3:
                steps.append(tuple(w))
            else:
                die("%s:%d: cannot read %r" % (path, n, line))
    if not start:
        die("%s has no 'start' line" % path)
    return start, steps


def sequence(start, steps):
    """The maps in order, and how each is reached: 'start', 'transition' or 'direct'."""
    seq, how = [start], ["start"]
    for frm, to, lm in steps:
        if frm != seq[-1]:
            die("the list is not a chain: %s follows %s but the step starts from %s" % (to, seq[-1], frm))
        seq.append(to)
        how.append("direct" if lm == "-" else "transition")
    return seq, how


def plan(start, steps, from_map=None, count=None):
    """The chain a stage writes: [(map, how, next_command or None)], from `from_map` on."""
    seq, how = sequence(start, steps)
    first = 0
    if from_map:
        if from_map not in seq:
            die("%s is not in the list" % from_map)
        first = seq.index(from_map)
    chain = []
    for i in range(first, len(seq)):
        nxt = None
        if i + 1 < len(seq):
            lm = steps[i][2]
            nxt = ('map "%s"' % seq[i + 1]) if lm == "-" else ('changelevel2 "%s" "%s"' % (seq[i + 1], lm))
        chain.append((seq[i], "start" if i == first else how[i], nxt))
    if count:
        chain = chain[:count]
        chain[-1] = (chain[-1][0], chain[-1][1], None)  # the last map of a short chain ends it
    return chain


def cfg_lines(label, dwell_frames, chain, entry, end_label):
    m, how, nxt = entry
    lines = ["// tools/xbox-smoke-transitions.py (%s): remove after the run" % label,
             'echo "smoke: %s loaded (%s)"' % (m, how)]
    if how in ("start", "direct"):
        lines += ["wait %d" % ARRIVE_WAIT, "god", "notarget"]  # god and notarget toggle: only on a fresh map
    lines.append("wait %d" % dwell_frames)
    if nxt:
        lines.append(nxt)
    else:
        lines += ["host_frametime_analysis 0", "wait 10",
                  "// the end marker: a short session of its own, checked after the run",
                  'host_frametime_label "%s"' % end_label, "host_frametime_analysis 1", "wait 30",
                  "host_frametime_analysis 0", "echo smoke: done", "wait 120", "quit"]
    return lines


# --------------------------------------------------------------------------
# The console
# --------------------------------------------------------------------------

class DryRun:
    """Reads go to the console; writes are only reported."""

    def __init__(self, f):
        self.f = f

    def nlst(self, *a):
        return self.f.nlst(*a)

    def retrlines(self, *a):
        return self.f.retrlines(*a)

    def storbinary(self, cmd, fp, *a, **k):
        say("[dry run] %s (%d bytes)" % (cmd, len(fp.getvalue())))

    def rename(self, a, b):
        say("[dry run] RNFR %s -> RNTO %s" % (a, b))

    def delete(self, path):
        say("[dry run] DELE %s" % path)

    def quit(self):
        self.f.quit()

    def close(self):
        self.f.close()


def connect(a):
    say("connecting to %s ..." % a.host)
    f = ftplib.FTP(a.host, timeout=a.timeout)
    banner = f.getwelcome()
    f.login(a.user, a.password)
    f.set_pasv(True)
    if "unleashx" in banner.lower():
        f.close()
        die("this is UnleashX's FTP server (its LIST ignores the path). Start the game folder's server that lists "
            "HDD0-E / HDD0-F, or pass --game")
    return f


def listing(f, d):
    try:
        return set(os.path.basename(n) for n in f.nlst(d))
    except ftplib.error_perm:
        return set()


def game_dir(f, a):
    if a.game:
        return a.game.rstrip("/")
    for g in GAME_CANDIDATES:
        if "default.xbe" in listing(f, g):
            return g
    die("no game folder found (tried %s): pass --game" % ", ".join(GAME_CANDIDATES))


def put_text(f, path, lines, quiet=False):
    f.storbinary("STOR " + path, io.BytesIO(("\n".join(lines) + "\n").encode()))
    if not quiet:
        say("wrote %s (%d lines)" % (path, len(lines)))


def get_text(f, path):
    held = []
    try:
        f.retrlines("RETR " + path, held.append)
    except ftplib.error_perm:
        return None
    return held


def stage(f, a):
    start, steps = read_list(a.list)
    chain = plan(start, steps, a.from_map, a.count)
    game = game_dir(f, a)
    cfgdir, mapdir = game + "/valve/userconfig.d", game + "/valve/maps"
    label = a.label or "smk-" + datetime.datetime.now().strftime("%m%d-%H%M")
    if len(label) > 18:
        die("label %r is over 18 characters (the recorder keeps 22, and '-end' is added)" % label)
    cfgs, maps_there = listing(f, cfgdir), listing(f, mapdir)
    if CONFLICT in cfgs:
        die("%s is staged (the Black Mesa route loads its own map): run tools/xbox-ftp-route-bmi.py restore first" % CONFLICT)
    owned_before = set(l[3:].strip() for l in (get_text(f, cfgdir + "/" + OWNED) or []) if l.startswith("// "))
    for m, _, nxt in chain:
        n = m + "_load.cfg"
        if n in maps_there and m not in owned_before:
            die("maps/%s already exists on the console and is not this script's: sort it out by hand" % n)
    for n in DISABLE:
        if n in cfgs:
            if n + ".off" in cfgs:
                die("%s and %s.off both exist; sort them out by hand" % (n, n))
            f.rename(cfgdir + "/" + n, cfgdir + "/" + n + ".off")
            say("renamed %s -> %s.off" % (n, n))
    dwell_frames = int(a.dwell * FPS)
    end_label = label + "-end"
    # The list of files first, so restore finds every one even if an upload below fails
    put_text(f, cfgdir + "/" + OWNED, ["// " + m for m in sorted(owned_before | set(c[0] for c in chain))], quiet=True)
    for m in sorted(owned_before - set(c[0] for c in chain)):
        if m + "_load.cfg" in maps_there:
            f.delete(mapdir + "/" + m + "_load.cfg")
    for i, entry in enumerate(chain, 1):
        put_text(f, mapdir + "/" + entry[0] + "_load.cfg", cfg_lines(label, dwell_frames, chain, entry, end_label), quiet=True)
        if i % 10 == 0 or i == len(chain):
            say("  maps/*_load.cfg: %d of %d" % (i, len(chain)))
    put_text(f, cfgdir + "/" + OWNED, ["// " + m[0] for m in chain], quiet=True)
    put_text(f, cfgdir + "/" + CFG_NAME,
             ["// tools/xbox-smoke-transitions.py (%s): remove after the run" % label,
              'host_frametime_label "%s"' % label, "host_frametime_analysis 1", "sv_cheats 1",
              'map "%s"' % chain[0][0]])
    put_text(f, cfgdir + "/" + STATE, ["// " + json.dumps(dict(label=label, dwell=a.dwell, first=chain[0][0],
                                                                last=chain[-1][0], maps=[c[0] for c in chain],
                                                                list=os.path.basename(a.list)))], quiet=True)
    mins = len(chain) * (a.dwell + 8) / 60  # the wait, a load of a few seconds, the god/notarget wait on a plain load
    say("staged %s: %d maps from %s to %s, %g s each, about %.0f minutes." % (
        label, len(chain), chain[0][0], chain[-1][0], a.dwell, max(1, mins)))
    say("Boot the game from the dashboard. It quits to the dashboard when the chain is done. If it stops "
        "on a crash or hang, power-cycle it; then `fetch` and `check` (and `stage --from <next map>`).")


def status(f, a):
    game = game_dir(f, a)
    cfgdir, mapdir = game + "/valve/userconfig.d", game + "/valve/maps"
    say("game folder: " + game)
    say("userconfig.d: " + ", ".join(sorted(listing(f, cfgdir))))
    state = get_text(f, cfgdir + "/" + STATE)
    if state:
        s = json.loads(state[0][3:])
        say("staged: %s, %d maps %s -> %s, %g s each" % (s["label"], len(s["maps"]), s["first"], s["last"], s["dwell"]))
    owned = [l[3:].strip() for l in (get_text(f, cfgdir + "/" + OWNED) or []) if l.startswith("// ")]
    there = listing(f, mapdir)
    say("maps/*_load.cfg staged: %d of %d listed" % (sum(1 for m in owned if m + "_load.cfg" in there), len(owned)))


def restore(f, a):
    game = game_dir(f, a)
    cfgdir, mapdir = game + "/valve/userconfig.d", game + "/valve/maps"
    cfgs = listing(f, cfgdir)
    there = listing(f, mapdir)
    for l in (get_text(f, cfgdir + "/" + OWNED) or []):
        m = l[3:].strip() if l.startswith("// ") else ""
        if m and m + "_load.cfg" in there:
            f.delete(mapdir + "/" + m + "_load.cfg")
    say("removed the staged maps/*_load.cfg")
    for n in (OWNED, CFG_NAME, STATE):
        if n in cfgs:
            f.delete(cfgdir + "/" + n)
            say("removed " + n)
    for n in DISABLE:
        if n + ".off" in cfgs and n not in cfgs:
            f.rename(cfgdir + "/" + n + ".off", cfgdir + "/" + n)
            say("renamed %s.off -> %s" % (n, n))


# --------------------------------------------------------------------------
# fetch and check
# --------------------------------------------------------------------------

def load_ft():
    spec = importlib.util.spec_from_file_location("frametime_report", os.path.join(HERE, "frametime-report.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def last_stage_label(a):
    p = os.path.join(a.out, "last-label.txt")
    if a.label:
        return a.label
    if os.path.exists(p):
        return open(p).read().strip()
    die("no label: pass --label, or `fetch` first")


def fetch(f_unused, a):
    f = connect(a)
    try:
        game = game_dir(f, a)
        state = get_text(f, game + "/valve/userconfig.d/" + STATE)
    finally:
        try:
            f.quit()
        except Exception:
            f.close()
    label = a.label or (json.loads(state[0][3:])["label"] if state else None)
    if not label:
        die("the console has no staged label (already restored?): pass --label")
    os.makedirs(a.out, exist_ok=True)
    for lab in (label, label + "-end"):
        say("fetching %s ..." % lab)
        r = subprocess.run([sys.executable, os.path.join(HERE, "frametime-report.py"), "fetch", "--host", a.host,
                            "--out", a.out, "--label", lab], capture_output=True, text=True)
        if r.returncode and lab == label:
            die("fetch failed: " + (r.stderr or r.stdout).strip()[:300])
        if lab == label:
            say("  " + (r.stdout.strip().splitlines() or ["nothing fetched"])[-1][-120:])
        elif r.returncode:
            say("  no %s recording (the chain did not reach its end)" % lab)
    with open(os.path.join(a.out, "last-label.txt"), "w") as fh:
        fh.write(label + "\n")
    if state:
        with open(os.path.join(a.out, label + ".state.json"), "w") as fh:
            fh.write(state[0][3:] + "\n")
    say("fetched into %s. Next: check" % a.out)


def play_seconds(ft, rec):
    """{segment index: in-play seconds}, over the frames the recorder flagged active (not menu, not a load)"""
    out = {}
    for i, seg in enumerate(ft.segments(rec)):
        t = 0.0
        for fr in rec["frames"][seg["start"]:seg["end"]]:
            fl = fr[7]
            if (fl & ft.F_ACTIVE) and not (fl & ft.F_MENU) and not (fl & ft.F_LOAD):
                t += ft.frame_ms(rec, fr)[0] / 1000.0
        out[i] = t
    return out


def analyse(a):
    ft = load_ft()
    label = last_stage_label(a)
    sp = os.path.join(a.out, label + ".state.json")
    if not os.path.exists(sp):
        die("no %s: `fetch` first" % sp)
    state = json.load(open(sp))
    start, steps = read_list(a.list)
    seq, how_all = sequence(start, steps)
    first = seq.index(state["first"])
    expected = seq[first:first + len(state["maps"])]
    how = dict(zip(seq, how_all))
    problems, rows = [], []
    try:
        rec, _ = ft.resolve([a.out], label, None)
    except (ft.FormatError, OSError, ValueError) as e:
        return dict(label=label, state=state, rows=[], problems=["no recording of %s: %s (the game never booted into the "
                                                                   "chain, or the files were not fetched)" % (label, e)])
    try:
        end_rec, _ = ft.resolve([a.out], label + "-end", None)
    except (ft.FormatError, OSError, ValueError):
        end_rec = False
    segs = [s for s in ft.segments(rec) if s["map"] is not None]
    secs = play_seconds(ft, rec)
    seg_all = ft.segments(rec)
    played = {}
    order = []
    for s in seg_all:
        if s["map"] is None:
            continue
        name = s["map"]["name"]
        played[name] = played.get(name, 0.0) + secs[seg_all.index(s)]
        if not order or order[-1] != name:
            order.append(name)
    need = a.min_play if a.min_play is not None else state["dwell"] * 0.5
    died = None
    for m in expected:
        if m not in played:
            rows.append((m, how[m], None, "not reached"))
            continue
        t = played[m]
        rows.append((m, how[m], t, "ok" if t >= need else "short"))
    # Where it stopped: the last map in the recording when the recording has no END
    cut = rec["end"] is None or rec.get("truncated")
    if order and cut:
        last = order[-1]
        t = played.get(last, 0.0)
        died = (last, "while loading (no in-play frames)" if t < 0.5 else "after %.0f s in play" % t)
        problems.append("the game stopped on %s %s: crashed, hung, or was reset (the recording has no END chunk)" % died)
    elif not order:
        problems.append("the recording has no map at all")
    if order != expected[:len(order)]:
        problems.append("maps played %s, expected %s" % (" -> ".join(order[:8]) + (" ..." if len(order) > 8 else ""),
                                                          " -> ".join(expected[:8]) + " ..."))
    nxt = None
    if died and died[0] in expected:
        i = expected.index(died[0])
        nxt = expected[i + 1] if i + 1 < len(expected) else None
    shorts = [r[0] for r in rows if r[3] == "short" and not (died and r[0] == died[0])]
    for m in shorts:
        problems.append("%s: only %.0f s in play, expected at least %.0f s (the chain moved on early)" % (
            m, played[m], need))
    missing = [r[0] for r in rows if r[3] == "not reached"]
    if missing and not died:
        # The last segment closed cleanly and the next map never started: the game died (or hung) while
        # loading it, or on the transition into it
        prev = order[-1] if order else "the start"
        died = (missing[0], "while loading (its recording never started; after %s)" % prev)
        problems.append("the chain stopped before %s: the game crashed or hung loading it, or on the transition "
                        "from %s" % (missing[0], prev))
        i = expected.index(missing[0])
        nxt = expected[i + 1] if i + 1 < len(expected) else None
        if len(missing) > 1:
            problems.append("not reached after it: %s" % " ".join(missing[1:]))
    if not cut:
        if end_rec is False:
            problems.append("no end-marker recording: the last map's cfg did not run to its last line")
        elif end_rec["end"] is None or not end_rec["frames"]:
            problems.append("the end-marker recording is empty or cut short")
    return dict(label=label, state=state, rows=rows, problems=problems, died=died, resume=nxt, need=need)


def check(a):
    r = analyse(a)
    s = r["state"]
    say("smoke %s: %d maps, %s -> %s, %g s dwell" % (r["label"], len(s["maps"]), s["first"], s["last"], s["dwell"]))
    if r["rows"]:
        say("%-10s %-11s %9s  %s" % ("map", "arrival", "in play", "result"))
        for m, how, t, res in r["rows"]:
            say("%-10s %-11s %9s  %s" % (m, how, "-" if t is None else "%.1f s" % t, res))
    for p in r["problems"]:
        say("PROBLEM: " + p)
    if r["problems"]:
        if r.get("resume"):
            say("To go on past it: python tools/xbox-smoke-transitions.py stage --host <ip> --from %s --dwell %g" % (
                r["resume"], s["dwell"]))
        say("FAIL")
        return 1
    say("PASS: every map loaded and stayed %.0f s, the chain ran to its end" % r["need"])
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("action", choices=["stage", "status", "restore", "fetch", "check"])
    p.add_argument("--host", default="192.168.68.56")
    p.add_argument("--user", default="xbox")
    p.add_argument("--password", default="xbox")
    p.add_argument("--timeout", type=float, default=60.0)
    p.add_argument("--game", help="the game folder on the console (default: found under /HDD0-F, /HDD0-E)")
    p.add_argument("--list", default=DEFAULT_LIST, help="the transition list (default: %(default)s)")
    p.add_argument("--dwell", type=float, default=30.0, help="stage: seconds on each map (default %(default)g)")
    p.add_argument("--count", type=int, help="stage: only the first N maps from the start (a short pilot run)")
    p.add_argument("--dry-run", action="store_true", help="stage/restore: read the console, report what would be written")
    p.add_argument("--from", dest="from_map", help="stage: start the walk at this map (to continue past a crash)")
    p.add_argument("--label", help="stage: the recording's label (at most 18 characters); fetch/check: which run")
    p.add_argument("--out", default=OUT_ROOT, help="where fetch puts the recordings (default %(default)s)")
    p.add_argument("--min-play", type=float, help="check: in-play seconds a map must reach (default: half the dwell)")
    a = p.parse_args()
    if a.action == "check":
        sys.exit(check(a))
    if a.action == "fetch":
        fetch(None, a)
        return
    f = connect(a)
    if a.dry_run:
        f = DryRun(f)
    try:
        {"stage": stage, "status": status, "restore": restore}[a.action](f, a)
    finally:
        try:
            f.quit()
        except Exception:
            f.close()


if __name__ == "__main__":
    main()
