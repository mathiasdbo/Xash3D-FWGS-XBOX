#!/usr/bin/env python3
"""Build the packager's release files locally, and publish them only when asked.

    python tools/packager/release.py --xbe <clean main build> --version v0.21
    python tools/packager/release.py --xbe <...> --version v0.21 --publish

Without --publish nothing leaves this machine: the files land in
tools/packager/dist and the gh command that would publish them is printed.
With --publish the tag is created on the current commit of the repo and the
files are attached to a new GitHub release.

Files:
  Xash3D-FWGS-XBOX-Packager-<ver>-windows.exe   one file, no Python needed
  Xash3D-FWGS-XBOX-Packager-<ver>-portable.zip  any OS with Python 3 and Tk:
                                           python3 r3d_packager.py
  SHA256SUMS.txt

The Windows exe is built here; a binary for Linux can only be built on Linux,
so the portable zip is the Linux release.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DIST = os.path.join(HERE, "dist")


def run(cmd, **kw):
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def git(*a):
    return subprocess.run(["git", "-C", REPO] + list(a), capture_output=True, text=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xbe", required=True, help="the XBE to ship (a clean build of main)")
    ap.add_argument("--version", required=True, help="release tag, e.g. v0.21")
    ap.add_argument("--publish", action="store_true")
    ap.add_argument("--allow-dirty", action="store_true")
    a = ap.parse_args()

    if not a.allow_dirty and git("status", "--porcelain", "--untracked-files=no"):
        sys.exit("The tree has uncommitted changes. Commit them, or pass --allow-dirty for a test build.")

    shutil.rmtree(DIST, ignore_errors=True)
    os.makedirs(DIST)
    run([sys.executable, os.path.join(HERE, "make_payload.py"), "--xbe", a.xbe])
    # the asset baker the packager runs on the user's own files (tools/asset-studio, built for this OS)
    shutil.rmtree(os.path.join(HERE, "bin"), ignore_errors=True)
    run([sys.executable, os.path.join(HERE, "build_baker.py")])

    ver = a.version
    exe_name = "Xash3D-FWGS-XBOX-Packager-%s-windows.exe" % ver
    if sys.platform == "win32":
        sep = ";"
        run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--onefile", "--windowed",
             "--name", exe_name[:-4], "--icon", os.path.join(HERE, "assets", "icon-xash.ico"),
             "--add-data", os.path.join(HERE, "payload") + sep + "payload",
             "--add-data", os.path.join(HERE, "assets") + sep + "assets",
             "--add-binary", os.path.join(HERE, "bin", "AssetStudio.exe") + sep + "bin",
             "--distpath", DIST, "--workpath", os.path.join(HERE, "build"),
             "--specpath", os.path.join(HERE, "build"),
             os.path.join(HERE, "r3d_packager.py")])
    else:
        print("Not on Windows: skipping the .exe.")

    zname = "Xash3D-FWGS-XBOX-Packager-%s-portable.zip" % ver
    with zipfile.ZipFile(os.path.join(DIST, zname), "w", zipfile.ZIP_DEFLATED) as z:
        top = "Xash3D-FWGS-XBOX-Packager"
        z.write(os.path.join(HERE, "r3d_packager.py"), top + "/r3d_packager.py")
        z.write(os.path.join(HERE, "build_baker.py"), top + "/build_baker.py")
        # the baker built here only runs on this OS; elsewhere build_baker.py makes one from the repo
        for base in ("r3dpack", "payload", "assets", "bin"):
            for dp, dn, fn in os.walk(os.path.join(HERE, base)):
                dn[:] = [d for d in dn if d != "__pycache__"]
                for n in fn:
                    full = os.path.join(dp, n)
                    z.write(full, top + "/" + os.path.relpath(full, HERE).replace(os.sep, "/"))

    files = sorted(n for n in os.listdir(DIST) if os.path.isfile(os.path.join(DIST, n)))
    with open(os.path.join(DIST, "SHA256SUMS.txt"), "w") as f:
        for n in files:
            f.write("%s  %s\n" % (sha(os.path.join(DIST, n)), n))
    files.append("SHA256SUMS.txt")

    with open(os.path.join(HERE, "payload", "manifest.json")) as f:
        build = json.load(f)["titles"]["hl"]["build"]
    notes = os.path.join(DIST, "NOTES.md")
    with open(notes, "w") as f:
        f.write("Xash3D-FWGS-XBOX Packager %s\n\n"
                "Packs your own Half-Life files with the Xbox build into a console-ready folder or XISO.\n\n"
                "- Windows: run the .exe.\n"
                "- Linux / other: unzip the portable zip and run `python3 r3d_packager.py` "
                "(needs Python 3 with Tk, e.g. `python3-tk`). The zip's baker is the Windows one; "
                "on Linux run `python3 build_baker.py` from a checkout of the repository first "
                "(needs cmake and g++).\n"
                "- The first export bakes the Xbox data from your own game files (a few minutes) and "
                "keeps the result for next time.\n\n"
                "Xbox build inside: commit %s (%s), XBE source sha256 %s.\n"
                % (ver, build["commit"][:10], build["branch"], build["xbe_source_sha256"]))

    cmd = ["gh", "release", "create", ver, "--title", "Xash3D-FWGS-XBOX Packager " + ver,
           "--notes-file", notes] + [os.path.join(DIST, n) for n in files]
    if not a.publish:
        print("\nBuilt, not published. To publish:\n  " + " ".join(cmd))
        return
    run(cmd, cwd=REPO)


if __name__ == "__main__":
    main()
