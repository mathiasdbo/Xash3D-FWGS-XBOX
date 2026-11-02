"""Assemble a title's disc layout and write it out as a folder or an XISO."""

import os
import re
import shutil

from . import payload as payload_mod
from . import xdvdfs
from .inspect_install import inspect
from .titles import TITLES


class BuildError(Exception):
    pass


def _walk(root, prefix):
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for n in sorted(fn):
            full = os.path.join(dp, n)
            rel = prefix + "/" + os.path.relpath(full, root).replace(os.sep, "/")
            yield rel, full, os.path.getsize(full)


_FLAGS = ("animated_title", "hd_background")


def _sync_gameinfo(gameinfo, liblist):
    """tools/xbox-xiso.sh sync_gameinfo_hl25_flags: gameinfo.txt wins over liblist.gam
    when both exist, but the Steam one omits these two flags liblist.gam sets, so copy
    them across rather than lose the animated title and HD background."""
    text = gameinfo.decode("latin-1")
    lib = liblist.decode("latin-1")
    changed = False
    for key in _FLAGS:
        if re.search(r"^\s*%s\s" % key, text, re.I | re.M):
            continue
        if re.search(r'^\s*%s\s+"?1"?\s*(//.*)?$' % key, lib, re.I | re.M):
            if not text.endswith("\n"):
                text += "\n"
            text += '%s\t\t"1"\n' % key
            changed = True
    return text.encode("latin-1") if changed else gameinfo


def plan(steam_root, title, payload_dir=None, log=print, baked=None):
    """Returns (files, build_info). files: (disc path, source path or bytes, size).

    baked: the files bake.bake() made; they go on the disc after the retail files, so the
    baked menu layout replaces the retail one."""
    t = TITLES[title]
    if not t["enabled"]:
        raise BuildError("%s is not supported yet." % t["label"])
    info = inspect(steam_root, title)
    if not info["ok"]:
        raise BuildError("\n".join(info["problems"]))
    for n in info["notes"]:
        log(n)
    build, extra = payload_mod.load(title, payload_dir)

    files = {}
    for gd in dict.fromkeys([t["gamedir"], t["basedir"]]):
        for rel, src, size in _walk(os.path.join(steam_root, gd), gd):
            files[rel.lower()] = (rel, src, size)

    # gameinfo.txt: add the two flags the Steam copy leaves out
    for gd in dict.fromkeys([t["gamedir"], t["basedir"]]):
        gi = files.get((gd + "/gameinfo.txt").lower())
        ll = files.get((gd + "/liblist.gam").lower())
        if gi and ll:
            with open(gi[1], "rb") as f:
                old = f.read()
            with open(ll[1], "rb") as f:
                new = _sync_gameinfo(old, f.read())
            if new != old:
                files[gi[0].lower()] = (gi[0], new, len(new))
                log("%s: added the animated title / HD background flags from liblist.gam" % gi[0])

    for rel, src, size in list(baked or []) + list(extra):
        files[rel.lower()] = (rel, src, size)

    # The baked archive holds the map. A loose .bsp beside it is loaded instead and the Xbox
    # renderer then has no world to draw (black, models only), so a baked map's .bsp stays off the disc.
    gd = t["gamedir"]
    dropped = 0
    for rel, _, _ in list(baked or []):
        low = rel.lower()
        if low.startswith(gd + "/") and low.count("/") == 1 and low.endswith(".mpak"):
            if files.pop("%s/maps/%s.bsp" % (gd, low[len(gd) + 1:-5]), None):
                dropped += 1
    if dropped:
        log("Left out %d loose .bsp files: their maps are inside the baked archives." % dropped)
    return [files[k] for k in sorted(files)], build


def export_folder(files, out_dir, title, progress=None, log=print):
    target = os.path.join(out_dir, TITLES[title]["folder"])
    if os.path.exists(target) and os.listdir(target):
        raise BuildError("%s already exists and is not empty. Choose another place or remove it." % target)
    total = sum(s for _, _, s in files) or 1
    done = 0
    try:
        for rel, src, size in files:
            dst = os.path.join(target, *rel.split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if isinstance(src, (bytes, bytearray)):
                with open(dst, "wb") as f:
                    f.write(src)
            else:
                shutil.copyfile(src, dst)
            if os.path.getsize(dst) != size:
                raise BuildError("Copy of %s is the wrong size." % rel)
            done += size
            if progress:
                progress(done, total, rel)
    except BaseException:
        shutil.rmtree(target, ignore_errors=True)
        raise
    log("Folder written: %s" % target)
    return target


def export_xiso(files, out_dir, title, progress=None, log=print):
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, TITLES[title]["folder"] + ".iso")
    if os.path.exists(out):
        raise BuildError("%s already exists. Remove it or choose another place." % out)
    try:
        size = xdvdfs.write_xiso(files, out, progress)
        # read it back: the tree must list every file at its size
        got = xdvdfs.read_xiso(out)
        for rel, _, sz in files:
            if got.get(rel, (None, None))[1] != sz:
                raise BuildError("The image does not list %s correctly." % rel)
    except xdvdfs.XisoError as e:
        raise BuildError(str(e))
    log("XISO written: %s (%d bytes)" % (out, size))
    return out
