"""Find an Xbox on the network and upload a packaged game to it over FTP.

Only the standard library. The Xbox FTP servers (Evolution-X and friends) are
slow and old: one connection at a time, passive mode, plain LIST (no MLSD),
and a root that lists drives (C, E, F ... or HDD0-E ...). Nothing here assumes
more than that.
"""

import ftplib
import io
import posixpath
import re
import socket
import threading
from concurrent.futures import ThreadPoolExecutor

DEFAULT_USER = "xbox"
DEFAULT_PASS = "xbox"


class UploadError(Exception):
    pass


# The console's own executable: an upload that replaces it keeps the old one beside it.
BACKUP_FILES = ("default.xbe",)

# Files that hold the user's own state on an install that is already there. They are left as
# they are: the packager's copies come from the PC's Half-Life folder, not from the console.
_SETTINGS_NAMES = {"config.cfg", "config.cfg.bak", "rt.cfg", "custom.hpk", "autoexec.cfg", "userconfig.cfg"}


def is_user_setting(rel):
    """rel is a path like valve/config.cfg or valve/userconfig.d/00-x.cfg."""
    parts = rel.lower().split("/")
    if len(parts) == 2 and parts[1] in _SETTINGS_NAMES:
        return True
    return len(parts) >= 3 and parts[1] in ("userconfig.d", "save")


class UploadPlan:
    """What survey() found. needs_confirmation is true when something would be replaced."""

    def __init__(self, exists):
        self.exists = exists
        self.new, self.replace, self.keep = [], [], []
        self.force = set()
        self.same = 0
        self.xbe_differs = False

    @property
    def needs_confirmation(self):
        return self.exists and bool(self.replace)

    def describe(self, where):
        if not self.exists:
            return "%s is a new folder; nothing there is replaced." % where
        lines = ["%s is already on the console." % where]
        if self.xbe_differs:
            lines.append("- default.xbe is a different build. The old one is kept as default.xbe.before-packager.")
        others = [r for r in self.replace if r not in BACKUP_FILES]
        if others:
            lines.append("- %d other file(s) would be replaced: %s%s" % (
                len(others), ", ".join(others[:6]), ", ..." if len(others) > 6 else ""))
        lines.append("- %d new file(s), %d already identical." % (len(self.new), self.same))
        if self.keep:
            lines.append("- %d settings file(s) of yours stay as they are (config.cfg, userconfig.d ...)." % len(self.keep))
        return "\n".join(lines)


# ---- finding the console ----

def local_prefix():
    """The first three octets of this machine's LAN address, or None."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # no packet is sent
        ip = s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()
    return ".".join(ip.split(".")[:3]) if not ip.startswith("127.") else None


def _port_open(ip, port, timeout):
    s = socket.socket()
    s.settimeout(timeout)
    try:
        return s.connect_ex((ip, port)) == 0
    finally:
        s.close()


def scan(prefix=None, port=21, timeout=0.4, user=DEFAULT_USER, password=DEFAULT_PASS, progress=None):
    """Hosts on the /24 that answer on the FTP port and accept the login."""
    prefix = prefix or local_prefix()
    if not prefix:
        return []
    ips = ["%s.%d" % (prefix, i) for i in range(1, 255)]
    with ThreadPoolExecutor(64) as pool:
        open_ips = [ip for ip, ok in zip(ips, pool.map(lambda i: _port_open(i, port, timeout), ips)) if ok]
    found = []
    for ip in open_ips:
        try:
            f = ftplib.FTP()
            f.connect(ip, port, timeout=4)
            f.login(user, password)
            found.append(ip)
            f.quit()
        except (OSError, ftplib.Error):
            pass
        if progress:
            progress(ip)
    return found


# ---- listing ----

_UNIX = re.compile(r"^([-dl])\S*\s+\d+\s+\S+\s+\S+\s+(\d+)\s+\w+\s+\d+\s+[\d:]+\s+(.+)$")
_UNIX2 = re.compile(r"^([-dl])\S*\s+\S+\s+\S+\s+\S+\s+(\d+)\s+\S+\s+\S+\s+\S+\s+(.+)$")
_DOS = re.compile(r"^\d\d-\d\d-\d\d\s+\d\d:\d\d[AP]M\s+(<DIR>|\d+)\s+(.+)$")


def parse_listing(lines):
    """-> {name: (is_dir, size)} from LIST output in Unix or DOS style."""
    out = {}
    for line in lines:
        line = line.rstrip("\r\n")
        m = _DOS.match(line)
        if m:
            d = m.group(1) == "<DIR>"
            out[m.group(2)] = (d, 0 if d else int(m.group(1)))
            continue
        m = _UNIX.match(line) or _UNIX2.match(line)
        if m and m.group(3) not in (".", ".."):
            out[m.group(3)] = (m.group(1) == "d", int(m.group(2)))
    return out


class Xbox:
    def __init__(self, host, user=DEFAULT_USER, password=DEFAULT_PASS, port=21, timeout=30):
        self.host, self.user, self.password, self.port, self.timeout = host, user, password, port, timeout
        self.ftp = None

    def connect(self):
        self.close()
        f = ftplib.FTP()
        try:
            f.connect(self.host, self.port, timeout=self.timeout)
            f.login(self.user, self.password)
        except (OSError, ftplib.Error) as e:
            raise UploadError("Could not log in to %s: %s" % (self.host, e))
        f.set_pasv(True)
        self.ftp = f

    def close(self):
        if self.ftp:
            try:
                self.ftp.quit()
            except (OSError, ftplib.Error):
                pass
            self.ftp = None

    def listdir(self, path="/"):
        lines = []
        try:
            self.ftp.retrlines("LIST " + path if path != "/" else "LIST", lines.append)
        except ftplib.error_perm:
            return {}
        return parse_listing(lines)

    def find_game_folders(self):
        """Where a game can go, best first, as (path, exists, is_big_drive).

        C is the system partition and X, Y, Z are the dashboard's cache
        partitions: too small, and not for games. F and G are the extended
        partitions of a big drive; E is the main data partition, smaller but
        what a stock 8 GB drive offers. A drive with no Games folder is
        offered too (it would be created), so a fresh F: still shows up.
        """
        out = []
        for drive, (is_dir, _) in sorted(self.listdir("/").items()):
            if not is_dir:
                continue
            letter = drive.upper()[-1:]
            if letter in ("C", "X", "Y", "Z") or letter < "E":
                continue
            games = None
            for name, (d, _) in self.listdir("/" + drive).items():
                if d and name.lower() == "games":
                    games = name
            path = "/%s/%s" % (drive, games or "Games")
            out.append((path, games is not None, letter != "E"))
        out.sort(key=lambda t: (not t[2], t[0]))
        return out

    def _mkdirs(self, path, made):
        if path in made or path in ("", "/"):
            return
        self._mkdirs(posixpath.dirname(path), made)
        try:
            self.ftp.mkd(path)
        except ftplib.error_perm:
            pass  # exists
        made.add(path)

    def exists(self, path):
        parent, name = posixpath.split(path.rstrip("/"))
        return name in self.listdir(parent or "/")

    def remove_stale_bsp(self, files, remote_dir):
        """Delete the loose .bsp of every map the upload carries as a baked .mpak.

        An earlier upload may have left them, and a .bsp beside its archive is loaded in its
        place (the world then draws black). Only those files are touched. Returns the names."""
        gamedirs = {}
        for rel, _, _ in files:
            parts = rel.split("/")
            if len(parts) == 2 and parts[1].lower().endswith(".mpak"):
                gamedirs.setdefault(parts[0], set()).add(parts[1][:-5].lower())
        removed = []
        for gd, maps in gamedirs.items():
            mdir = posixpath.join(remote_dir, gd, "maps")
            for n, (is_dir, _) in self.listdir(mdir).items():
                if not is_dir and n.lower().endswith(".bsp") and n[:-4].lower() in maps:
                    try:
                        self.ftp.delete(posixpath.join(mdir, n))
                    except (OSError, EOFError, ftplib.Error) as e:
                        raise UploadError("Removing %s failed: %s" % (n, e))
                    removed.append(gd + "/maps/" + n)
        return removed

    def read_file(self, path):
        """The remote file's bytes, or None when it is not there."""
        buf = io.BytesIO()
        try:
            self.ftp.retrbinary("RETR " + path, buf.write)
        except ftplib.error_perm:
            return None
        return buf.getvalue()

    def write_file(self, path, data):
        self._mkdirs(posixpath.dirname(path), set())
        self.ftp.storbinary("STOR " + path, io.BytesIO(data))

    def rename_file(self, old, new):
        self.ftp.rename(old, new)

    def find_installs(self, games_dir, depth=2, needs=None):
        """Folders under games_dir that hold a default.xbe, as paths: Half-Life, or
        r3d-packager-test/Half-Life one level further down. needs names a folder the install must
        also have (valve), so the other games on the drive are not offered."""
        out = []
        for name, (is_dir, _) in sorted(self.listdir(games_dir).items()):
            if not is_dir:
                continue
            path = posixpath.join(games_dir, name)
            inner = self.listdir(path)
            if any(n.lower() == "default.xbe" and not d for n, (d, _) in inner.items()):
                if not needs or any(n.lower() == needs.lower() and d for n, (d, _) in inner.items()):
                    out.append(path)
            elif depth > 1:
                out += self.find_installs(path, depth - 1, needs)
        return out

    def _remote_equals(self, rpath, src):
        """True when the remote file has exactly the bytes of src (a path or bytes)."""
        buf = io.BytesIO()
        self.ftp.retrbinary("RETR " + rpath, buf.write)
        if isinstance(src, (bytes, bytearray)):
            return buf.getvalue() == bytes(src)
        with open(src, "rb") as fh:
            return buf.getvalue() == fh.read()

    def survey(self, files, remote_dir):
        """What an upload to remote_dir would do to an install that is already there.

        Nothing is changed. Returns an UploadPlan: files to add (new), to replace (replace),
        the same already (same), the user's settings left alone (keep), and the game's own
        executable when the console has a different one (xbe_differs)."""
        plan = UploadPlan(self.exists(remote_dir))
        if not plan.exists:
            plan.new = [rel for rel, _, _ in files]
            return plan
        cache = {}
        for rel, src, size in files:
            rdir = posixpath.join(remote_dir, posixpath.dirname(rel)).rstrip("/")
            if rdir not in cache:
                cache[rdir] = {n: s for n, (d, s) in self.listdir(rdir).items() if not d}
            have = cache[rdir].get(posixpath.basename(rel))
            if have is None:
                plan.new.append(rel)
            elif is_user_setting(rel):
                plan.keep.append(rel)
            elif rel in BACKUP_FILES and have == size and not self._remote_equals(posixpath.join(remote_dir, rel), src):
                plan.replace.append(rel)  # same size, other build: a size check alone would miss it
                plan.force.add(rel)
            elif have != size:
                plan.replace.append(rel)
            else:
                plan.same += 1
        plan.xbe_differs = any(r in BACKUP_FILES for r in plan.replace)
        return plan

    def _backup(self, rpath):
        """Rename a remote file out of the way (name.before-packager, then -2, -3 ...)."""
        have = self.listdir(posixpath.dirname(rpath))
        n, k = rpath + ".before-packager", 1
        while posixpath.basename(n) in have:
            k += 1
            n = "%s.before-packager-%d" % (rpath, k)
        self.ftp.rename(rpath, n)
        return n

    def upload(self, files, remote_dir, progress=None, skip_same_size=True, retries=3, plan=None):
        """files: (relative path, source path or bytes, size). Returns (sent, skipped).

        With a plan from survey(): the user's settings files already on the console are left
        alone, and the console's own default.xbe is renamed (default.xbe.before-packager)
        before a different one replaces it."""
        total = sum(s for _, _, s in files) or 1
        done = 0
        made = set()
        sent = skipped = 0
        remote_sizes = {}
        keep = set(plan.keep) if plan else set()
        force = set(plan.force) if plan else set()
        for rel, src, size in files:
            rdir = posixpath.join(remote_dir, posixpath.dirname(rel)).rstrip("/")
            rpath = posixpath.join(remote_dir, rel)
            for attempt in range(retries):
                try:
                    if rdir not in remote_sizes:
                        self._mkdirs(rdir, made)
                        remote_sizes[rdir] = {n: s for n, (d, s) in self.listdir(rdir).items() if not d}
                    have = remote_sizes[rdir].get(posixpath.basename(rel))
                    if rel in keep or (skip_same_size and have == size and rel not in force):
                        skipped += 1
                        break
                    if plan and rel in BACKUP_FILES and have is not None:
                        self._backup(rpath)
                        remote_sizes.clear()
                    if isinstance(src, (bytes, bytearray)):
                        self.ftp.storbinary("STOR " + rpath, io.BytesIO(src))
                    else:
                        with open(src, "rb") as fh:
                            self.ftp.storbinary("STOR " + rpath, fh, 64 * 1024)
                    sent += 1
                    break
                except (OSError, EOFError, ftplib.Error) as e:
                    if attempt == retries - 1:
                        raise UploadError("Uploading %s failed: %s" % (rel, e))
                    self.connect()  # the old connection is gone; start over on this file
                    remote_sizes.clear()
            done += size
            if progress:
                progress(done, total, rel)
        return sent, skipped
