#!/usr/bin/env python3
"""Do the saved games players really have still read in this build? (docs/r3d/fork-plan.md, XS.4)

    R3D_SAVE_FIXTURES=<folder of saves> python tools/save_fixture_gate.py check
    python tools/save_fixture_gate.py scan <folder> --out tools/save-fixtures-expected.txt

The fixtures are real saves from real consoles, so they stay outside the repository. What is checked in
(tools/save-fixtures-expected.txt) is derived and holds nothing personal: per fixture, the SHA-256 of the file, its
name (a map or slot name) and the function names its callback pointers must resolve to, with how many times.

`check` finds the fixtures in the folder named by R3D_SAVE_FIXTURES, by hash (a renamed file still counts), and runs
every `ofs:N` string in each through the ENGINE's own reader (engine/common/save_legacy.c, compiled on the host), not
through a copy of the table in Python. So a build whose reader or table lost an entry fails here, which is what the
plan's exit asks for. It fails when:

  * the variable is unset, or the folder is missing or holds no fixture: a release is refused, never skipped;
  * a fixture named in the expected file is not in the folder;
  * a pointer in a fixture does not resolve (an offset no public build wrote), or resolves to a different name than
    the expected file says.

This is the host level. The live level (xemu boots the candidate XBE, loads each save, reaches play) is the other half
of XS.4 and is not here yet; until it is, this gate says the pointers read, not that the whole save plays.
"""

import argparse
import collections
import hashlib
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
EXPECTED = os.path.join(HERE, "save-fixtures-expected.txt")
ENV = "R3D_SAVE_FIXTURES"
RESOLVER_C = os.path.join(HERE, "host-tests", "save_legacy_resolve.c")
OFS = re.compile(rb"ofs:(-?\d+)\0")
HEAD = "# XS.4 save fixtures: what every callback pointer in each real save must resolve to. Derived; holds no save content.\n"


class GateError(Exception):
    pass


def sha(data):
    return hashlib.sha256(data).hexdigest()


def refs_of(data):
    """the pointer strings in a save, as 'ofs:N' text"""
    return [b"ofs:" + m.group(1) for m in OFS.finditer(data)]


def build_resolver(out_dir):
    cc = os.environ.get("CC") or "gcc"
    exe = os.path.join(out_dir, "save_legacy_resolve" + (".exe" if os.name == "nt" else ""))
    cmd = [cc, "-O2", "-std=c99", "-I" + os.path.join(REPO, "engine", "common"), RESOLVER_C, "-o", exe]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
    except OSError as e:
        raise GateError("cannot run the C compiler %r: %s" % (cc, e))
    if r.returncode:
        raise GateError("the engine's save reader does not compile:\n" + r.stderr)
    return exe


def resolve_all(exe, strings):
    """engine verdicts, one per string: 'OK name', 'BYFIELD', 'UNKNOWN' or 'BAD'"""
    uniq = sorted(set(strings))
    if not uniq:
        return {}
    r = subprocess.run([exe], input="\n".join(s.decode() for s in uniq) + "\n", capture_output=True, text=True)
    out = r.stdout.splitlines()
    if r.returncode or len(out) != len(uniq):
        raise GateError("the engine's save reader gave %d answers for %d pointers (exit %d)" % (len(out), len(uniq), r.returncode))
    return dict(zip((s.decode() for s in uniq), out))


def summarise(data, verdict):
    """name -> count for a save's pointers; verdicts that are not a name are kept as their own keys"""
    c = collections.Counter()
    for s in refs_of(data):
        v = verdict[s.decode()]
        c[v[3:] if v.startswith("OK ") else v] += 1
    return c


def fmt(counter):
    return ";".join("%s=%d" % kv for kv in sorted(counter.items())) or "-"


def parse_expected(text):
    rows = {}
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split(" ")
        if len(parts) != 4 or parts[0] != "fixture" or len(parts[1]) != 64:
            raise GateError("bad line in the expected file: %r" % line)
        c = collections.Counter()
        if parts[3] != "-":
            for item in parts[3].split(";"):
                k, _, n = item.rpartition("=")
                c[k] = int(n)
        rows[parts[1]] = (parts[2], c)
    return rows


def fixtures_in(folder):
    found = {}
    for root, _, names in os.walk(folder):
        for n in sorted(names):
            try:
                with open(os.path.join(root, n), "rb") as f:
                    data = f.read()
            except OSError:
                continue
            found.setdefault(sha(data), (n, data))
    return found


def scan(folder, out):
    found = fixtures_in(folder)
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_resolver(tmp)
        verdict = resolve_all(exe, [s for _, d in found.values() for s in refs_of(d)])
    lines = [HEAD]
    for h, (n, d) in sorted(found.items(), key=lambda kv: kv[1][0]):
        if not refs_of(d):
            continue
        lines.append("fixture %s %s %s\n" % (h, n.replace(" ", "_"), fmt(summarise(d, verdict))))
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("".join(lines))
    return len(lines) - 1


def problems(folder, expected_path=EXPECTED, exe=None):
    if not folder:
        return ["%s is not set: the fixture saves are needed to release a build (they stay outside the repository)" % ENV]
    if not os.path.isdir(folder):
        return ["%s=%s is not a folder" % (ENV, folder)]
    with open(expected_path, encoding="utf-8") as f:
        expected = parse_expected(f.read())
    if not expected:
        return ["%s lists no fixture" % os.path.basename(expected_path)]
    found = fixtures_in(folder)
    with tempfile.TemporaryDirectory() as tmp:
        exe = exe or build_resolver(tmp)
        verdict = resolve_all(exe, [s for _, d in found.values() for s in refs_of(d)])
    out = []
    for h, (name, want) in sorted(expected.items(), key=lambda kv: kv[1][0]):
        if h not in found:
            out.append("fixture %s (%s) is not in %s" % (h[:12], name, folder))
            continue
        got = summarise(found[h][1], verdict)
        if got != want:
            out.append("%s (%s): its pointers read %s, expected %s" % (name, h[:12], fmt(got), fmt(want)))
    for h, (n, d) in found.items():
        if h not in expected and refs_of(d):
            c = summarise(d, verdict)
            if c.get("UNKNOWN") or c.get("BAD"):
                out.append("%s (%s) is not in the expected file and holds pointers that do not read: %s" % (n, h[:12], fmt(c)))
    return out


def require(folder=None, expected_path=EXPECTED):
    """For the release path: SystemExit with the reason unless every fixture reads."""
    folder = os.environ.get(ENV) if folder is None else folder
    try:
        p = problems(folder, expected_path)
    except (GateError, OSError) as e:
        p = [str(e)]
    if p:
        raise SystemExit("The save fixture gate failed, so this build must not be published (owner rule: no build may "
                         "ever make an existing save unloadable; docs/r3d/fork-plan.md, XS.4):\n  - " + "\n  - ".join(p))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("folder")
    s.add_argument("--out", default=EXPECTED)
    sub.add_parser("check")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "scan":
            print("%d fixtures written to %s" % (scan(a.folder, a.out), a.out))
        else:
            require()
            print("save fixture gate: ok")
    except GateError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
