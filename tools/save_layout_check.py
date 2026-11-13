#!/usr/bin/env python3
"""Does this XBE keep the save layout of the builds already in players' hands? (docs/r3d/fork-plan.md, XS.0)

    python tools/save_layout_check.py check <default.xbe>
    python tools/save_layout_check.py baseline <default.xbe> --map <default.map> --label xbe-cdd64653 \
        --out tools/save-layout-baseline.txt

Why it exists. The Xbox build writes a saved function pointer (a think, touch, use, blocked or move-done callback)
as `ofs:N`, N being the byte offset of the function from `GameDLLInit` (engine/common/lib_common.c). A save is
therefore valid for a build only if the server code keeps every offset the save holds. The three public builds
(v0.1.2, v0.1.3, v0.2.0) moved their server code as one block, so one set of offsets serves all of them; this
checks that a new XBE has not broken that.

What it checks, from the XBE alone (no link map is needed):

  * the server module's export table (entity name -> function, 251 entries) keeps every baseline name, and every
    one at the same distance from the first export;
  * the SDK's dispatch table (`gFunctionTable`: GameDLLInit, DispatchSpawn, DispatchThink, ... 50 code pointers
    in a row) is where the baseline says, relative to the same anchor, entry by entry. Its first word is
    GameDLLInit itself, the point every saved offset is measured from.

Extra exports are fine (a new entity appends to the table). Anything else fails, and a failure means: do not
publish this XBE until XS.2 and XS.3 (names in saves, and the old offsets read through a table) are in.

The check cannot see a saved target that is neither exported nor in the dispatch table. XS.1 (the legacy table,
checked against the link map) closes that; until then this is the guard against the one thing known to move
offsets, which is the server block itself moving or changing size.
"""

import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, "save-layout-baseline.txt")

# An entity every Half-Life server module exports: the table that holds it is the server's export table.
MARKER = "worldspawn"
MIN_EXPORTS = 40  # a run of (name, function) pairs shorter than this is not the table
DISPATCH_MIN = 20  # the dispatch table is at least this many code pointers in a row


class XbeError(Exception):
    pass


class Xbe:
    """The parts of an XBE this check reads: its sections, and the bytes at a virtual address."""

    def __init__(self, data):
        if len(data) < 0x200 or data[:4] != b"XBEH":
            raise XbeError("not an XBE (no XBEH header)")
        self.data = data
        base, self.header_size, self.image_size = struct.unpack_from("<III", data, 0x104)
        count, sh_addr = struct.unpack_from("<II", data, 0x11C)
        if not 0 < count < 256:
            raise XbeError("an XBE with %d sections" % count)
        self.sections = []
        for i in range(count):
            o = sh_addr - base + i * 56
            if o < 0 or o + 24 > len(data):
                raise XbeError("section header %d is outside the file" % i)
            flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<IIIIII", data, o)
            if raw + rsize > len(data):
                raise XbeError("section %d runs past the end of the file" % i)
            n = name_va - base
            name = data[n:data.index(b"\0", n)].decode("latin-1") if 0 <= n < len(data) else "?"
            self.sections.append({"name": name, "flags": flags, "va": va, "vsize": vsize, "raw": raw, "rsize": rsize})
        self.code = [(s["va"], s["va"] + s["vsize"]) for s in self.sections if s["flags"] & 4]
        if not self.code:
            raise XbeError("no executable section")

    def is_code(self, va):
        return any(a <= va < b for a, b in self.code)

    def offset(self, va):
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["rsize"]:
                return s["raw"] + va - s["va"]
        return None

    def cstring(self, va):
        o = self.offset(va)
        if o is None:
            return None
        end = self.data.find(b"\0", o, o + 200)
        if end <= o:
            return None
        s = self.data[o:end]
        if not all(33 <= c < 127 for c in s):
            return None
        return s.decode("ascii")

    def words(self, section):
        """(first virtual address, the 32-bit words) of a data section"""
        n = section["rsize"] // 4
        raw = self.data[section["raw"]:section["raw"] + n * 4]
        return section["va"], struct.unpack("<%dI" % n, raw)

    def data_sections(self):
        return [s for s in self.sections if not s["flags"] & 4 and s["rsize"] >= 8]


def export_tables(xbe):
    """Runs of (name pointer, function pointer) pairs: {name: address} for each run of MIN_EXPORTS or more."""
    out = []
    for s in xbe.data_sections():
        va0, w = xbe.words(s)
        i = 0
        while i + 1 < len(w):
            run = {}
            j = i
            while j + 1 < len(w) and xbe.is_code(w[j + 1]):
                name = xbe.cstring(w[j])
                if not name:
                    break
                run[name] = w[j + 1]
                j += 2
            if len(run) >= MIN_EXPORTS:
                out.append(run)
                i = j
            else:
                i += 1
    return out


def server_exports(xbe):
    for run in export_tables(xbe):
        if MARKER in run:
            return run
    raise XbeError("no server export table found (no table of entity names holds %r)" % MARKER)


def dispatch_table(xbe, anchor_va, offsets):
    """Where the baseline's dispatch table sits in this XBE: the data address of its first word, or None."""
    first = anchor_va + offsets[0]
    for s in xbe.data_sections():
        va0, w = xbe.words(s)
        for i, v in enumerate(w):
            if v == first and i + len(offsets) <= len(w):
                if all(w[i + k] == anchor_va + offsets[k] for k in range(len(offsets))):
                    return va0 + 4 * i
    return None


def find_dispatch_table(xbe, gameinit_va):
    """For making a baseline: the data address of the word that holds GameDLLInit, and the run of code pointers that
    starts there (the SDK's gFunctionTable and what follows it). GameDLLInit's address comes from a link map."""
    hits = []
    for s in xbe.data_sections():
        va0, w = xbe.words(s)
        for i, v in enumerate(w):
            if v == gameinit_va:
                j = i
                while j < len(w) and xbe.is_code(w[j]):
                    j += 1
                hits.append(list(w[i:j]))
    if len(hits) != 1:
        raise XbeError("GameDLLInit (%#x) is held by %d data words, expected exactly one" % (gameinit_va, len(hits)))
    if len(hits[0]) < DISPATCH_MIN:
        raise XbeError("only %d code pointers follow GameDLLInit" % len(hits[0]))
    return hits[0]


def gameinit_from_map(text):
    """GameDLLInit's address in a link map (lld's -map output)."""
    found = {}
    for line in text.splitlines():
        f = line.split()
        if len(f) >= 3 and "GameDLLInit" in f[1] and f[0].startswith("0001:"):
            found[f[1]] = int(f[2], 16)
    server = [a for n, a in found.items() if n.startswith("sv_") or len(found) == 1]
    if len(set(server)) != 1:
        raise XbeError("the map holds %d GameDLLInit symbols: %s" % (len(found), sorted(found)))
    return server[0]


def make_baseline(xbe, label, sha256, gameinit_va):
    exports = server_exports(xbe)
    anchor = min(exports, key=lambda n: (exports[n], n))
    anchor_va = exports[anchor]
    offsets = [v - anchor_va for v in find_dispatch_table(xbe, gameinit_va)]
    lines = [
        "# Save layout baseline (docs/r3d/fork-plan.md, XS.0). Written by tools/save_layout_check.py baseline.",
        "# Every offset is in bytes from the anchor (the lowest-addressed server export).",
        "build %s %s" % (label, sha256),
        "anchor %s" % anchor,
        "gamedllinit %d" % (gameinit_va - anchor_va),
        "dispatch " + " ".join(str(o) for o in offsets),
    ]
    for n in sorted(exports, key=lambda n: (exports[n], n)):
        lines.append("export %s %d" % (n, exports[n] - anchor_va))
    return "\n".join(lines) + "\n"


def parse_baseline(text):
    b = {"build": None, "anchor": None, "gamedllinit": None, "dispatch": None, "exports": {}}
    for ln, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = line.split()
        try:
            if f[0] == "build":
                b["build"] = (f[1], f[2])
            elif f[0] == "anchor":
                b["anchor"] = f[1]
            elif f[0] == "gamedllinit":
                b["gamedllinit"] = int(f[1])
            elif f[0] == "dispatch":
                b["dispatch"] = [int(x) for x in f[1:]]
            elif f[0] == "export":
                b["exports"][f[1]] = int(f[2])
            else:
                raise ValueError(f[0])
        except (IndexError, ValueError):
            raise XbeError("baseline line %d is not understood: %r" % (ln, line))
    if not (b["build"] and b["anchor"] and b["dispatch"] and b["exports"] and b["gamedllinit"] is not None):
        raise XbeError("the baseline is missing its build, anchor, gamedllinit, dispatch or exports")
    if b["dispatch"][0] != b["gamedllinit"]:
        raise XbeError("the baseline's dispatch table must start at GameDLLInit")
    if b["anchor"] not in b["exports"] or b["exports"][b["anchor"]] != 0:
        raise XbeError("the baseline's anchor must be an export at offset 0")
    if len(b["dispatch"]) < DISPATCH_MIN:
        raise XbeError("the baseline's dispatch table has %d entries" % len(b["dispatch"]))
    return b


def check(xbe, baseline):
    """A list of what differs from the baseline; empty means the layout is the one saves were written against."""
    problems = []
    try:
        exports = server_exports(xbe)
    except XbeError as e:
        return [str(e)]
    anchor = baseline["anchor"]
    if anchor not in exports:
        return ["the anchor export %r is gone, so nothing can be compared" % anchor]
    anchor_va = exports[anchor]
    missing = [n for n in baseline["exports"] if n not in exports]
    if missing:
        problems.append("%d exported server function(s) are gone: %s" % (len(missing), ", ".join(sorted(missing)[:8])))
    moved = [(n, exports[n] - anchor_va - off) for n, off in baseline["exports"].items()
             if n in exports and exports[n] - anchor_va != off]
    if moved:
        moved.sort(key=lambda m: baseline["exports"][m[0]])
        problems.append("%d exported server function(s) moved relative to %s: %s" % (
            len(moved), anchor, ", ".join("%s %+d" % m for m in moved[:8])))
    if dispatch_table(xbe, anchor_va, baseline["dispatch"]) is None:
        problems.append("the SDK's dispatch table (GameDLLInit first) is not where the baseline has it, relative to %s"
                        % anchor)
    return problems


def check_file(path, baseline_path=BASELINE):
    with open(baseline_path, encoding="utf-8") as f:
        baseline = parse_baseline(f.read())
    with open(path, "rb") as f:
        return check(Xbe(f.read()), baseline), baseline


def require(data, baseline_path=BASELINE):
    """For the release path: raise SystemExit with the reason unless the XBE keeps the saves' layout."""
    try:
        with open(baseline_path, encoding="utf-8") as f:
            baseline = parse_baseline(f.read())
        problems = check(Xbe(data), baseline)
    except (XbeError, OSError) as e:
        problems = [str(e)]
    if problems:
        raise SystemExit(
            "This XBE does not keep the layout players' saves were written against, so it must not be published "
            "(owner rule: no build may ever make an existing save unloadable; docs/r3d/fork-plan.md, XS):\n  - "
            + "\n  - ".join(problems))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("xbe")
    c.add_argument("--baseline", default=BASELINE)
    b = sub.add_parser("baseline")
    b.add_argument("xbe")
    b.add_argument("--label", required=True, help="the release this XBE is, e.g. xbe-cdd64653")
    b.add_argument("--map", required=True, help="that build's link map (it names GameDLLInit's address)")
    b.add_argument("--out", required=True)
    a = ap.parse_args()
    try:
        if a.cmd == "check":
            problems, base = check_file(a.xbe, a.baseline)
            if problems:
                print("FAIL %s against %s:" % (a.xbe, base["build"][0]))
                for p in problems:
                    print("  - " + p)
                return 1
            print("ok   %s keeps the save layout of %s (%d exports, %d dispatch entries)"
                  % (a.xbe, base["build"][0], len(base["exports"]), len(base["dispatch"])))
            return 0
        with open(a.xbe, "rb") as f:
            raw = f.read()
        with open(a.map, encoding="utf-8", errors="replace") as f:
            gameinit = gameinit_from_map(f.read())
        text = make_baseline(Xbe(raw), a.label, hashlib.sha256(raw).hexdigest(), gameinit)
        # the baseline must pass against the very XBE it came from, or it is wrong
        problems = check(Xbe(raw), parse_baseline(text))
        if problems:
            print("the baseline does not pass against its own XBE: %s" % problems)
            return 1
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote %s" % a.out)
        return 0
    except XbeError as e:
        print("error: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
