#!/usr/bin/env python3
"""Build tools/packager/payload from this repo and a built XBE.

    python tools/packager/make_payload.py --xbe path/to/default.xbe [--title hl]

The XBE is retitled "Half-Life" (the console dashboard shows that string), gets the
Xash3D boot logo and the $$XTIMAGE / $$XSIMAGE images, and a default.tbn tile goes
beside it; and
the build record - repo commit, the XBE's own sha256, whether the tree was
clean - goes into the manifest so a packaged app says which build it carries.
The XBE should come from a clean build of main: the packager ships it as the
stable build.
"""

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from r3dpack import xbepatch  # noqa: E402
from r3dpack.titles import TITLES  # noqa: E402


def git(*a):
    return subprocess.run(["git", "-C", REPO] + list(a), capture_output=True, text=True).stdout.strip()


# (disc path under the game folder, repo source) - the files tools/xbox-xiso.sh stages
def repo_files(gamedir):
    out = {
        gamedir + "/gfx/fonts/FiraSansCondensed-Regular.ttf": "assets/fonts/FiraSansCondensed-Regular.ttf",
        gamedir + "/gfx/fonts/FiraSansCondensed-OFL.txt": "assets/fonts/FiraSansCondensed-OFL.txt",
        gamedir + "/gfx/shell/colors.lst": "assets/gfx/shell/colors.lst",
        gamedir + "/gfx/shell/legend_button.tga": "assets/gfx/shell/legend_button.tga",
        gamedir + "/gfx/shell/duke.tga": "assets/gfx/shell/duke.tga",
    }
    rdir = os.path.join(REPO, "assets", "resource")
    for n in sorted(os.listdir(rdir)):
        if n.startswith("mainui_") and n.endswith(".txt"):
            out[gamedir + "/resource/" + n] = "assets/resource/" + n
    return out


# The menu's control images (checkbox, slider, arrows): without them every options control draws
# greyed out. They come from xash-extras, not from the game's files.
SHELL_CONTROLS = (
    ["cb_checked", "cb_disabled", "cb_down", "cb_empty", "cb_over", "down", "gamma", "slider", "up"]
    + ["%s%s" % (a, s) for a in ("dnarrow", "uparrow") for s in ("d", "f", "p")]
    + ["%s%s" % (a, s) for a in ("larrow", "rarrow") for s in ("default", "flyover", "pressed")]
)

# The retail video.cfg leaves r_refdll empty; the console needs the nv2a renderer.
VIDEO_CFG = b'// written by the Resonance3D packager\r\nr_refdll "nv2a"\r\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--title", default="hl")
    ap.add_argument("--out", default=os.path.join(HERE, "payload"))
    a = ap.parse_args()
    t = TITLES[a.title]
    tdir = os.path.join(a.out, a.title)
    shutil.rmtree(tdir, ignore_errors=True)
    files = {}

    def put(rel, data):
        p = os.path.join(tdir, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
        files[rel] = hashlib.sha256(data).hexdigest()

    with open(a.xbe, "rb") as f:
        raw = f.read()
    assets = os.path.join(HERE, "assets")
    with open(os.path.join(assets, "boot-logo.pgm"), "rb") as f:
        xbe = xbepatch.set_logo(xbepatch.set_title(raw, t["label"]), f.read())
    # the dashboard's own title and save images, as sections of the XBE
    with open(os.path.join(assets, "titleimage.xbx"), "rb") as f:
        xt = f.read()
    with open(os.path.join(assets, "saveimage.xbx"), "rb") as f:
        xs = f.read()
    xbe = xbepatch.add_sections(xbe, [("$$XTIMAGE", xt), ("$$XSIMAGE", xs)])
    put("titleimage.xbx", xt)  # the engine copies it to UDATA for the memory screen (divergences.md #190)
    put("default.xbe", xbe)
    # dashboard tile beside the XBE (UnleashX, XBMC and the dashboards that follow them)
    with open(os.path.join(assets, "icon-xash-128.png"), "rb") as f:
        put("default.tbn", f.read())
    for rel, src in repo_files(t["gamedir"]).items():
        with open(os.path.join(REPO, src), "rb") as f:
            put(rel, f.read())
    extras = os.path.join(REPO, "3rdparty", "extras", "xash-extras", "gfx", "shell")
    for n in SHELL_CONTROLS:
        with open(os.path.join(extras, n + ".tga"), "rb") as f:
            put(t["gamedir"] + "/gfx/shell/" + n + ".tga", f.read())
    put(t["gamedir"] + "/video.cfg", VIDEO_CFG)

    man_path = os.path.join(a.out, "manifest.json")
    try:
        with open(man_path) as f:
            man = json.load(f)
    except (OSError, ValueError):
        man = {"titles": {}}
    man["titles"][a.title] = {
        "files": files,
        "build": {"commit": git("rev-parse", "HEAD"), "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
                  "clean": git("status", "--porcelain", "--untracked-files=no") == "",
                  "xbe_source_sha256": hashlib.sha256(raw).hexdigest()},
    }
    os.makedirs(a.out, exist_ok=True)
    with open(man_path, "w") as f:
        json.dump(man, f, indent=1, sort_keys=True)
    print("payload for %s: %d files, xbe source sha256 %s"
          % (a.title, len(files), man["titles"][a.title]["build"]["xbe_source_sha256"][:12]))


if __name__ == "__main__":
    main()
