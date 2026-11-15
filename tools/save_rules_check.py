#!/usr/bin/env python3
"""The rules a saved game depends on that no fixture exercises (docs/r3d/fork-plan.md, XS.5).

    python tools/save_rules_check.py source          # the engine source keeps the save format and the limits
    python tools/save_rules_check.py xbe <default.xbe>   # the XBE keeps the title id the saves live under

A saved game is found by where it lives and read by what the engine accepts, so a build that changes either makes every
existing save vanish or fail without a single save file being touched. The rules, each of them checked here:

  * the title id. Saves are in E:\\UDATA\\<title id>\\valve\\valve\\save (nxdk's cxbe gives every build 0xFFFF0002), so an XBE
    with another id would not see one of them. `xbe` reads it from the certificate; make_payload.py refuses another id.
  * the save format. engine/server/sv_save.c's SAVEGAME_HEADER ("JSAV"), SAVEGAME_VERSION (0x71) and
    CLIENT_SAVEGAME_VERSION (0x67): a newer number makes the engine call every older save "<old version>".
  * the limits. A save holds as many entities, models, sounds, light styles and decals as the game had, so the engine must
    accept at least that many: MINIMUMS below lists them (the values of the build with XASH_LOW_MEMORY=1,
    which is what the Xbox build uses). Raising a limit is fine; lowering one fails.

`source` reads the macros out of the headers (it follows the `#if XASH_LOW_MEMORY` branches and takes the Xbox one), so it needs no
compiler. A rule whose macro cannot be found fails, so a rename is noticed instead of passing silently.

Not here: the baker renumbering brush models or entities against the retail .bsp, the SDK's saved-field tables, and a full
disk, which need measurements of their own (see XS.5 in the plan).
"""

import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SV_SAVE = os.path.join(REPO, "engine", "server", "sv_save.c")
TITLE_ID = 0xFFFF0002
XASH_LOW_MEMORY = 1  # the Xbox build (tools/xbox-cc.sh)

# The lowest value each engine limit may have: what the public builds (v0.1.2 to v0.2.0, XASH_LOW_MEMORY=1) had. A saved game
# holds as many entities, models, sounds, light styles and decals as the game had. Raising an engine limit is fine; lowering
# one fails. These numbers are never lowered; tools/test_save_rules_check.py checks them against the public commit's source.
MINIMUMS = {
    "DEFAULT_MAX_EDICTS": 1200,
    "MAX_MODELS": 1024,
    "MAX_SOUNDS": 2048,
    "MAX_LIGHTSTYLES": 256,
    "MAX_DECALS": 512,
}
PUBLIC_COMMIT = "cdd64653"  # xbe-cdd64653, v0.2.0

FORMAT = {  # name: the value the public builds write and read
    "SAVEGAME_VERSION": 0x0071,
    "CLIENT_SAVEGAME_VERSION": 0x0067,
}
HEADER_EXPR = "(('V'<<24)+('A'<<16)+('S'<<8)+'J')"  # little-endian "JSAV"

LIMIT_FILES = [
    os.path.join(REPO, "engine", "common", "protocol.h"),
    os.path.join(REPO, "engine", "common", "common.h"),
    os.path.join(REPO, "common", "defaults.h"),
]


class RulesError(Exception):
    pass


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def defines(text, low_memory=XASH_LOW_MEMORY, touched=None):
    """name -> expression text, from the #defines a build with XASH_LOW_MEMORY=low_memory sees. It follows #if/#elif/#else/
    #endif on XASH_LOW_MEMORY == N, and #ifndef/#ifdef NAME for names it has defined. Any other condition it cannot decide:
    its branches are skipped, and every name defined or undefined inside one is added to `touched`, so a caller that
    protects a name can refuse to answer for it instead of reading the wrong branch."""
    out, stack = {}, []
    if touched is None:
        touched = set()

    def live():
        return all(s["on"] for s in stack)

    def unknown():
        return any(s.get("unknown") for s in stack)

    for raw in strip_comments(text).splitlines():
        line = raw.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif|define|undef)\b\s*(.*)", line)
        if not m:
            continue
        kind, rest = m.group(1), m.group(2).strip()
        if kind == "undef":
            name = rest.split()[0] if rest else ""
            if unknown():
                touched.add(name)
            if live():
                out.pop(name, None)
        elif kind == "define":
            dm = re.match(r"(\w+)(?:\s+(.*))?$", rest)
            if dm and unknown():
                touched.add(dm.group(1))
            if live():
                if dm and "(" not in dm.group(1):
                    out[dm.group(1)] = (dm.group(2) or "").strip()  # the last active definition is the one the compiler keeps
        elif kind in ("if", "ifdef", "ifndef"):
            lm = re.match(r"XASH_LOW_MEMORY\s*==\s*(\d+)$", rest)
            if kind == "ifndef":
                on, unk = rest not in out, False
            elif kind == "ifdef":
                on, unk = rest in out, False
            elif lm:
                on, unk = int(lm.group(1)) == low_memory, False
            else:
                on, unk = False, True  # `#if XASH_LOW_MEMORY`, `#if defined(...)`, ...: not decided here
            stack.append({"on": on, "taken": on, "unknown": unk})
        elif kind == "elif" and stack:
            lm = re.match(r"XASH_LOW_MEMORY\s*==\s*(\d+)$", rest)
            if not lm:
                stack[-1]["unknown"] = True  # an expression this does not read: the branch cannot be decided
            on = (not stack[-1]["taken"]) and bool(lm and int(lm.group(1)) == low_memory)
            stack[-1]["on"] = on
            stack[-1]["taken"] = stack[-1]["taken"] or on
        elif kind == "else" and stack:
            stack[-1]["on"] = not stack[-1]["taken"]
            stack[-1]["taken"] = True
        elif kind == "endif" and stack:
            stack.pop()
    return out


def evaluate(expr, table, depth=0):
    """an integer from a macro expression: numbers, other macros, ( ), << and the + - * / the headers use"""
    if depth > 20:
        raise RulesError("macro loop in %r" % expr)
    expr = expr.strip()

    def sub(m):
        name = m.group(0)
        if name in table:
            return "(%d)" % evaluate(table[name], table, depth + 1)
        return name

    expr = re.sub(r"\b[A-Za-z_]\w*\b", sub, expr)
    if not re.fullmatch(r"[0-9xXa-fA-F()<>+\-*/ ]+", expr):
        raise RulesError("cannot evaluate %r" % expr)
    return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - the charset above is digits and operators only


def source_problems(sv_save=SV_SAVE, limit_files=None, minimums=None):
    problems = []
    try:
        with open(sv_save, encoding="utf-8") as f:
            sv = f.read()
    except OSError as e:
        return ["cannot read %s: %s" % (sv_save, e)]
    touched_sv = set()
    d = defines(sv, touched=touched_sv)
    for name, want in FORMAT.items():
        try:
            got = evaluate(d[name], d) if name in d else None
        except RulesError as e:
            got = str(e)
        if got != want:
            problems.append("%s is %s, saves are written and read as 0x%X" % (name, got if got is None else
                                                                         (hex(got) if isinstance(got, int) else got), want))
    if d.get("SAVEGAME_HEADER", "").replace(" ", "") != HEADER_EXPR.replace(" ", ""):
        problems.append("SAVEGAME_HEADER is %r, not the \"JSAV\" tag" % d.get("SAVEGAME_HEADER"))
    protected = set(FORMAT) | {"SAVEGAME_HEADER"}
    for n in sorted(protected & touched_sv):
        problems.append("%s is defined under a condition this check cannot read: it cannot vouch for it" % n)

    table = {}
    touched = set()
    for p in limit_files or LIMIT_FILES:
        try:
            with open(p, encoding="utf-8") as f:
                for k, v in defines(f.read(), touched=touched).items():
                    table.setdefault(k, v)
        except OSError as e:
            problems.append("cannot read %s: %s" % (p, e))
    mins = MINIMUMS if minimums is None else minimums
    # a protected limit, or a name its value is made of, defined where the condition is unreadable: fail closed
    watch, todo = set(), list(mins)
    while todo:
        n = todo.pop()
        if n in watch:
            continue
        watch.add(n)
        todo += re.findall(r"[A-Za-z_]\w*", table.get(n, ""))
    for n in sorted(watch & touched):
        problems.append("%s feeds a saved-game limit and is defined under a condition this check cannot read: it cannot vouch for it" % n)
    for name, want in sorted(mins.items()):
        if name not in table:
            problems.append("limit %s is not defined where it was: the rule cannot be checked (renamed?)" % name)
            continue
        try:
            got = evaluate(table[name], table)
        except RulesError as e:
            problems.append("limit %s: %s" % (name, e))
            continue
        if got < want:
            problems.append("limit %s is %d, lowered from %d: saves that hold more than that would not load" % (name, got, want))
    return problems


def title_id(data):
    """dwTitleId from the XBE certificate"""
    if data[:4] != b"XBEH":
        raise RulesError("not an XBE")
    base = struct.unpack_from("<I", data, 0x104)[0]
    cert = struct.unpack_from("<I", data, 0x118)[0] - base
    if cert < 0 or cert + 0x10 > len(data):
        raise RulesError("the certificate is outside the file")
    return struct.unpack_from("<I", data, cert + 0x08)[0]


def xbe_problems(data):
    try:
        got = title_id(data)
    except (RulesError, struct.error) as e:
        return [str(e)]
    if got != TITLE_ID:
        return ["the title id is 0x%08X, not 0x%08X: the console would look for saves in another UDATA folder" % (got, TITLE_ID)]
    return []


def require_xbe(data):
    """For the release path: SystemExit unless the XBE keeps the title id."""
    p = xbe_problems(data)
    if p:
        raise SystemExit("This XBE would not find the saved games already on consoles (docs/r3d/fork-plan.md, XS.5):\n  - "
                         + "\n  - ".join(p))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("source")
    x = sub.add_parser("xbe")
    x.add_argument("xbe")
    a = ap.parse_args(argv)
    if a.cmd == "source":
        p = source_problems()
    else:
        with open(a.xbe, "rb") as f:
            p = xbe_problems(f.read())
    if p:
        sys.exit("save rules: " + "; ".join(p))
    print("save rules: ok")


if __name__ == "__main__":
    main()
