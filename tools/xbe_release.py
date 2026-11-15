#!/usr/bin/env python3
"""Prepare (and, only when told, publish) an `xbe-<commit>` release: the raw Xbox build the packager's workflow turns into the
Windows release (docs/r3d/fork-plan.md, XS.7).

    python tools/xbe_release.py check   --xbe default.xbe --commit <40-hex>
    python tools/xbe_release.py publish --xbe default.xbe --commit <40-hex> [--notes notes.md] --publish

`check` (and `publish` without --publish) changes nothing: it runs every gate and prints the release it would make. A release is
refused unless ALL of these hold, so an XBE that could make a player's saved game unloadable is never put where the packager
and the players can reach it:

  * the XBE is a real build of the commit named: this checkout is at that commit, and the commit is on origin/main;
  * the title id is the one the saves live under (tools/save_rules_check.py xbe), and the source keeps the save format and the
    limits (`save_rules_check.py source`);
  * the live save gate ran on exactly these bytes and every fixture save loaded and played in it
    (tools/save_live_gate.py; its evidence is committed as tools/save-live-evidence/<sha256>.txt);
  * its link map is beside it (`default.map`, written by tools/xbox-game-link-check.sh) with the build's own `.identity`
    naming the same XBE hash, so the map published is the map of the bytes published.

What it publishes: default.xbe, default.map (the link map: what a later build's offsets and names were measured against; the
frozen legacy table came from exactly such a map) and save-live-evidence.txt (the save baseline the build was checked against:
fixture hashes, the maps they play, PASS), and the release notes carry the hashes. Publishing needs `--publish`; without it this
only prints. The tool never touches a console.
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import save_live_gate  # noqa: E402
import save_rules_check  # noqa: E402

GH_REPO = "mathiasdbo/Xash3D-FWGS-XBOX"


class ReleaseError(Exception):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(*args):
    r = subprocess.run(["git", "-C", REPO] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


def read_identity(path):
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            if "=" in line:
                k, _, v = line.strip().partition("=")
                out[k] = v
    return out


def problems(xbe_path, commit, map_path=None, identity_path=None, evidence=None, runner=None, source_check=None):
    """every reason this XBE must not be released (empty: it may be)"""
    runner = runner or git
    out = []
    if not re.fullmatch(r"[0-9a-f]{40}", commit or ""):
        return ["--commit must be the full 40-hex commit the XBE was built from"]
    try:
        with open(xbe_path, "rb") as f:
            xbe = f.read()
    except OSError as e:
        return ["cannot read the XBE: %s" % e]

    # the commit
    rc, head = runner("rev-parse", "HEAD")
    if rc or head != commit:
        out.append("this checkout is at %s, not at %s: the source rules below are read from the checkout, so check the build's "
                   "commit out" % ((head or "an unknown commit")[:10], commit[:10]))
    rc, _ = runner("merge-base", "--is-ancestor", commit, "origin/main")
    if rc:
        out.append("%s is not on origin/main (fetch it, or merge first): releases are built from main" % commit[:10])

    # the save rules
    out += save_rules_check.xbe_problems(xbe)
    out += (source_check or save_rules_check.source_problems)()
    # the live gate, bound to these bytes
    ev = evidence or os.path.join(save_live_gate.COMMITTED, sha(xbe) + ".txt")
    out += save_live_gate.evidence_problems(xbe, ev)

    # the link map, bound to the XBE
    map_path = map_path or os.path.splitext(xbe_path)[0] + ".map"
    identity_path = identity_path or xbe_path + ".identity"
    m = None
    try:
        with open(map_path, "rb") as f:
            m = f.read()
        if len(m) < 1000 or b"GameDLLInit" not in m:
            out.append("%s is not the link map of a game build (too small, or no GameDLLInit in it)" % map_path)
    except OSError as e:
        out.append("no link map at %s (%s): xbox-game-link-check.sh writes it beside the XBE" % (map_path, e))
    try:
        ident = read_identity(identity_path)
        if ident.get("sha256_xbe") != sha(xbe):
            out.append("%s names XBE %s, not these bytes (%s): the map would not be this build's" % (
                identity_path, (ident.get("sha256_xbe") or "none")[:12], sha(xbe)[:12]))
        if ident.get("modules") != "server+client+freevgui":
            out.append("the build's modules are %r, not server+client+freevgui: not a bootable game build" % ident.get("modules"))
        # the identity is what binds the XBE to its source and to its map (xbox-game-link-check.sh writes all three)
        if not ident.get("commit"):
            out.append("%s does not record the commit or the map: rebuild with the current tools/xbox-game-link-check.sh" % identity_path)
        else:
            if ident["commit"] != commit:
                out.append("the XBE was built from %s, not from %s: its identity names the commit, and so must the release" % (
                    ident["commit"][:10], commit[:10]))
            if ident.get("dirty") != "0":
                out.append("the XBE was built with tracked files changed from its commit (dirty=%s): build from a clean checkout" %
                           ident.get("dirty"))
            if ident.get("sha256_map") != (sha(m) if m is not None else None):
                out.append("the map at %s is not the map of this build (the identity names %s): the published map must be the one "
                           "the XBE was linked with" % (map_path, (ident.get("sha256_map") or "none")[:12]))
    except OSError as e:
        out.append("no build identity at %s (%s)" % (identity_path, e))
    return out


def notes_text(xbe, commit, extra, map_bytes, evidence_path):
    with open(evidence_path, encoding="utf-8") as f:
        fixtures = sum(1 for l in f if l.startswith("fixture "))
    lines = [
        "Not a download for users: the raw Xbox build (default.xbe, %d bytes) the packager workflow turns into the Windows release." % len(xbe),
        "",
        "- Built from commit %s." % commit,
        "- default.xbe sha256: %s" % sha(xbe),
        "- default.map sha256: %s (the link map of this build)." % sha(map_bytes),
        "- Save baseline: %d fixture saves loaded and played in this build in xemu (save-live-evidence.txt, sha256 %s; "
        "tools/save-live-evidence/%s.txt in the repository)." % (fixtures, sha(open(evidence_path, "rb").read()), sha(xbe)),
        "- Saved games from v0.1.2, v0.1.3 and v0.2.0 are read through the frozen legacy table; new saves are written by function name.",
    ]
    if extra:
        lines += ["", extra.strip()]
    return "\n".join(lines) + "\n"


def command(xbe_path, commit, notes_file, evidence, map_path, runner_gh=None):
    tag = "xbe-" + commit[:8]
    with open(xbe_path, "rb") as f:
        xbe = f.read()
    ev = evidence or os.path.join(save_live_gate.COMMITTED, sha(xbe) + ".txt")
    map_path = map_path or os.path.splitext(xbe_path)[0] + ".map"
    extra = ""
    if notes_file:
        with open(notes_file, encoding="utf-8") as f:
            extra = f.read()
    with open(map_path, "rb") as f:
        mp = f.read()
    body = notes_text(xbe, commit, extra, mp, ev)
    stage = os.path.join(os.path.dirname(os.path.abspath(xbe_path)), "release-" + tag)
    return tag, body, [(xbe_path, "default.xbe"), (map_path, "default.map"), (ev, "save-live-evidence.txt")], stage


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("check", "publish"):
        p = sub.add_parser(name)
        p.add_argument("--xbe", required=True)
        p.add_argument("--commit", required=True)
        p.add_argument("--map")
        p.add_argument("--identity")
        p.add_argument("--evidence")
        p.add_argument("--notes")
        if name == "publish":
            p.add_argument("--publish", action="store_true", help="really create the release (without it, only prints)")
    a = ap.parse_args(argv)
    p = problems(a.xbe, a.commit, a.map, a.identity, a.evidence)
    if p:
        sys.exit("This XBE must not be released (docs/r3d/fork-plan.md, XS):\n  - " + "\n  - ".join(p))
    tag, body, assets, stage = command(a.xbe, a.commit, a.notes, a.evidence, a.map)
    print("Every gate passes. The release would be %s with:" % tag)
    for src, name in assets:
        print("  %s  <-  %s" % (name, src))
    print("\n" + body)
    if a.cmd == "check" or not getattr(a, "publish", False):
        print("Nothing was published (add --publish to publish).")
        return
    os.makedirs(stage, exist_ok=True)
    args = []
    for src, name in assets:
        dst = os.path.join(stage, name)
        with open(src, "rb") as f, open(dst, "wb") as g:
            g.write(f.read())
        args.append(dst)
    notes_path = os.path.join(stage, "notes.md")
    with open(notes_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)
    r = subprocess.run(["gh", "release", "create", tag, "--repo", GH_REPO, "--prerelease", "--target", a.commit,
                        "--title", "Xbox build input (%s)" % a.commit[:8], "--notes-file", notes_path] + args)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
