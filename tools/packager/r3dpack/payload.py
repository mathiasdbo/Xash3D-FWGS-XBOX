"""The payload: everything the console build adds to the retail files.

Built by make_payload.py from this repo (the XBE plus the fonts, colours and
translations the build expects) and checked here against its manifest, so a
damaged or half-copied payload stops the build instead of making a disc that
boots into the wrong font.
"""

import hashlib
import json
import os
import sys


class PayloadError(Exception):
    pass


def default_dir():
    base = getattr(sys, "_MEIPASS", None)
    if base:
        return os.path.join(base, "payload")
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "payload")


def _sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def load(title, directory=None):
    """Returns (info, files): files is a list of (disc path, source path, size)."""
    directory = directory or default_dir()
    mpath = os.path.join(directory, "manifest.json")
    try:
        with open(mpath, encoding="utf-8") as f:
            man = json.load(f)
    except (OSError, ValueError) as e:
        raise PayloadError("The payload is missing or unreadable (%s). Run make_payload.py." % e)
    entry = man.get("titles", {}).get(title)
    if not entry:
        raise PayloadError("The payload has no build for '%s'." % title)
    out = []
    for rel, sha in sorted(entry["files"].items()):
        src = os.path.join(directory, title, *rel.split("/"))
        if not os.path.isfile(src):
            raise PayloadError("Payload file missing: %s" % rel)
        if _sha(src) != sha:
            raise PayloadError("Payload file damaged (hash differs): %s" % rel)
        out.append((rel, src, os.path.getsize(src)))
    if not any(r == "default.xbe" for r, _, _ in out):
        raise PayloadError("The payload has no default.xbe.")
    return entry.get("build", {}), out
