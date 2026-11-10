#!/usr/bin/env python3
"""Build the packager's release files locally, and publish them only when asked.

    python tools/packager/release.py --xbe <clean main build> --version v0.21
    python tools/packager/release.py --xbe <...> --version v0.21 --publish

Without --publish nothing leaves this machine: the files land in
tools/packager/dist and the gh command that would publish them is printed.
With --publish the tag is created on the current commit of the repo and the
files are attached to a new GitHub release.

Files:
  Xash3D-FWGS-XBOX-Packager-<ver>-windows.zip   the exe (no Python needed) and the payload folder beside it,
                                           which holds the Xbox build: keep them together
  Xash3D-FWGS-XBOX-Packager-<ver>-portable.zip  any OS with Python 3 and Tk:
                                           python3 r3d_packager.py
  SHA256SUMS.txt

The exe contains only what the repository builds (the packager and the asset baker); the Xbox build is
data beside it. That split is what lets CI build the exe and have it signed (tools/packager/ci_build.py,
.github/workflows/packager-windows.yml). The Windows exe can only be built on Windows; a binary for Linux
only on Linux, so the portable zip is the Linux release.
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
sys.path.insert(0, HERE)
from r3dpack import versioninfo  # noqa: E402

EXE = "Xash3D-FWGS-XBOX-Packager.exe"  # inside the windows zip, always this name: SignPath's configuration names it
TOP = "Xash3D-FWGS-XBOX-Packager"


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


def make_payload(xbe):
    """tools/packager/payload from the repo's files and a built XBE (the one input that is not built from source here)"""
    run([sys.executable, os.path.join(HERE, "make_payload.py"), "--xbe", xbe])


def build_baker():
    """the asset baker the packager runs on the user's own files (tools/asset-studio, built for this OS)"""
    shutil.rmtree(os.path.join(HERE, "bin"), ignore_errors=True)
    run([sys.executable, os.path.join(HERE, "build_baker.py")])


def build_exe(ver, out_dir):
    """The one-file Windows exe: the packager, its assets and the baker. No payload: the Xbox build sits beside it."""
    if sys.platform != "win32":
        raise SystemExit("The Windows exe can only be built on Windows.")
    os.makedirs(out_dir, exist_ok=True)
    work = os.path.join(HERE, "build")
    os.makedirs(work, exist_ok=True)
    vfile = os.path.join(work, "version-info.txt")
    with open(vfile, "w", encoding="utf-8") as f:
        f.write(versioninfo.render(ver, EXE))
    run([sys.executable, "-m", "PyInstaller", "--noconfirm", "--onefile", "--windowed",
         "--name", EXE[:-4], "--icon", os.path.join(HERE, "assets", "icon-xash.ico"),
         "--version-file", vfile,
         "--add-data", os.path.join(HERE, "assets") + ";assets",
         "--add-binary", os.path.join(HERE, "bin", "AssetStudio.exe") + ";bin",
         "--distpath", out_dir, "--workpath", work, "--specpath", work,
         os.path.join(HERE, "r3d_packager.py")])
    return os.path.join(out_dir, EXE)


def _add_tree(z, base, top):
    for dp, dn, fn in os.walk(os.path.join(HERE, base)):
        dn[:] = sorted(d for d in dn if d != "__pycache__")
        for n in sorted(fn):
            full = os.path.join(dp, n)
            z.write(full, top + "/" + os.path.relpath(full, HERE).replace(os.sep, "/"))


def windows_zip(ver, exe, dist):
    """the exe, with the payload folder beside it"""
    path = os.path.join(dist, "Xash3D-FWGS-XBOX-Packager-%s-windows.zip" % ver)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(exe, TOP + "/" + EXE)
        _add_tree(z, "payload", TOP)
        z.writestr(TOP + "/README.txt",
                   "Xash3D-FWGS-XBOX Packager %s\r\n\r\nRun %s. Keep the payload folder next to it: it holds the\r\n"
                   "Xbox build the packager puts on your console.\r\n" % (ver, EXE))
    return path


def portable_zip(ver, dist):
    path = os.path.join(dist, "Xash3D-FWGS-XBOX-Packager-%s-portable.zip" % ver)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(HERE, "r3d_packager.py"), TOP + "/r3d_packager.py")
        z.write(os.path.join(HERE, "build_baker.py"), TOP + "/build_baker.py")
        # the baker built here only runs on this OS; elsewhere build_baker.py makes one from the repo
        for base in ("r3dpack", "payload", "assets", "bin"):
            _add_tree(z, base, TOP)
    return path


def finish(ver, dist, signed=False):
    """SHA256SUMS.txt and NOTES.md for what is in dist; the list of files to attach"""
    files = sorted(n for n in os.listdir(dist) if os.path.isfile(os.path.join(dist, n))
                   and n not in ("SHA256SUMS.txt", "NOTES.md"))
    with open(os.path.join(dist, "SHA256SUMS.txt"), "w") as f:
        for n in files:
            f.write("%s  %s\n" % (sha(os.path.join(dist, n)), n))
    files.append("SHA256SUMS.txt")

    with open(os.path.join(HERE, "payload", "manifest.json")) as f:
        build = json.load(f)["titles"]["hl"]["build"]
    with open(os.path.join(dist, "NOTES.md"), "w") as f:
        f.write("Xash3D-FWGS-XBOX Packager %s\n\n"
                "Packs your own Half-Life files with the Xbox build into a console-ready folder or XISO.\n\n"
                "- Windows: unzip the windows zip and run the .exe; keep the payload folder next to it. %s\n"
                "- Linux / other: unzip the portable zip and run `python3 r3d_packager.py` "
                "(needs Python 3 with Tk, e.g. `python3-tk`). The zip's baker is the Windows one; "
                "on Linux run `python3 build_baker.py` from a checkout of the repository first "
                "(needs cmake and g++).\n"
                "- The first export bakes the Xbox data from your own game files (a few minutes) and "
                "keeps the result for next time.\n\n"
                "Xbox build inside: commit %s (%s), XBE source sha256 %s.\n"
                % (ver,
                   "The exe is signed: free code signing by SignPath.io, certificate by SignPath Foundation."
                   if signed else "The exe is not signed: Windows will warn about an unknown publisher.",
                   build["commit"][:10], build["branch"], build["xbe_source_sha256"]))
    files.append("NOTES.md")
    return files


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
    make_payload(a.xbe)
    build_baker()

    ver = a.version
    if sys.platform == "win32":
        exe = build_exe(ver, os.path.join(HERE, "dist-exe"))
        windows_zip(ver, exe, DIST)
    else:
        print("Not on Windows: skipping the .exe.")
    portable_zip(ver, DIST)
    files = finish(ver, DIST)

    cmd = ["gh", "release", "create", ver, "--title", "Xash3D-FWGS-XBOX Packager " + ver,
           "--notes-file", os.path.join(DIST, "NOTES.md")] + [os.path.join(DIST, n) for n in files if n != "NOTES.md"]
    if not a.publish:
        print("\nBuilt, not published. To publish:\n  " + " ".join(cmd))
        return
    run(cmd, cwd=REPO)


if __name__ == "__main__":
    main()
