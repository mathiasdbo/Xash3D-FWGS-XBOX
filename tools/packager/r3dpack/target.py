"""Where the Console settings screen reads and writes: an install on the console
(over FTP) or a folder on this PC (the one an export wrote). Paths are relative
to the install folder, like valve/userconfig.d/50-packager.cfg.
"""

import os
import posixpath

from . import consolecfg, xboxftp

TITLE_ID = "FFFF0002"  # the title id cxbe gives the build; its save folder is UDATA/<id>/<gamedir>/<gamedir>


class TargetError(Exception):
    pass


class _Base:
    def userconfig_files(self, gamedir):
        """[(name, enabled, size)] for the .cfg and .cfg.off files of userconfig.d, by name."""
        out = []
        for name, (is_dir, size) in sorted(self.listdir("%s/userconfig.d" % gamedir).items(), key=lambda t: t[0].lower()):
            low = name.lower()
            if is_dir:
                continue
            if low.endswith(".cfg"):
                out.append((name, True, size))
            elif low.endswith(".cfg.off"):
                out.append((name, False, size))
        return out

    def run_order(self, gamedir):
        """[(name, text)]: the live config.cfg when it can be read, then the enabled userconfig.d files."""
        files = []
        live = self.read_live_config(gamedir)
        if live is not None:
            files.append(("config.cfg (menu)", live.decode("latin-1")))
        for name, enabled, _ in self.userconfig_files(gamedir):
            if enabled:
                data = self.read("%s/userconfig.d/%s" % (gamedir, name))
                files.append((name, (data or b"").decode("latin-1")))
        return files

    def toggle(self, gamedir, name, enable):
        """Rename name.cfg <-> name.cfg.off. Never overwrites: if the other name is taken it stops."""
        d = "%s/userconfig.d/" % gamedir
        cur, new = (name + ".off", name) if enable else (name, name + ".off")
        if self.read(d + new) is not None:
            raise TargetError("%s already exists; not overwriting it" % new)
        self.rename(d + cur, d + new)

    def save_with_backup(self, rel, data):
        """Write rel; the file it replaces is kept as rel.before-packager (once, the first time)."""
        old = self.read(rel)
        if old is not None and old != data and self.read(rel + ".before-packager") is None:
            self.write(rel + ".before-packager", old)
        self.write(rel, data)


class FtpTarget(_Base):
    def __init__(self, host, install, user=xboxftp.DEFAULT_USER, password=xboxftp.DEFAULT_PASS, port=21):
        self.x = xboxftp.Xbox(host, user, password, port=port)
        self.install = install.rstrip("/")
        self.host = host

    def open(self):
        self.x.connect()

    def close(self):
        self.x.close()

    def _p(self, rel):
        return posixpath.join(self.install, rel)

    def read(self, rel):
        return self.x.read_file(self._p(rel))

    def write(self, rel, data):
        self.x.write_file(self._p(rel), data)

    def rename(self, old, new):
        self.x.rename_file(self._p(old), self._p(new))

    def listdir(self, rel):
        return self.x.listdir(self._p(rel))

    def read_live_config(self, gamedir):
        """The config.cfg the menu writes, in the save folder on whichever drive has it."""
        for drive, (is_dir, _) in sorted(self.x.listdir("/").items()):
            if not is_dir:
                continue
            p = "/%s/UDATA/%s/%s/%s/config.cfg" % (drive, TITLE_ID, gamedir, gamedir)
            data = self.x.read_file(p)
            if data is not None:
                return data
        return None


class FolderTarget(_Base):
    def __init__(self, root):
        self.root = root

    def open(self):
        if not os.path.isdir(self.root):
            raise TargetError("%s is not a folder" % self.root)

    def close(self):
        pass

    def _p(self, rel):
        return os.path.join(self.root, *rel.split("/"))

    def read(self, rel):
        try:
            with open(self._p(rel), "rb") as f:
                return f.read()
        except OSError:
            return None

    def write(self, rel, data):
        p = self._p(rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    def rename(self, old, new):
        os.rename(self._p(old), self._p(new))

    def listdir(self, rel):
        p = self._p(rel)
        out = {}
        try:
            for n in os.listdir(p):
                full = os.path.join(p, n)
                out[n] = (os.path.isdir(full), 0 if os.path.isdir(full) else os.path.getsize(full))
        except OSError:
            pass
        return out

    def read_live_config(self, gamedir):
        return None  # a folder on the PC has no save folder of its own


def load_state(target, gamedir):
    """Everything the screen shows: the managed file's values, the effective settings, the files."""
    text = target.read("%s/userconfig.d/%s" % (gamedir, consolecfg.MANAGED_NAME))
    values, startup = consolecfg.read_managed(text.decode("latin-1")) if text else ({}, {})
    return {
        "managed_text": text.decode("latin-1") if text else None,
        "values": values,
        "startup": startup,
        "effective": consolecfg.Effective(target.run_order(gamedir)),
        "files": target.userconfig_files(gamedir),
    }


def write_settings(target, gamedir, values, startup, toggles=None):
    """Write 50-packager.cfg (an empty choice removes the file's lines, not the file) and apply
    the on/off changes {file name: enable}. Returns the text written."""
    text = consolecfg.render(values, startup)
    target.save_with_backup("%s/userconfig.d/%s" % (gamedir, consolecfg.MANAGED_NAME), text.encode("latin-1"))
    for name, enable in (toggles or {}).items():
        base = name[:-4] if name.lower().endswith(".off") else name
        target.toggle(gamedir, base, enable)
    return text
