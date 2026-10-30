#!/usr/bin/env python3
"""Stage the Black Mesa Inbound route on the console over FTP (PrometheOS).

The console counterpart of tools/xemu-route-bmi.sh: the same two route
files, written into the game folder instead of a packed ISO.

  stage:   python tools/xbox-ftp-route-bmi.py stage --label bmi-al-a \
               --xbe .sweep/pending-xbox/x9-audio-al-output-d9cb8072.xbe \
               --cfg "r3d_snd_backend sdl"
  status:  python tools/xbox-ftp-route-bmi.py status
  restore: python tools/xbox-ftp-route-bmi.py restore [--xbe-back]

stage:
  1. keeps the console's default.xbe as default.xbe.<keep> (once: an
     existing default.xbe.<keep> is never overwritten, so a stage after a
     broken upload does not lose the original);
  2. uploads --xbe as default.xbe, printing progress, checks the size
     afterwards and leaves its sha256 in default.xbe.sha256; skipped only
     when that file says this very build is already there;
  3. renames the cfgs that would fight the route (00-c1a0.cfg, which
     loads a map, and 91-frametime.cfg, which sets another label) to
     .off;
  4. writes valve/userconfig.d/90-route-bmi.cfg (label, recorder on, the
     --cfg lines, map c0a0) and valve/maps/c0a0e_load.cfg (the end point);
     --map-cfg MAP=LINE adds LINE to maps/MAP_load.cfg, which the server
     runs at that map's load (sv_init.c) - a setting per map, e.g. an
     anti-aliasing mode each, compared in one run.
  Every step can be re-run: a stage interrupted half way is finished by
  running the same command again.

Then boot the game, let it run about 7 minutes (it ends on c0a0e with
"route-bmi: done"), go back to the dashboard and fetch:
  python tools/frametime-report.py fetch --host <ip> --out .sweep/route-bmi/<label> --label <label>

restore removes the two route files and renames the .off cfgs back;
--xbe-back also puts default.xbe.<keep> back as default.xbe.

FTP is unavailable while the game runs; the console takes one connection
at a time, so close any other FTP client first.
"""
import argparse
import ftplib
import hashlib
import io
import os
import sys

GAME = "/HDD0-E/Games/R3D-X9K"
CFGDIR = GAME + "/valve/userconfig.d"
MAPDIR = GAME + "/valve/maps"
ROUTE_CFG = "90-route-bmi.cfg"
MAPLIST = "90-route-bmi.maps"  # the maps/*_load.cfg --map-cfg wrote (not a .cfg: never executed)
END_CFG = "c0a0e_load.cfg"
SHA_FILE = "default.xbe.sha256"  # the sha256 of the last default.xbe this script uploaded
DISABLE = ["00-c1a0.cfg", "91-frametime.cfg"]

END_LINES = [
    "// route-bmi: the route's end point on c0a0e",
    "sv_enttools_enable 1",
    "wait 2700",
    "ent_fire barnwalk3 use",
    "wait 1200",
    "host_frametime_analysis 0",
    "echo route-bmi: done",
    "wait 120",
    "quit",  # the recorder's session closes; a build with #179 then launches the dashboard
]


def say(*a):
    print(*a, flush=True)


def connect(a):
    say("connecting to %s ..." % a.host)
    f = ftplib.FTP(a.host, timeout=a.timeout)
    f.login(a.user, a.password)
    f.set_pasv(True)
    return f


def listing(f, d):
    try:
        return set(os.path.basename(n) for n in f.nlst(d))
    except ftplib.error_perm:
        return set()


def size(f, path):
    try:
        f.voidcmd("TYPE I")
        return f.size(path)
    except ftplib.error_perm:
        return None


def put_text(f, path, lines):
    data = ("\n".join(lines) + "\n").encode()
    f.storbinary("STOR " + path, io.BytesIO(data))
    say("wrote %s:" % path)
    for l in lines:
        say("    " + l)


def map_cfgs(a):
    """--map-cfg MAP=LINE, grouped: {map: [lines]}"""
    out = {}
    for item in a.map_cfg or []:
        m, sep, line = item.partition("=")
        if not sep or not m or not line:
            sys.exit("--map-cfg wants MAP=LINE, not %r" % item)
        out.setdefault(m, []).append(line)
    return out


def read_maplist(f):
    """The maps whose MAP_load.cfg a stage wrote (90-route-bmi.maps), [] without one"""
    if MAPLIST not in listing(f, CFGDIR):
        return []
    held = []
    f.retrlines("RETR " + CFGDIR + "/" + MAPLIST, held.append)
    return [l[3:].strip() for l in held if l.startswith("// ") and l[3:].strip()]


def status(f, a):
    g = listing(f, GAME)
    for n in sorted(x for x in g if x.startswith("default.xbe")):
        say("%-28s %s" % (n, size(f, GAME + "/" + n)))
    say("userconfig.d: " + ", ".join(sorted(listing(f, CFGDIR))))
    say("maps/%s: %s" % (END_CFG, "present" if END_CFG in listing(f, MAPDIR) else "absent"))


def stage(f, a):
    local = os.path.getsize(a.xbe)
    g = listing(f, GAME)
    keep = "default.xbe." + a.keep
    if keep in g:
        say("%s already kept; default.xbe will be overwritten" % keep)
    elif "default.xbe" in g:
        f.rename(GAME + "/default.xbe", GAME + "/" + keep)
        say("kept the console's default.xbe as " + keep)

    # Builds are page-rounded, so two builds are often the same size: the
    # skip goes by the sha256 this script left next to the last upload
    with open(a.xbe, "rb") as fh:
        digest = hashlib.sha256(fh.read()).hexdigest()
    sha_path = GAME + "/" + SHA_FILE
    held = []
    try:
        f.retrlines("RETR " + sha_path, held.append)
    except ftplib.error_perm:
        pass
    if held and held[0].strip() == digest and size(f, GAME + "/default.xbe") == local:
        say("default.xbe already this build (sha256 %s), upload skipped" % digest[:12])
    else:
        if SHA_FILE in listing(f, GAME):
            f.delete(sha_path)  # an upload cut short must not look finished
        sent = [0, -1]

        def tick(block):
            sent[0] += len(block)
            pct = sent[0] * 100 // local
            if pct // 10 != sent[1]:
                sent[1] = pct // 10
                say("  uploading default.xbe: %3d%% (%d / %d)" % (pct, sent[0], local))

        with open(a.xbe, "rb") as fh:
            f.storbinary("STOR " + GAME + "/default.xbe", fh, blocksize=32768, callback=tick)
        got = size(f, GAME + "/default.xbe")
        if got != local:
            sys.exit("default.xbe is %s bytes on the console, %d here: run stage again" % (got, local))
        f.storbinary("STOR " + sha_path, io.BytesIO((digest + "\n").encode()))
        say("default.xbe uploaded, %d bytes, sha256 %s" % (got, digest[:12]))

    cfgs = listing(f, CFGDIR)
    for n in DISABLE:
        if n in cfgs:
            if n + ".off" in cfgs:
                sys.exit("%s and %s.off both exist; sort them out by hand" % (n, n))
            f.rename(CFGDIR + "/" + n, CFGDIR + "/" + n + ".off")
            say("renamed %s -> %s.off" % (n, n))

    put_text(f, CFGDIR + "/" + ROUTE_CFG,
             ["// tools/xbox-ftp-route-bmi.py (%s): remove after the run" % a.label,
              'host_frametime_label "%s"' % a.label,
              "host_frametime_analysis %d" % (1 if a.no_overlay else 2)] + list(a.cfg) + ["map c0a0"])
    per_map = map_cfgs(a)
    wanted = sorted(m for m in per_map if m + "_load.cfg" != END_CFG)
    # Every file this script may own: the ones an earlier stage listed (a
    # stage cut short, or a rerun) and the ones asked for now. Anything
    # else already there is not ours - checked for every map before a
    # single file is written.
    owned = set(read_maplist(f))
    maps_there = listing(f, MAPDIR)
    for m in wanted:
        if m + "_load.cfg" in maps_there and m not in owned:
            sys.exit("maps/%s_load.cfg already exists on the console; it is not this script's - sort it out by hand" % m)
    # The list first, so restore finds every file even if an upload below fails
    put_text(f, CFGDIR + "/" + MAPLIST, ["// " + m for m in sorted(owned | set(wanted))])
    for m in wanted:
        put_text(f, MAPDIR + "/" + m + "_load.cfg", ["// tools/xbox-ftp-route-bmi.py (%s): remove after the run" % a.label] + per_map[m])
    # Listed by an earlier stage but not asked for now: theirs no longer
    for m in sorted(owned - set(wanted)):
        if m + "_load.cfg" in maps_there:
            f.delete(MAPDIR + "/" + m + "_load.cfg")
            say("removed maps/%s_load.cfg (an earlier stage's)" % m)
    if owned - set(wanted):
        put_text(f, CFGDIR + "/" + MAPLIST, ["// " + m for m in wanted])
    # --stay: the route opens the tram and hands over; the recording runs on until the game quits
    end = END_LINES[:END_LINES.index("ent_fire barnwalk3 use") + 1] + ["echo route-bmi: done, play on"] if a.stay else END_LINES
    put_text(f, MAPDIR + "/" + END_CFG, end[:1] + per_map.get(END_CFG[:-len("_load.cfg")], []) + end[1:])
    say("staged %s. Boot the game; it ends on c0a0e after about 7 minutes%s." % (a.label, "" if a.stay else " and quits"))


def restore(f, a):
    cfgs = listing(f, CFGDIR)
    if MAPLIST in cfgs:
        maps_there = listing(f, MAPDIR)
        for m in read_maplist(f):
            n = m + "_load.cfg"
            if n in maps_there:
                f.delete(MAPDIR + "/" + n)
                say("removed maps/" + n)
        f.delete(CFGDIR + "/" + MAPLIST)
    if ROUTE_CFG in cfgs:
        f.delete(CFGDIR + "/" + ROUTE_CFG)
        say("removed " + ROUTE_CFG)
    if END_CFG in listing(f, MAPDIR):
        f.delete(MAPDIR + "/" + END_CFG)
        say("removed maps/" + END_CFG)
    for n in DISABLE:
        if n + ".off" in cfgs and n not in cfgs:
            f.rename(CFGDIR + "/" + n + ".off", CFGDIR + "/" + n)
            say("renamed %s.off -> %s" % (n, n))
    if a.xbe_back:
        keep = "default.xbe." + a.keep
        if keep not in listing(f, GAME):
            sys.exit("no %s to put back" % keep)
        if "default.xbe" in listing(f, GAME):
            f.delete(GAME + "/default.xbe")
        f.rename(GAME + "/" + keep, GAME + "/default.xbe")
        if SHA_FILE in listing(f, GAME):
            f.delete(GAME + "/" + SHA_FILE)
        say("put %s back as default.xbe" % keep)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("action", choices=["stage", "status", "restore"])
    p.add_argument("--host", default="192.168.68.53")
    p.add_argument("--user", default="xbox")
    p.add_argument("--password", default="xbox")
    p.add_argument("--timeout", type=float, default=60.0)
    p.add_argument("--label", help="stage: the recording's label")
    p.add_argument("--xbe", help="stage: the XBE to run")
    p.add_argument("--map-cfg", action="append", help="stage: MAP=LINE, a line for maps/MAP_load.cfg (repeatable)")
    p.add_argument("--cfg", action="append", default=[], help="stage: an extra cfg line (repeatable)")
    p.add_argument("--no-overlay", action="store_true", help="stage: record without drawing the overlay (host_frametime_analysis 1)")
    p.add_argument("--stay", action="store_true", help="stage: at the end point hand over and keep recording (quit from the menu to close it)")
    p.add_argument("--keep", default="voxstream", help="suffix the console's own default.xbe is kept under")
    p.add_argument("--xbe-back", action="store_true", help="restore: put default.xbe.<keep> back too")
    a = p.parse_args()
    if a.action == "stage" and not (a.label and a.xbe):
        p.error("stage needs --label and --xbe")
    f = connect(a)
    try:
        {"stage": stage, "status": status, "restore": restore}[a.action](f, a)
        if a.action != "status":
            say("--- now:")
            status(f, a)
    finally:
        try:
            f.quit()
        except Exception:
            f.close()


if __name__ == "__main__":
    main()
