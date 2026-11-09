"""Bake the Xbox data the game needs from the user's own Half-Life files.

The Xbox renderer draws the world from a per-map .mpak (textures, lightmaps,
vertex data), the menu draws its background from baked tiles, skies are DXT1
(a sixth of the GPU memory of the retail TGAs), and the music plays from ADPCM
.xwav files. None of it is shippable - it is made from the
user's retail files - so the packager runs tools/asset-studio (the repo's own
baker, built for the host) against their install and puts the results on the
disc beside the retail files.

Results are cached under the user's cache folder, keyed by the baker and the
files it read, so the few minutes this takes are paid once per install.
"""

import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

BAKER_NAME = "AssetStudio.exe" if os.name == "nt" else "AssetStudio"
MUSIC_RATES = (None, 48000)  # source rate, and the console's mix rate
BAKE_VERSION = 2  # bump when a step is added or changed: cached results from before are not reused


class BakeError(Exception):
    pass


def find_baker(explicit=None):
    here = os.path.dirname(os.path.abspath(__file__))
    packager = os.path.dirname(here)
    cands = []
    if explicit:
        cands.append(explicit)
    if os.environ.get("R3D_BAKER"):
        cands.append(os.environ["R3D_BAKER"])
    base = getattr(sys, "_MEIPASS", None)
    if base:
        cands.append(os.path.join(base, "bin", BAKER_NAME))
    cands.append(os.path.join(packager, "bin", BAKER_NAME))
    for c in cands:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    raise BakeError("The asset baker (%s) was not found. Build it with tools/packager/build_baker.py, "
                    "or pass --baker <path>." % BAKER_NAME)


def _adopt_old_cache(old, new):
    """The product was renamed; a bake kept under the old name is moved, not redone."""
    try:
        if os.path.isdir(old) and not os.path.exists(new):
            os.makedirs(os.path.dirname(new), exist_ok=True)
            os.rename(old, new)
    except OSError:
        pass


def default_cache_root():
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        root = os.path.join(base, "Xash3D-FWGS-XBOX Packager", "bake")
        _adopt_old_cache(os.path.join(base, "Resonance3D Packager", "bake"), root)
        return root
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    root = os.path.join(base, "xash3d-fwgs-xbox-packager", "bake")
    _adopt_old_cache(os.path.join(base, "resonance3d-packager", "bake"), root)
    return root


def _sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def _inputs(gamedir_path):
    """The files the bake reads, as (name, size) pairs - what changes the result."""
    out = []
    for sub, exts in (("maps", (".bsp",)), ("", (".wad",)), ("media", (".mp3",)), ("resource", (".txt", ".tga")),
                      ("gfx/env", (".tga",))):
        d = os.path.join(gamedir_path, sub) if sub else gamedir_path
        try:
            names = sorted(os.listdir(d))
        except OSError:
            continue
        for n in names:
            if n.lower().endswith(exts) and (sub != "resource" or "background" in n.lower() or "layout" in n.lower()):
                p = os.path.join(d, n)
                out.append((sub + "/" + n, os.path.getsize(p)))
    return out


def cache_key(baker, gamedir_path):
    h = hashlib.sha256()
    h.update(("v%d:" % BAKE_VERSION).encode())
    h.update(_sha(baker).encode())
    for n, s in _inputs(gamedir_path):
        h.update(("%s:%d\n" % (n, s)).encode())
    return h.hexdigest()[:24]


def _run(baker, args, log_path):
    with open(log_path, "wb") as lf:
        r = subprocess.run([baker] + args, stdout=lf, stderr=subprocess.STDOUT)
    return r.returncode


def _tree(root, prefix):
    """(disc path, file, size) for everything under root."""
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for n in sorted(fn):
            full = os.path.join(dp, n)
            yield prefix + "/" + os.path.relpath(full, root).replace(os.sep, "/"), full, os.path.getsize(full)


def bake(steam_root, gamedir, baker=None, cache_root=None, progress=None, log=print, jobs=None):
    """Returns [(disc path, file, size)] for the baked files, baking if the cache has none.

    progress(done, total, name) counts baked items (maps, then the menu and the music)."""
    baker = find_baker(baker)
    gd_path = os.path.join(steam_root, gamedir)
    maps_dir = os.path.join(gd_path, "maps")
    maps = sorted(n[:-4] for n in os.listdir(maps_dir) if n.lower().endswith(".bsp")) if os.path.isdir(maps_dir) else []
    if not maps:
        raise BakeError("No maps found in %s." % maps_dir)
    key = cache_key(baker, gd_path)
    root = os.path.join(cache_root or default_cache_root(), key)
    done_mark = os.path.join(root, "complete")
    out = os.path.join(root, "out")
    if os.path.exists(done_mark):
        log("Baked data found in the cache (%s)." % key)
        return list(_tree(out, gamedir))

    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(out)
    logs = os.path.join(root, "logs")
    os.makedirs(logs)
    total = len(maps) + 2 + len(MUSIC_RATES)
    state = {"n": 0}

    def tick(name):
        state["n"] += 1
        if progress:
            progress(state["n"], total, name)

    log("Baking the Xbox data from your game files (%d maps, the menu art, the skies, the music). "
        "This takes a few minutes the first time." % len(maps))
    failed = []

    def bake_map(m):
        scratch = os.path.join(logs, m + ".d")
        os.makedirs(scratch, exist_ok=True)
        rc = _run(baker, ["--game-dir", gd_path, "--map", os.path.join(maps_dir, m + ".bsp"),
                          "--output", os.path.join(scratch, m + ".mpak"), "--target", "xbox", "--profile", "nv2a"],
                  os.path.join(logs, m + ".log"))
        mp = os.path.join(scratch, m + ".mpak")
        if rc == 0 and os.path.isfile(mp):
            shutil.move(mp, os.path.join(out, m + ".mpak"))
            shutil.rmtree(scratch, ignore_errors=True)
            return m, True
        return m, False

    workers = jobs or max(1, min(8, (os.cpu_count() or 2)))
    with concurrent.futures.ThreadPoolExecutor(workers) as ex:
        for m, ok in ex.map(bake_map, maps):
            if not ok:
                failed.append(m)
            tick(m + ".mpak")
    if len(failed) == len(maps):
        raise BakeError("The baker failed on every map; see the logs in %s" % logs)
    if failed:
        log("Could not bake %d map(s): %s. They will load without the Xbox data." % (len(failed), ", ".join(failed)))

    # menu background tiles
    menu = os.path.join(root, "menu")
    rc = _run(baker, ["--game-dir", gd_path, "--bake-menu-art", "--menu-art-dxt1", "--output", menu],
              os.path.join(logs, "menu.log"))
    if rc != 0 or not os.path.isfile(os.path.join(menu, "resource", "BackgroundLayout.txt")):
        raise BakeError("The menu art could not be baked; see %s" % os.path.join(logs, "menu.log"))
    shutil.copytree(menu, out, dirs_exist_ok=True)
    tick("menu art")

    # skies as DXT1: 192 KiB of GPU memory each instead of 1.5 MiB (the TGAs stay as the fallback)
    sky = os.path.join(root, "sky")
    rc = _run(baker, ["--game-dir", gd_path, "--bake-sky-dxt1", "--output", sky], os.path.join(logs, "sky.log"))
    if rc != 0 or not os.path.isdir(os.path.join(sky, "gfx", "env")):
        raise BakeError("The skies could not be baked; see %s" % os.path.join(logs, "sky.log"))
    shutil.copytree(sky, out, dirs_exist_ok=True)
    tick("skies")

    # music: the source-rate and the 48 kHz tracks side by side
    for rate in MUSIC_RATES:
        md = os.path.join(root, "music%s" % (rate or ""))
        args = ["--game-dir", gd_path, "--music", md] + (["--music-rate", str(rate)] if rate else [])
        rc = _run(baker, args, os.path.join(logs, "music%s.log" % (rate or "")))
        media = os.path.join(md, "media")
        if rc != 0 or not os.path.isdir(media):
            raise BakeError("The music could not be baked; see %s" % os.path.join(logs, "music%s.log" % (rate or "")))
        os.makedirs(os.path.join(out, "media"), exist_ok=True)
        for n in os.listdir(media):
            shutil.copyfile(os.path.join(media, n), os.path.join(out, "media", n))
        tick("music" + (" %d Hz" % rate if rate else ""))

    shutil.rmtree(menu, ignore_errors=True)
    shutil.rmtree(sky, ignore_errors=True)
    for rate in MUSIC_RATES:
        shutil.rmtree(os.path.join(root, "music%s" % (rate or "")), ignore_errors=True)
    with open(done_mark, "w") as f:
        f.write("ok\n")
    files = list(_tree(out, gamedir))
    log("Baked %d files, %.0f MiB." % (len(files), sum(s for _, _, s in files) / 1048576.0))
    return files
