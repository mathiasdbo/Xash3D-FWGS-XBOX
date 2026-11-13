#!/usr/bin/env python3
"""Does this XBE keep the save layout of the builds already in players' hands? (docs/r3d/fork-plan.md, XS.0)

    python tools/save_layout_check.py check <default.xbe>
    python tools/save_layout_check.py baseline <default.xbe> --map <default.map> --label xbe-cdd64653 \
        --also-xbe <older default.xbe> --also-xbe <older default.xbe> --out tools/save-layout-baseline.txt

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
    GameDLLInit itself, the point every saved offset is measured from;
  * the code at the offset of every server function (about 3,000, from the link map the baseline was made with):
    its first 32 bytes, with the bytes that differed between the three public builds masked, because those hold
    addresses that move with the rest of the image. A saved callback that is neither exported nor in the dispatch
    table is one of these, so a function that moved, changed or swapped places with another shows here.

Extra exports are fine (a new entity appends to the table). Anything else fails, and a failure means: do not
publish this XBE until XS.2 and XS.3 (names in saves, and the old offsets read through a table) are in.

Where the policy comes from. The baseline is made from the three public builds only (their SHA-256s are pinned in
PUBLIC_BUILDS; `baseline` refuses any other XBE), its own digest is pinned in BASELINE_SHA256, and the release path
checks both, so regenerating the file against a candidate, or editing it, fails until someone changes the pinned
values in this file too: a deliberate act that shows in a review, not an accident. (Anyone who can edit this file can
edit anything; the check against saves that really load, XS.4, is the independent one.) The functions the baseline
leaves out (fewer than 8 stable bytes, or no code at that offset) are listed in it as `skip` lines, counted, and a test
checks that none is ever assigned as a think, touch, use, blocked or move-done callback in the SDK.

What it still cannot see: two functions with identical beginnings trading places, and a function the link map
did not name. XS.1 (the legacy table: every callback a save can hold, checked against the map) closes the gap;
until then this is the guard against the thing known to move offsets, the server code moving or changing size.
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
# The builds the saves in the wild were written by, and the file that says what they have in common. A baseline is
# made from exactly these; changing any of this is the act of accepting a new layout (docs/r3d/fork-plan.md, XS).
PUBLIC_BUILDS = {
    "xbe-88862388": "24694acc4c17a2e22ad1275be859213c38253f5279d6e17595c8fbdc9c66d564",  # v0.1.2
    "xbe-1f4eedc1": "ab8131b09c9013d00b5c970c591dd1290704d87f55a617f6ed3e44d8395041bd",  # v0.1.3
    "xbe-cdd64653": "77f9f341ca43bb5bff58a045972b4b1dc24963f424621ebfba46be9b94854dcf",  # v0.2.0
}
BASELINE_BUILD = "xbe-cdd64653"  # the build the baseline's exports, dispatch table and map symbols come from
BASELINE_SHA256 = "5a89dc4b4303470504baec6bd66f1bd419a8250022de9e909bb9e3811127de55"  # of the baseline file (LF)
WINDOW = 32  # bytes of each server function that are compared
MIN_STABLE = 8  # a function with fewer unmasked bytes than this is left out: it would prove nothing


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


def server_symbols_from_map(text):
    """{address: first symbol} of the server module's functions in a link map (the `sv_` symbols of the code section)."""
    out = {}
    for line in text.splitlines():
        f = line.split()
        if len(f) >= 3 and f[0].startswith("0001:") and f[1].startswith("sv_"):
            out.setdefault(int(f[2], 16), f[1])
    return out


def window(xbe, va):
    o = xbe.offset(va)
    if o is None or o + WINDOW > len(xbe.data):
        return None
    return xbe.data[o:o + WINDOW]


def derive_functions(builds, offsets_names):
    """builds: [(Xbe, anchor address)], at least two, each a build the saves were written against. offsets_names:
    [(offset from the anchor, name)]. A function's pattern is its first WINDOW bytes with the positions that differ
    between the builds masked ('??'); it is kept when MIN_STABLE bytes or more remain."""
    if len(builds) < 2:
        raise XbeError("masks come from the differences between builds: give at least two")
    out = []
    for off, name in offsets_names:
        ws = [window(x, a + off) for x, a in builds]
        if None in ws:
            continue
        stable = [all(w[i] == ws[0][i] for w in ws) for i in range(WINDOW)]
        if sum(stable) >= MIN_STABLE:
            out.append((off, name, "".join("%02x" % ws[0][i] if stable[i] else "??" for i in range(WINDOW))))
    return out


def skipped_functions(offsets_names, kept):
    """the (offset, name) pairs derive_functions left out"""
    have = {off for off, _, _ in kept}
    return [(off, name) for off, name in offsets_names if off not in have]


def text_digest(text):
    """SHA-256 of a baseline's text with line endings normalised, so a CRLF checkout hashes as the committed LF file"""
    return hashlib.sha256(text.replace("\r\n", "\n").encode("utf-8")).hexdigest()


def pattern_matches(pattern, data):
    return len(data) == WINDOW and all(
        pattern[2 * i:2 * i + 2] == "??" or data[i] == int(pattern[2 * i:2 * i + 2], 16) for i in range(WINDOW))


def make_baseline(xbe, label, sha256, gameinit_va, functions=(), skipped=(), masks=()):
    exports = server_exports(xbe)
    anchor = min(exports, key=lambda n: (exports[n], n))
    anchor_va = exports[anchor]
    offsets = [v - anchor_va for v in find_dispatch_table(xbe, gameinit_va)]
    lines = [
        "# Save layout baseline (docs/r3d/fork-plan.md, XS.0). Written by tools/save_layout_check.py baseline.",
        "# Every offset is in bytes from the anchor (the lowest-addressed server export).",
        "build %s %s" % (label, sha256),
    ] + ["masks %s %s" % m for m in masks] + [
        "anchor %s" % anchor,
        "gamedllinit %d" % (gameinit_va - anchor_va),
        "dispatch " + " ".join(str(o) for o in offsets),
    ]
    for n in sorted(exports, key=lambda n: (exports[n], n)):
        lines.append("export %s %d" % (n, exports[n] - anchor_va))
    if functions or skipped:
        lines.append("symbols %d" % (len(functions) + len(skipped)))
    for off, name, pattern in sorted(functions):
        lines.append("fn %d %s %s" % (off, pattern, name))
    for off, name in sorted(skipped):
        lines.append("skip %d %s" % (off, name))
    return "\n".join(lines) + "\n"


def _is_hex(x):
    return len(x) == 2 and all(c in "0123456789abcdef" for c in x)


def parse_baseline(text):
    b = {"build": None, "masks": [], "anchor": None, "gamedllinit": None, "dispatch": None, "exports": {},
         "functions": [], "skipped": [], "symbols": None}
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
            elif f[0] == "masks":
                b["masks"].append((f[1], f[2]))
            elif f[0] == "symbols":
                b["symbols"] = int(f[1])
            elif f[0] == "skip":
                b["skipped"].append((int(f[1]), f[2]))
            elif f[0] == "fn":
                pat = f[2]
                if len(pat) != 2 * WINDOW or any(pat[i:i + 2] != "??" and not _is_hex(pat[i:i + 2])
                                                 for i in range(0, 2 * WINDOW, 2)):
                    raise ValueError("pattern")
                b["functions"].append((int(f[1]), pat, f[3] if len(f) > 3 else "?"))
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
    if b["functions"] or b["skipped"]:
        if b["symbols"] != len(b["functions"]) + len(b["skipped"]):
            raise XbeError("the baseline says %s server functions but lists %d checked and %d skipped"
                           % (b["symbols"], len(b["functions"]), len(b["skipped"])))
    return b


def provenance_problems(baseline, text, expected_sha256=None):
    """What is wrong with where a baseline came from: it must be the committed one (its digest is pinned) and made from
    the public builds (their hashes are pinned)."""
    problems = []
    if expected_sha256 is not None and text_digest(text) != expected_sha256:
        problems.append("the baseline file is not the one the pinned digest names (%s, expected %s): it was edited or "
                        "regenerated; accepting a new layout means changing BASELINE_SHA256 in save_layout_check.py "
                        "too, on purpose" % (text_digest(text)[:12], expected_sha256[:12]))
    label, sha = baseline["build"]
    if PUBLIC_BUILDS.get(label) != sha:
        problems.append("the baseline says it was made from %s %s, which is not a pinned public build" % (label, sha[:12]))
    elif label != BASELINE_BUILD:
        problems.append("the baseline was made from %s, not %s" % (label, BASELINE_BUILD))
    others = sorted(v for k, v in PUBLIC_BUILDS.items() if k != BASELINE_BUILD)
    masks = sorted(sha for _, sha in baseline["masks"])
    if masks != others:
        problems.append("the baseline's function masks were not made from the other public builds")
    return problems


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
    differ = [(off, name) for off, pattern, name in baseline["functions"]
              if not pattern_matches(pattern, window(xbe, anchor_va + off) or b"")]
    if differ:
        problems.append("%d of %d server function(s) do not start with the baseline's bytes at their offset from %s: %s"
                        % (len(differ), len(baseline["functions"]), anchor,
                           ", ".join("%+d %s" % (o, n) for o, n in differ[:6])))
    return problems


def check_file(path, baseline_path=BASELINE):
    with open(baseline_path, encoding="utf-8") as f:
        text = f.read()
    baseline = parse_baseline(text)
    problems = []
    if baseline_path == BASELINE:
        problems += provenance_problems(baseline, text, BASELINE_SHA256)
    with open(path, "rb") as f:
        return problems + check(Xbe(f.read()), baseline), baseline


def require(data, baseline_path=BASELINE, expected_sha256=BASELINE_SHA256):
    """For the release path: raise SystemExit with the reason unless the XBE keeps the saves' layout. The baseline must
    be the committed one made from the public builds; expected_sha256=None skips that, for tests only."""
    try:
        with open(baseline_path, encoding="utf-8") as f:
            text = f.read()
        baseline = parse_baseline(text)
        problems = []
        if expected_sha256 is not None:
            problems += provenance_problems(baseline, text, expected_sha256)
        problems += check(Xbe(data), baseline)
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
    b.add_argument("--map", required=True,
                   help="that build's link map (it names GameDLLInit and every server function)")
    b.add_argument("--also-xbe", action="append", default=[], required=True,
                   help="another build the saves were written against (repeat it; the public builds are three): "
                        "the bytes that differ between them are masked")
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
        with open(a.map, encoding="utf-8", errors="replace") as f:
            symbols = server_symbols_from_map(f.read())
        main_sha = hashlib.sha256(raw).hexdigest()
        if a.label != BASELINE_BUILD or PUBLIC_BUILDS.get(a.label) != main_sha:
            raise XbeError("a baseline is made from the public %s XBE (sha256 %s) and no other; this one is %s"
                           % (BASELINE_BUILD, PUBLIC_BUILDS[BASELINE_BUILD][:12], main_sha[:12]))
        main_xbe = Xbe(raw)
        main_anchor = min(server_exports(main_xbe).values())
        builds = [(main_xbe, main_anchor)]
        masks = []
        for path in a.also_xbe:
            with open(path, "rb") as f:
                other_raw = f.read()
            masks.append(("", hashlib.sha256(other_raw).hexdigest()))
            builds.append((Xbe(other_raw), min(server_exports(Xbe(other_raw)).values())))
        want = sorted(v for k, v in PUBLIC_BUILDS.items() if k != BASELINE_BUILD)
        if sorted(m[1] for m in masks) != want:
            raise XbeError("the masks must come from the other two public builds (sha256 %s)"
                           % ", ".join(w[:12] for w in want))
        masks = [({v: k for k, v in PUBLIC_BUILDS.items()}[m[1]], m[1]) for m in masks]
        offsets_names = [(va - main_anchor, nm) for va, nm in sorted(symbols.items())]
        functions = derive_functions(builds, offsets_names)
        skipped = skipped_functions(offsets_names, functions)
        print("%d server functions: %d checked, %d skipped" % (len(offsets_names), len(functions), len(skipped)))
        for off, nm in skipped:
            print("  skipped %+d %s" % (off, nm))
        text = make_baseline(main_xbe, a.label, main_sha, gameinit, functions, skipped, masks)
        # the baseline must pass against the very XBE it came from, or it is wrong
        problems = check(Xbe(raw), parse_baseline(text))
        if problems:
            print("the baseline does not pass against its own XBE: %s" % problems)
            return 1
        with open(a.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote %s; BASELINE_SHA256 for it is %s" % (a.out, text_digest(text)))
        return 0
    except XbeError as e:
        print("error: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
