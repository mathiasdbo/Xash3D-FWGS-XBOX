#!/usr/bin/env python3
"""The CI steps of the Windows release (.github/workflows/packager-windows.yml), also runnable by hand.

    python tools/packager/ci_build.py exe --version v0.1.1 --out dist-exe
        builds the asset baker and the one-file exe (the packager, its assets and the baker; no Xbox build)
    python tools/packager/ci_build.py assemble --version v0.1.1 --exe <exe, signed or not> \
            --xbe default.xbe --xbe-sha256 <sha256> [--signed] --out dist
        builds the payload from the repo and the XBE, and writes the release files: the windows zip (exe + payload
        beside it), the portable zip, SHA256SUMS.txt and NOTES.md

The exe is built from the repository alone, which is what a signing service can verify. The XBE is the one input
that is not built here yet: it is a clean build of main made elsewhere, so its sha256 is required and checked, and
the payload manifest records it.
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import release  # noqa: E402
from r3dpack import versioninfo  # noqa: E402


def cmd_exe(a):
    versioninfo.parse(a.version)  # a bad version fails now, not after the baker has been built
    release.build_baker()
    exe = release.build_exe(a.version, a.out)
    print("built %s (sha256 %s)" % (exe, release.sha(exe)))


def cmd_assemble(a):
    versioninfo.parse(a.version)
    if not os.path.isfile(a.exe):
        sys.exit("no exe at %s" % a.exe)
    got = release.sha(a.xbe)
    if got != a.xbe_sha256.lower():
        sys.exit("the XBE's sha256 is %s, expected %s: refusing to ship a build nobody vouched for" % (got, a.xbe_sha256))
    shutil.rmtree(a.out, ignore_errors=True)
    os.makedirs(a.out)
    release.make_payload(a.xbe, a.xbe_commit or None, a.xbe_prepatched)
    release.windows_zip(a.version, a.exe, a.out)
    release.portable_zip(a.version, a.out)
    files = release.finish(a.version, a.out, signed=a.signed)
    print("release files in %s: %s" % (a.out, ", ".join(files)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("exe")
    p.add_argument("--version", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_exe)
    p = sub.add_parser("assemble")
    p.add_argument("--version", required=True)
    p.add_argument("--exe", required=True)
    p.add_argument("--xbe", required=True)
    p.add_argument("--xbe-sha256", required=True)
    p.add_argument("--xbe-commit", default="", help="the commit the XBE was built from (default: HEAD)")
    p.add_argument("--xbe-prepatched", action="store_true", help="the XBE is the default.xbe of an earlier payload")
    p.add_argument("--signed", action="store_true", help="say in the release notes that the exe is signed")
    p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_assemble)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
