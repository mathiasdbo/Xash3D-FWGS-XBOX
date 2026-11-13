#!/usr/bin/env python3
"""XS.1 - the legacy callback table: what an `ofs:N` string in an old saved game means.

An old save holds each think/touch/use/blocked/move-done pointer as `ofs:N`, N = the function's offset from
GameDLLInit in the link that wrote it (docs/r3d/fork-plan.md, XS). Saves written by the three public builds all share
one layout, so one table serves them: tools/save-legacy-table.txt, generated once from the v0.2.0 build and then
frozen. It is a list of every server function game code can assign to a saved pointer, with its offset and the field(s)
it is stored in.

How the set of functions is found - from the compiled objects, not from the source text. A function can be assigned to
a saved pointer only if its address is taken, and in a COFF object taking an address is an absolute (DIR32)
relocation against the function's symbol. A virtual function's address appears in a vtable (.rdata) and is never what
SetThink and friends store. The field comes from the store itself: `mov [reg+disp], imm32` with the relocation as
imm32; disp 20, 24, 28, 32 and 204 are m_pfnThink, m_pfnTouch, m_pfnUse, m_pfnBlocked (CBaseEntity) and
m_pfnCallWhenMoveDone (CBaseToggle). A function is in the table if its address is stored into one of those (the field
is then written as seen, e.g. `think`), or, when the address went through a register and no field can be read, if it is
a non-virtual member function of one of the three shapes the fields hold (`~think,movedone` for void(), `~touch,blocked`
for void(CBaseEntity*), `~use` for the use signature): the pointers are typed, so nothing else can be assigned. Exports,
the dispatch functions, static initialisers and everything else whose address is merely taken are not in it, and an
offset that is not in it resolves to nothing. A function resolves only for a field it can hold.

Folding. lld folds functions with identical code, so one offset can be several functions. The generator lists every
such offset; each must be resolvable by field (the names at it store into pairwise different known fields) or
generation stops. The one that exists is -210368: CBaseMonster::CallMonsterThink (think) and CFuncPlat::CallHitBottom
(move-done).

Commands:
  generate <objdir> --map MAP --xbe XBE --out FILE   (the evidence stays outside the repository)
  check [FILE]                                       (the checked-in table is well formed and is the pinned one)
  resolve OFFSET [FIELD]                             (what an ofs: value means)
"""
import argparse
import collections
import glob
import hashlib
import itertools
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, "save-legacy-table.txt")
TABLE_SHA256 = "d67ce4129e15f58fa3dcca0779fc75657dca4c5064d64099d1f81b460aa4b7a2"  # of the checked-in table (LF)

# The one build the table is made from: its link map gives the offsets, its objects give the functions.
SOURCE_BUILD = "xbe-cdd64653"
SOURCE_XBE_SHA256 = "77f9f341ca43bb5bff58a045972b4b1dc24963f424621ebfba46be9b94854dcf"
SDK_COMMIT = "e958ca4eff31f06671eec81981e9ae2a8c43a992"  # deps/hlsdk at that build
FIELDS = {20: "think", 24: "touch", 28: "use", 32: "blocked", 204: "movedone"}
# The member-function signatures the five fields hold (void(), void(CBaseEntity*), use(...)), as they appear in a
# mangled name, and the fields each fits. A function that is neither stored into a field we can see nor a non-virtual
# member of one of these shapes cannot be a saved callback (SetThink and friends are typed), so it is not in the table.
SIGNATURES = {"XXZ": ("think", "movedone"), "XPAVCBaseEntity@@@Z": ("touch", "blocked"),
              "XPAVCBaseEntity@@0W4USE_TYPE@@M@Z": ("use",)}
MEMBER = re.compile(r"^\?[^?@]+@[^?]+@@[QAI]AE(?P<sig>.+)$")
DIR32 = 6
BLOCK = (0x27C000, 0x338000)  # the server code block of xbe-cdd64653
GAMEDLLINIT = "?GameDLLInit@@YAXXZ"
MAP_LINE = re.compile(r"\s*0001:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)")
GENERATED = re.compile(r"^\?\?__[EF]|^__GLOBAL__|^\?\?_[0-9C]")  # static-init/atexit stubs the compiler makes: no source name


class TableError(Exception):
    pass


def text_digest(text):
    return hashlib.sha256(text.replace("\r\n", "\n").encode("utf-8")).hexdigest()


# --- COFF objects -------------------------------------------------------------------------------------------

def parse_coff(data):
    """-> (sections, symbols); a symbol is None in the slot of an auxiliary record"""
    _mach, nsec, _ts, symptr, nsym, optsz, _fl = struct.unpack_from("<HHIIIHH", data, 0)
    secs = []
    for i in range(nsec):
        o = 20 + optsz + 40 * i
        name, _vs, _va, _rs, rptr, relptr, _ln, nrel, _nl, ch = struct.unpack_from("<8sIIIIIIHHI", data, o)
        secs.append({"name": name.rstrip(b"\0").decode("latin1"), "rptr": rptr, "relptr": relptr, "nrel": nrel, "ch": ch})
    strtab = symptr + 18 * nsym
    syms = []
    i = 0
    while i < nsym:
        n8, _val, secnum, typ, _sc, aux = struct.unpack_from("<8sIhHBB", data, symptr + 18 * i)
        if n8[:4] == b"\0\0\0\0":
            off = struct.unpack("<I", n8[4:])[0]
            name = data[strtab + off:data.index(b"\0", strtab + off)].decode("latin1")
        else:
            name = n8.rstrip(b"\0").decode("latin1")
        syms.append({"name": name, "sec": secnum, "typ": typ})
        syms.extend([None] * aux)
        i += 1 + aux
    return secs, syms


def address_taken(data):
    """-> [(symbol name, in_code, store_disp or None)] for every DIR32 relocation in the object"""
    secs, syms = parse_coff(data)
    out = []
    for sec in secs:
        code = bool(sec["ch"] & 0x20)
        if sec["name"].startswith(".rdata") or sec["name"].startswith(".debug") or sec["name"].startswith(".pdata"):
            continue  # vtables and constant tables: a virtual function's address, never a stored callback
        for r in range(sec["nrel"]):
            va, idx, typ = struct.unpack_from("<IIH", data, sec["relptr"] + 10 * r)
            if typ != DIR32 or idx >= len(syms) or syms[idx] is None:
                continue
            disp = None
            if code:
                at = sec["rptr"] + va
                c = data[at - 3:at]
                if len(c) == 3 and c[0] == 0xC7 and (c[1] & 0xC0) == 0x40:  # mov [reg+disp8], imm32
                    disp = c[2]
                else:
                    c = data[at - 6:at]
                    if len(c) == 6 and c[0] == 0xC7 and (c[1] & 0xC0) == 0x80:  # mov [reg+disp32], imm32
                        disp = struct.unpack("<I", c[2:6])[0]
            out.append((syms[idx]["name"], code, disp))
    return out


# --- link map -----------------------------------------------------------------------------------------------

def map_symbols(text):
    """-> {mangled name (the isolation prefix removed): address} for the code symbols of the map"""
    out = {}
    for line in text.splitlines():
        m = MAP_LINE.match(line)
        if m:
            name = m.group(1)
            out[name[3:] if name.startswith("sv_") else name] = int(m.group(2), 16)
    return out


# --- the table ----------------------------------------------------------------------------------------------

def build_table(uses, symbols, gameinit):
    """uses: {name: [(in_code, disp)]}; symbols: {name: address}. -> [(offset, fields, name)] sorted, or TableError"""
    rows = []
    problems = []
    for name, lst in uses.items():
        addr = symbols.get(name)
        if addr is None:
            continue  # a variable, a constant: not a function the map names
        stored = sorted({FIELDS.get(d, "disp%d" % d) for c, d in lst if c and d is not None})
        m = MEMBER.match(name)
        sig = SIGNATURES.get(m.group("sig")) if m else None
        if stored:
            if any(f.startswith("disp") for f in stored):
                problems.append("%s is stored at an unrecognised field offset %s" % (name, stored))
            elif sig is None or any(f not in sig for f in stored):
                problems.append("%s is stored into %s but is not a member function of that shape" % (name, stored))
            fields = ",".join(stored)  # seen: the field the store names
        elif sig is not None:
            fields = "~" + ",".join(sig)  # by shape only: a register carried the address
        else:
            continue  # not stored into a callback field and not shaped like a callback: an export, an initialiser, ...
        if not (BLOCK[0] <= addr < BLOCK[1]):
            problems.append("%s is at %#x, outside the server block" % (name, addr))
        rows.append((addr - gameinit, fields, name))
    by = collections.defaultdict(list)
    for off, fields, name in rows:
        by[off].append((fields, name))
    for off, group in by.items():
        if len(group) > 1:
            known = [f for f, _ in group]
            if any(f.startswith("~") or "," in f for f in known) or len(set(known)) != len(known):
                problems.append("fold set at %+d is not resolvable by field: %s" % (off, sorted(group)))
    if problems:
        raise TableError("; ".join(problems))
    return sorted(rows)


def make_text(rows, xbe_sha256):
    lines = ["# XS.1 legacy callback table: what an `ofs:N` in a saved game written by v0.1.2, v0.1.3 or v0.2.0 means.",
             "# Frozen. Generated by tools/save_legacy_table.py from the objects and link map of %s; never regenerate to fit a new build."
             % SOURCE_BUILD,
             "build %s %s" % (SOURCE_BUILD, xbe_sha256),
             "sdk %s" % SDK_COMMIT,
             "gamedllinit %s" % GAMEDLLINIT,
             "functions %d" % len(rows)]
    lines += ["fn %d %s %s" % r for r in rows]
    return "\n".join(lines) + "\n"


def parse_table(text):
    t = {"build": None, "sdk": None, "count": None, "rows": []}
    for n, line in enumerate(text.splitlines(), 1):
        f = line.split()
        if not f or f[0].startswith("#"):
            continue
        try:
            if f[0] == "build":
                t["build"] = (f[1], f[2])
            elif f[0] == "sdk":
                t["sdk"] = f[1]
            elif f[0] == "gamedllinit":
                pass
            elif f[0] == "functions":
                t["count"] = int(f[1])
            elif f[0] == "fn":
                t["rows"].append((int(f[1]), f[2], f[3]))
            else:
                raise TableError("line %d: unknown record %r" % (n, f[0]))
        except (IndexError, ValueError):
            raise TableError("line %d is malformed: %r" % (n, line))
    if not t["rows"] or t["build"] is None or t["count"] != len(t["rows"]):
        raise TableError("the table is empty, has no build line, or its count does not match")
    return t


def table_problems(text, expected_sha256=TABLE_SHA256):
    problems = []
    try:
        t = parse_table(text)
    except TableError as e:
        return [str(e)]
    if expected_sha256 is not None and text_digest(text) != expected_sha256:
        problems.append("the table is not the one the pinned digest names (%s, expected %s): it is frozen"
                        % (text_digest(text)[:12], expected_sha256[:12]))
    if t["build"] != (SOURCE_BUILD, SOURCE_XBE_SHA256) or t["sdk"] != SDK_COMMIT:
        problems.append("the table does not say it was made from %s" % SOURCE_BUILD)
    seen = {}
    for off, fields, name in t["rows"]:
        if name in seen:
            problems.append("%s is listed twice" % name)
        seen[name] = off
    by = collections.defaultdict(list)
    for off, fields, name in t["rows"]:
        by[off].append((fields, name))
        if fields.lstrip("~") == "" or any(f not in FIELDS.values() for f in fields.lstrip("~").split(",")):
            problems.append("%s has no valid field set: %r" % (name, fields))
    for off, group in by.items():
        if len(group) > 1:
            known = [f for f, _ in group]
            if any(f.startswith("~") or "," in f for f in known) or len(set(known)) != len(known):
                problems.append("offset %+d holds %d functions that cannot be told apart by field" % (off, len(group)))
    return problems


def resolve(table, offset, field=None):
    """The function names an offset means for a field (None: any); more than one only when the field was not given"""
    offset = offset - (1 << 32) if offset >= 1 << 31 else offset  # saves write the unsigned wrap of a negative offset
    hits = [(f, n) for o, f, n in table["rows"] if o == offset]
    if field is not None:
        hits = [(f, n) for f, n in hits if field in f.lstrip("~").split(",")]  # only a field the function can hold
    return sorted(n for f, n in hits)


def load_table(path=TABLE):
    with open(path, encoding="utf-8") as f:
        return parse_table(f.read())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("objdir")
    g.add_argument("--map", required=True)
    g.add_argument("--xbe", required=True)
    g.add_argument("--out", required=True)
    c = sub.add_parser("check")
    c.add_argument("file", nargs="?", default=TABLE)
    r = sub.add_parser("resolve")
    r.add_argument("offset", type=int)
    r.add_argument("field", nargs="?")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "generate":
            with open(a.xbe, "rb") as f:
                sha = hashlib.sha256(f.read()).hexdigest()
            if sha != SOURCE_XBE_SHA256:
                raise TableError("the table is made from the %s XBE (sha256 %s); this one is %s"
                                 % (SOURCE_BUILD, SOURCE_XBE_SHA256[:12], sha[:12]))
            with open(a.map, encoding="utf-8", errors="replace") as f:
                symbols = map_symbols(f.read())
            if GAMEDLLINIT not in symbols:
                raise TableError("the map has no %s" % GAMEDLLINIT)
            uses = collections.defaultdict(list)
            paths = sorted(glob.glob(os.path.join(a.objdir, "*.obj")))
            if not paths:
                raise TableError("no objects in %s" % a.objdir)
            for p in paths:
                with open(p, "rb") as f:
                    for name, code, disp in address_taken(f.read()):
                        uses[name].append((code, disp))
            rows = build_table(uses, symbols, symbols[GAMEDLLINIT])
            text = make_text(rows, sha)
            with open(a.out, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            nfold = sum(1 for _, g2 in itertools.groupby(rows, key=lambda r2: r2[0]) if len(list(g2)) > 1)
            print("%d objects, %d functions, %d fold sets; wrote %s\nTABLE_SHA256 for it is %s"
                  % (len(paths), len(rows), nfold, a.out, text_digest(text)))
        elif a.cmd == "check":
            with open(a.file, encoding="utf-8") as f:
                problems = table_problems(f.read(), TABLE_SHA256 if os.path.abspath(a.file) == TABLE else None)
            for p in problems:
                print("  " + p)
            print("the legacy table is " + ("NOT sound" if problems else "sound"))
            return 1 if problems else 0
        else:
            t = load_table()
            names = resolve(t, a.offset, a.field)
            print("\n".join(names) if names else "no function at that offset")
            return 0 if names else 1
    except (TableError, OSError) as e:
        print("save_legacy_table: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
