"""Look inside a Half-Life install before packaging it, and say what is wrong.

The checks are the ones tools/xbox-xiso.sh makes: the engine stops at boot
without a delta.lst (net_encode.c: Delta_InitFields -> Sys_Error), and the
menu draws raw lookup keys without resource/gameui_english.txt.
"""

import os
import struct

from .titles import TITLES


def _pak_names(path):
    try:
        with open(path, "rb") as f:
            head = f.read(12)
            if head[:4] != b"PACK":
                return set()
            off, length = struct.unpack_from("<II", head, 4)
            f.seek(off)
            raw = f.read(length)
    except OSError:
        return set()
    names = set()
    for i in range(0, len(raw) - 63, 64):
        names.add(raw[i:i + 56].split(b"\0")[0].decode("latin-1").lower())
    return names


def _find(gamedirs, rel):
    """True if rel exists loose or inside any pak of the given game folders."""
    for g in gamedirs:
        if os.path.isfile(os.path.join(g, *rel.split("/"))):
            return True
    for g in gamedirs:
        try:
            paks = sorted(n for n in os.listdir(g) if n.lower().endswith(".pak"))
        except OSError:
            continue
        for p in paks:
            if rel.lower() in _pak_names(os.path.join(g, p)):
                return True
    return False


def inspect(root, title="hl"):
    """Returns a dict: ok, problems (blocking), notes, size, files, gamedir, basedir."""
    t = TITLES[title]
    res = {"ok": False, "problems": [], "notes": [], "size": 0, "files": 0,
           "gamedir": t["gamedir"], "basedir": t["basedir"]}
    if not root or not os.path.isdir(root):
        res["problems"].append("That folder does not exist.")
        return res
    gdir = os.path.join(root, t["gamedir"])
    bdir = os.path.join(root, t["basedir"])
    if not os.path.isfile(os.path.join(gdir, "liblist.gam")):
        res["problems"].append(
            "%s/liblist.gam was not found. Pick the folder that contains the '%s' folder "
            "(for Steam: steamapps/common/Half-Life)." % (t["gamedir"], t["gamedir"]))
        return res
    if t["basedir"] != t["gamedir"] and not os.path.isdir(bdir):
        res["problems"].append("The base game folder '%s' is missing." % t["basedir"])
        return res
    dirs = [gdir] + ([bdir] if bdir != gdir else [])
    if not os.path.isfile(os.path.join(gdir, "gameinfo.txt")):
        res["notes"].append("No gameinfo.txt: the engine will derive it from liblist.gam.")
    if not _find(dirs, "delta.lst"):
        res["problems"].append(
            "delta.lst is not in this install (not loose, not in any .pak). The game stops at "
            "startup without it; it first shipped with Half-Life 1.1.0.0. Update the game on "
            "Steam, or use an install that has it.")
    if not _find(dirs, "resource/gameui_english.txt"):
        res["problems"].append(
            "resource/gameui_english.txt is not in this install, so every menu label would show "
            "as its lookup key. Use a current Steam install.")
    size = files = 0
    for d in dirs:
        for dp, _, fn in os.walk(d):
            for n in fn:
                try:
                    size += os.path.getsize(os.path.join(dp, n))
                    files += 1
                except OSError:
                    pass
    res["size"], res["files"] = size, files
    res["ok"] = not res["problems"]
    return res
