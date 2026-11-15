"""The packager's net under saved games (docs/r3d/fork-plan.md, XS.6).

Replacing the Xbox build on a console must never cost the player their saved games. Before an upload swaps default.xbe:

  * the console's save folder (E:\\UDATA\\<title id>\\<game>\\<game>\\save, on whichever drive has it) is COPIED to this PC,
    into a dated folder named with the build that was installed. Nothing on the console is changed, moved or deleted: this
    only reads. The copy is checked file by file against the sizes the console reported, and a copy that is not whole
    stops the upload before anything is replaced;
  * the installed build is identified by a hash of its CODE (`code_id`), because its certificate is patched per install and
    its title images are added after the build, so the file's own hash does not say which build it is;
  * a build that is not one of the public ones (v0.1.2, v0.1.3, v0.2.0, whose saves every later build reads) is named in the
    confirmation: nothing says a save it wrote is readable, which is why the copy exists.

`restore_saves` and `restore_previous_build` are the way back after a downgrade (a save written by a build that stores callbacks
by name does not load in an older one): both only run when asked, both only ever add or rename, and neither deletes a save.
"""

import datetime
import hashlib
import json
import os
import posixpath
import re

from . import xbepatch
from .target import TITLE_ID

# code_id of the XBEs whose saved games every later build reads (the frozen legacy table, tools/save-legacy-table.txt),
# measured from the files the xbe-* releases publish (their sha256 is pinned in tools/save_layout_check.py)
PUBLIC_BUILDS = {
    "v0.1.2": "9cbc2ed398b0cad3f0633f0622a140c173c4f9f15c5a32d9de3cde3092c6476c",
    "v0.1.3": "f26631680b89be229bcda2cc3ee3246fbe3b2da26ff9a5cf0810aac9641886e2",
    "v0.2.0": "552f886efcf52a145dd7b0fc20b2aa08c8f73e64ee1aee09a604afc42fead8a9",
}
MANIFEST = "manifest.json"


class SaveNetError(Exception):
    pass


def code_id(xbe):
    """sha256 over the XBE's sections that are the build itself: every one whose name does not start with `$$` (the title
    images the packager appends), with its name and size. The certificate and the boot logo, which are patched per install,
    are in the header and are not part of it."""
    secs = xbepatch.list_sections(xbe)
    h = hashlib.sha256()
    n = 0
    for s in secs:
        if s["name"].startswith("$$"):
            continue
        raw = xbe[s["raw"]:s["raw"] + s["rsize"]]
        if len(raw) != s["rsize"]:
            raise SaveNetError("section %s runs past the end of the file" % s["name"])
        h.update(s["name"].encode("latin-1") + b"\0" + s["rsize"].to_bytes(4, "little") + raw)
        n += 1
    if not n:
        raise SaveNetError("no sections")
    return h.hexdigest()


def classify(installed_id, incoming_id):
    """'same' (nothing is replaced), 'public' (a build whose saves the frozen table covers) or 'unknown'"""
    if installed_id == incoming_id:
        return "same"
    for label, cid in PUBLIC_BUILDS.items():
        if cid and installed_id == cid:
            return "public"
    return "unknown"


def label_of(installed_id):
    for label, cid in PUBLIC_BUILDS.items():
        if cid and installed_id == cid:
            return label
    return "build-" + installed_id[:8]


def find_save_dir(x, gamedir):
    """the console's save folder for the game, on whichever drive has it, or None. Only listings."""
    for drive, (is_dir, _) in sorted(x.listdir("/").items()):
        if not is_dir:
            continue
        p = "/%s/UDATA/%s/%s/%s/save" % (drive, TITLE_ID, gamedir, gamedir)
        try:
            entries = x.listdir(p)
        except Exception:  # noqa: BLE001 - a drive without it, or one that cannot be listed
            continue
        if entries:
            return p
    return None


def _safe_name(name):
    return bool(name) and name not in (".", "..") and re.fullmatch(r"[A-Za-z0-9_.\- ]+", name) is not None


def backup_saves(x, gamedir, dest_root, label, now=None):
    """Copy the console's save folder to dest_root/<date>-<label>/. Returns (folder, file count, bytes), or None when the
    console has no saves. Reads only. The copy is made in a .partial folder and renamed when every file matches the size the
    console listed, so an interrupted or short copy is never mistaken for a backup."""
    src = find_save_dir(x, gamedir)
    if src is None:
        return None
    listing = {n: s for n, (d, s) in x.listdir(src).items() if not d}
    if not listing:
        return None
    stamp = (now or datetime.datetime.now()).strftime("%Y%m%d-%H%M%S")
    name = "%s-%s" % (stamp, re.sub(r"[^A-Za-z0-9_.\-]", "_", label))
    final = os.path.join(dest_root, name)
    part = final + ".partial"
    if os.path.exists(final) or os.path.exists(part):
        raise SaveNetError("%s already exists: not overwriting a backup" % final)
    os.makedirs(part)
    manifest = {"label": label, "source": src, "files": {}}
    total = 0
    for fname, size in sorted(listing.items()):
        if not _safe_name(fname):
            raise SaveNetError("the console has a save file with an unexpected name (%r): not copying it" % fname)
        data = x.read_file(posixpath.join(src, fname))
        if data is None or len(data) != size:
            raise SaveNetError("%s came back as %s bytes, the console lists %d: the backup is not whole"
                               % (fname, "no" if data is None else len(data), size))
        with open(os.path.join(part, fname), "wb") as f:
            f.write(data)
        manifest["files"][fname] = {"size": size, "sha256": hashlib.sha256(data).hexdigest()}
        total += size
    with open(os.path.join(part, MANIFEST), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    os.rename(part, final)
    return final, len(listing), total


def read_manifest(folder):
    try:
        with open(os.path.join(folder, MANIFEST), encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError) as e:
        raise SaveNetError("%s is not a save backup (%s)" % (folder, e))
    if not isinstance(m.get("files"), dict) or not m["files"]:
        raise SaveNetError("%s lists no files" % folder)
    return m


def restore_saves(x, backup_folder, remote_save_dir):
    """Put a backup's files back into the console's save folder. Only adds or replaces a file of the same name, never deletes
    anything, and checks every file against the manifest before sending the first. Returns the number of files sent."""
    m = read_manifest(backup_folder)
    blobs = {}
    for fname, meta in sorted(m["files"].items()):
        if not _safe_name(fname):
            raise SaveNetError("the backup lists a file with an unexpected name (%r)" % fname)
        with open(os.path.join(backup_folder, fname), "rb") as f:
            data = f.read()
        if len(data) != meta["size"] or hashlib.sha256(data).hexdigest() != meta["sha256"]:
            raise SaveNetError("%s does not match the backup's manifest: not restoring" % fname)
        blobs[fname] = data
    for fname, data in blobs.items():
        x.write_file(posixpath.join(remote_save_dir, fname), data)
    return len(blobs)


def restore_previous_build(x, remote_dir):
    """Bring default.xbe.before-packager back as default.xbe. The build being replaced is renamed to default.xbe.after-packager
    (numbered when that exists) and not deleted. Returns the name the replaced build was given."""
    cur, old = posixpath.join(remote_dir, "default.xbe"), posixpath.join(remote_dir, "default.xbe.before-packager")
    have = x.listdir(remote_dir)
    if "default.xbe.before-packager" not in have:
        raise SaveNetError("there is no default.xbe.before-packager in %s: nothing to go back to" % remote_dir)
    new, k = cur + ".after-packager", 1
    while posixpath.basename(new) in have:
        k += 1
        new = "%s.after-packager-%d" % (cur, k)
    if "default.xbe" in have:
        x.rename_file(cur, new)
    x.rename_file(old, cur)
    return new
