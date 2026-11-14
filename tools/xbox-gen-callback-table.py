#!/usr/bin/env python3
"""XS.2 - the names saved games hold instead of `ofs:N`.

A saved think/touch/use/blocked/move-done pointer used to be written as its offset from GameDLLInit, which is only
true for the link that wrote it. This tool lists, from the isolated server objects of the build being linked, every
function game code can store in one of those fields (the same rule tools/save_legacy_table.py froze for the saves of
the three public builds: a store into a callback field that can be read from the code, or a non-virtual member
function of a callback's shape), and writes them as extra lines for the server's export table. The engine then
writes a saved pointer as the function's platform-neutral name (`Think@CFoo`: the MSVC mangling with its leading `?`
and everything from `@@` removed, which is what COM_GetPlatformNeutralName turns a mangled name into on read) and
reads it back by name, so a save no longer depends on the link at all. XS.1's table names a function by its full
mangled name; its neutral name is `neutral()` of that, which is how XS.3 gets from an old `ofs:` to a current address.

  gen   OBJDIR --exports IN --out OUT     copy IN (the server's exports.txt) to OUT with one line per callback
  check MAP OUT                           after the link: every callback name has its own address in the map

The link must not fold identical functions (`-opt:noicf`); `check` is the gate that says it did not.
"""
import argparse
import collections
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_legacy_table as slt  # noqa: E402

PREFIX = "sv_"  # tools/xbox-game-isolate.sh renames the server's own symbols with it
MIN_CALLBACKS = 150  # the v0.2.0 build has 230
MAP_LINE = re.compile(r"\s*0001:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})")


class GenError(Exception):
    pass


def neutral(mangled):
    """`?Think@CFoo@@QAEXXZ` -> `Think@CFoo`, as COM_GetMSVCName does (engine/common/lib_common.c)"""
    i = mangled.find("@@")
    if not mangled.startswith("?") or i < 2:
        raise GenError("not an MSVC mangled name: %s" % mangled)
    return mangled[1:i]


def defined_symbols(data):
    _secs, syms = slt.parse_coff(data)
    return {s["name"] for s in syms if s and s["sec"] > 0 and (s["typ"] >> 4) == 2}  # defined, and a function


def required_names():
    """The neutral names of every function XS.1's frozen table lists: whatever saves already hold must stay nameable,
    so a build that drops one (a classifier regression, an SDK change) is refused, not quietly short."""
    return {neutral(n) for _off, _f, n in slt.load_table()["rows"]}


def missing_required(cbs, required):
    gone = sorted(set(required) - set(cbs))
    return ["%d of the %d callbacks XS.1's table lists are not in this build's table (%s)"
            % (len(gone), len(required), ", ".join(gone[:4]) + (" ..." if len(gone) > 4 else ""))] if gone else []


def callbacks(objdir, required=()):
    """-> {callback name: linker symbol} from the objects of the directory; `required` names must all be among them"""
    paths = sorted(glob.glob(os.path.join(objdir, "*.obj")))
    if not paths:
        raise GenError("no objects in %s" % objdir)
    uses = collections.defaultdict(list)
    defined = set()
    for p in paths:
        with open(p, "rb") as f:
            data = f.read()
        defined |= defined_symbols(data)
        for name, code, disp in slt.address_taken(data):
            uses[name].append((code, disp))
    out = {}
    problems = []
    for sym, lst in uses.items():
        if sym not in defined:
            continue  # an import, a constant, a variable: not a function this module defines
        mangled = sym[len(PREFIX):] if sym.startswith(PREFIX) else sym
        fields, problem = slt.classify(mangled, lst)
        if problem:
            problems.append(problem)
            continue
        if fields is None:
            continue
        name = neutral(mangled)
        if name in out and out[name] != sym:
            problems.append("%s and %s have the same neutral name %s" % (out[name], sym, name))
        out[name] = sym
    if problems:
        raise GenError("; ".join(problems))
    problems = missing_required(out, required)
    if problems:
        raise GenError(problems[0])
    if len(out) < MIN_CALLBACKS:
        raise GenError("only %d callbacks found in %d objects (expected at least %d): an incomplete object set"
                       % (len(out), len(paths), MIN_CALLBACKS))
    return out


def write_exports(base_text, cbs):
    """the server's export list plus one `name asm_symbol` line per callback"""
    names = {l.split()[0] for l in base_text.splitlines() if l.strip()}
    clash = sorted(set(cbs) & names)
    if clash:
        raise GenError("a callback has the name of an export: %s" % ", ".join(clash[:3]))
    text = base_text if base_text.endswith("\n") or not base_text else base_text + "\n"
    return text + "".join("%s %s\n" % (n, cbs[n]) for n in sorted(cbs))


def map_addresses(map_text):
    out = collections.defaultdict(list)
    for line in map_text.splitlines():
        m = MAP_LINE.match(line)
        if m:
            out[m.group(1)].append(int(m.group(2), 16))
    return out


def check_map(map_text, cbs):
    """every callback has an address in the map, and no two callbacks share one"""
    addrs = map_addresses(map_text)
    problems = []
    owner = {}
    for name, sym in sorted(cbs.items()):
        got = addrs.get(sym)
        if not got:
            problems.append("%s is not in the link map" % sym)
            continue
        if len(set(got)) != 1:
            problems.append("%s has several addresses" % sym)
        a = got[0]
        if a in owner:
            problems.append("%s and %s share address %#x: the link folded them" % (owner[a], name, a))
        owner[a] = name
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen")
    g.add_argument("objdir")
    g.add_argument("--exports", required=True)
    g.add_argument("--out", required=True)
    c = sub.add_parser("check")
    c.add_argument("map")
    c.add_argument("exports")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "gen":
            cbs = callbacks(a.objdir, required_names())
            with open(a.exports, encoding="utf-8") as f:
                text = write_exports(f.read(), cbs)
            with open(a.out, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            print("xbox-gen-callback-table: %d callbacks written to %s" % (len(cbs), a.out))
        else:
            with open(a.exports, encoding="utf-8") as f:
                cbs = {l.split()[0]: l.split()[1] for l in f if len(l.split()) == 2 and "@" in l.split()[0]
                       and l.split()[1].startswith(PREFIX + "?")}
            with open(a.map, encoding="utf-8", errors="replace") as f:
                problems = check_map(f.read(), cbs)
            if len(cbs) < MIN_CALLBACKS:
                problems.append("only %d callbacks in %s" % (len(cbs), a.exports))
            problems += missing_required(cbs, required_names())
            for p in problems:
                print("  " + p, file=sys.stderr)
            print("xbox-gen-callback-table: %d callbacks, %s" % (len(cbs), "NOT sound" if problems else "each at its own address"))
            return 1 if problems else 0
    except (GenError, slt.TableError, OSError) as e:
        print("xbox-gen-callback-table: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
