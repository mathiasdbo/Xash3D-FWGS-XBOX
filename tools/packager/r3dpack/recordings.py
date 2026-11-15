"""Frame-time recordings on the console: look at them, copy them to this PC, remove them (docs/r3d/fork-plan.md, XS).

The recorder (engine/platform/xbox/xbox_frametime.c) writes frametime/ft_<label>_<session>_<seq>.bin under the game's save folder on E:.
Left on, it fills the drive: on 2026-11-15 a leftover userconfig.d file had turned it on at every boot, 1,296 files and 4.9 GB later E:
was full, and saved games loaded half restored (no weapons, no suit, enemies that did not think). These are the way out, with the
same rule as the saved games' backup: nothing is deleted from the console until it is on this PC and checked.

  * `summary` only lists;
  * `copy_to_pc` copies every recording to a dated folder (made as .partial, renamed when every file has the size the console
    listed, with a manifest of sizes and SHA-256s);
  * `remove_all` does that, then deletes from the console exactly the files it copied, by name, only those still the size they
    were copied at (a file that grew is being written and is left), and only names that are recordings (ft_*_<8 hex>_<3 digits>.bin).
    It never touches the saved games, the screenshots or anything else.

The engine has the same pair as console commands (host_frametime_clean keeps the newest session, host_frametime_clear removes all).
"""

import datetime
import hashlib
import json
import os
import posixpath
import re
import shutil

from . import savenet
from .savenet import SaveNetError

QUARANTINE = ".removing"  # a recording being removed: renamed out of the recorder's way first, still a recording if the run is cut
REC_NAME = re.compile(r"ft_[A-Za-z0-9_\-]+_[0-9a-f]{8}_\d{3}\.bin(?:\.removing)?$")
MANIFEST = "manifest.json"


def default_dest():
    return os.path.join(os.path.expanduser("~"), "Xash3D-FWGS-XBOX-recordings")


def list_recordings(x, gamedir):
    """(folder on the console or None, {name: size}) of the recordings. Only listings; a listing that fails raises."""
    src = savenet.find_save_dir(x, gamedir, leaf="frametime")
    if src is None:
        return None, {}
    try:
        entries = x.listdir_strict(src)
    except Exception as e:  # noqa: BLE001
        raise SaveNetError("could not list %s: %s" % (src, e))
    return src, {n: s for n, (d, s) in entries.items() if not d and REC_NAME.match(n)}


def summary(x, gamedir):
    src, files = list_recordings(x, gamedir)
    return {"folder": src, "files": len(files), "bytes": sum(files.values())}


def copy_to_pc(x, gamedir, dest_root, now=None, progress=None):
    """Copy every recording to dest_root/<date>-recordings/. Returns (folder, {name: size}) or None when there are none. Reads only."""
    src, files = list_recordings(x, gamedir)
    if not files:
        return None
    need = sum(files.values())
    os.makedirs(dest_root, exist_ok=True)
    if shutil.disk_usage(dest_root).free < need + (64 << 20):
        raise SaveNetError("this PC has less than %d MB free in %s: not copying" % ((need >> 20) + 64, dest_root))
    stamp = (now or datetime.datetime.now()).strftime("%Y%m%d-%H%M%S")
    final, k = os.path.join(dest_root, stamp + "-recordings"), 1
    while os.path.exists(final) or os.path.exists(final + ".partial"):
        k += 1  # a copy is never overwritten: a second one in the same second is numbered
        final = os.path.join(dest_root, "%s-recordings-%d" % (stamp, k))
    part = final + ".partial"
    os.makedirs(part)
    manifest = {"source": src, "files": {}}
    done = 0
    for name, size in sorted(files.items()):
        h = hashlib.sha256()
        got = 0
        tmp = os.path.join(part, name)
        with open(tmp, "wb") as f:
            def write(b):
                nonlocal got
                f.write(b)
                h.update(b)
                got += len(b)
            try:
                x.ftp.retrbinary("RETR " + posixpath.join(src, name), write, 256 * 1024)
            except Exception as e:  # noqa: BLE001
                raise SaveNetError("could not copy %s: %s" % (name, e))
        if got != size:
            raise SaveNetError("%s came back as %d bytes, the console lists %d: the copy is not whole" % (name, got, size))
        manifest["files"][name] = {"size": size, "sha256": h.hexdigest()}
        done += size
        if progress:
            progress(done, need, name)
    with open(os.path.join(part, MANIFEST), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    os.rename(part, final)
    return final, dict(files)


def remove_all(x, gamedir, dest_root, now=None, progress=None):
    """Copy every recording to this PC, check the copy, then delete from the console exactly what was copied. Returns
    (folder, removed, left, bytes freed); the saved games and everything else on the console are not touched."""
    got = copy_to_pc(x, gamedir, dest_root, now, progress)
    if got is None:
        return None
    folder, copied = got
    # verify the copy again from the disk, against the manifest, before anything is deleted
    with open(os.path.join(folder, MANIFEST), encoding="utf-8") as f:
        manifest = json.load(f)["files"]
    for name, meta in manifest.items():
        p = os.path.join(folder, name)
        if not os.path.isfile(p) or os.path.getsize(p) != meta["size"]:
            raise SaveNetError("the copy of %s on this PC does not match: nothing was deleted from the console" % name)
    src, now_files = list_recordings(x, gamedir)
    removed, left, freed = 0, [], 0
    # Phase 1: take each file out of the recorder's way by renaming it. A file the recorder has open should refuse the rename
    # (and is left); one that grows after the listing above is caught in phase 2, not deleted on the strength of an old size.
    claimed = []
    for name, size in sorted(copied.items()):
        if not REC_NAME.match(name):
            continue
        if now_files.get(name) != size:
            left.append(name)  # gone already, or grew since the copy: being written, not ours to delete
            continue
        if name.endswith(QUARANTINE):
            claimed.append((name, name, size))  # left by an interrupted run, already out of the way
            continue
        try:
            x.rename_file(posixpath.join(src, name), posixpath.join(src, name + QUARANTINE))
        except Exception:  # noqa: BLE001 - refused: in use, or the console would not
            left.append(name)
            continue
        claimed.append((name, name + QUARANTINE, size))
    # Phase 2: one fresh listing of the claimed files; delete those still exactly the size that was copied, put the others back.
    _, after = list_recordings(x, gamedir)
    for name, q, size in claimed:
        if after.get(q) == size:
            x.ftp.delete(posixpath.join(src, q))
            removed += 1
            freed += size
            continue
        left.append(name)  # it changed after the copy: not deleted, given its own name back
        if q != name:
            try:
                x.rename_file(posixpath.join(src, q), posixpath.join(src, name))
            except Exception:  # noqa: BLE001
                pass  # stays as <name>.removing: still a recording, and listed again by the next run
    return folder, removed, left, freed
