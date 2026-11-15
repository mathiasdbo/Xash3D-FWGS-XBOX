"""XS.6: the packager's net under saved games (r3dpack/savenet.py and r3d_packager.upload)."""
import functools
import hashlib
import os
import struct
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from r3dpack import savenet, xboxftp, xbepatch  # noqa: E402

try:
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler
    from pyftpdlib.servers import FTPServer
except ImportError:
    FTPServer = None

BASE = 0x10000


def make_xbe(sections):
    """a small XBE with real section headers: [(name, bytes)]"""
    d = bytearray(0x1000)
    d[0:4] = b"XBEH"
    struct.pack_into("<I", d, 0x104, BASE)
    struct.pack_into("<I", d, 0x108, 0xA00)  # the headers end here; the rest of the page is free for the logo and the image sections
    struct.pack_into("<I", d, 0x118, BASE + 0x178)
    struct.pack_into("<I", d, 0x11C, len(sections))
    struct.pack_into("<I", d, 0x120, BASE + 0x400)
    struct.pack_into("<II", d, 0x170, BASE + 0x300, 40)
    names = 0x800
    raw = bytearray()
    for i, (name, data) in enumerate(sections):
        nb = name.encode("latin-1") + b"\0"
        d[names:names + len(nb)] = nb
        ra = 0x1000 + len(raw)
        struct.pack_into("<6I", d, 0x400 + i * 0x38, 0, 0x20000 + i * 0x10000, len(data), ra, len(data), BASE + names)
        names += len(nb)
        raw += data
    return bytes(d) + bytes(raw)


CODE = [(".text", b"\x90" * 100), (".rdata", b"R" * 50), (".data", b"D" * 20)]


class CodeId(unittest.TestCase):
    def test_the_header_patches_and_image_sections_do_not_change_it(self):
        xbe = make_xbe(CODE)
        a = savenet.code_id(xbe)
        patched = xbepatch.set_title(xbe, "Half-Life")
        self.assertNotEqual(patched, xbe)
        self.assertEqual(savenet.code_id(patched), a)
        with open(os.path.join(os.path.dirname(__file__), "..", "assets", "boot-logo.pgm"), "rb") as f:
            patched = xbepatch.set_logo(patched, f.read())
        patched = xbepatch.add_sections(patched, [("$$XTIMAGE", b"x" * 3000), ("$$XSIMAGE", b"y" * 1000)])
        self.assertIn("$$XTIMAGE", [s["name"] for s in xbepatch.list_sections(patched)])
        self.assertEqual(savenet.code_id(patched), a)

    def test_a_changed_byte_of_code_changes_it(self):
        other = [(".text", b"\x90" * 99 + b"\x91")] + CODE[1:]
        self.assertNotEqual(savenet.code_id(make_xbe(other)), savenet.code_id(make_xbe(CODE)))

    def test_not_an_xbe_is_an_error(self):
        with self.assertRaises(Exception):
            savenet.code_id(b"nope")

    def test_classify(self):
        pub = savenet.PUBLIC_BUILDS["v0.2.0"]
        self.assertEqual(savenet.classify(pub, "x" * 64), "public")
        self.assertEqual(savenet.classify("a" * 64, "a" * 64), "same")
        self.assertEqual(savenet.classify("a" * 64, "b" * 64), "unknown")
        self.assertEqual(savenet.label_of(pub), "v0.2.0")
        self.assertTrue(savenet.label_of("a" * 64).startswith("build-aaaaaaaa"))

    def test_the_public_ids_are_distinct_and_64_hex(self):
        ids = list(savenet.PUBLIC_BUILDS.values())
        self.assertEqual(len(set(ids)), 3)
        for i in ids:
            self.assertRegex(i, r"^[0-9a-f]{64}$")


class Fake:
    """what backup_saves needs from the console, with files that can come back short"""

    def __init__(self, files, short=None, drives=("HDD0-E",), deny=None):
        self.files, self.short, self.drives, self.deny = files, short, drives, deny

    def listdir_strict(self, p):
        if self.deny and self.deny in p:
            raise RuntimeError("550 permission denied")
        if p == "/":
            return {d: (True, 0) for d in self.drives}
        if not p.startswith("/HDD0-E"):
            return {}  # the saves are on E: only
        chain = {"UDATA": "FFFF0002", "FFFF0002": "valve", "valve": "valve"}
        last = p.rstrip("/").rsplit("/", 1)[1]
        if last.startswith("HDD0"):
            return {"UDATA": (True, 0)}
        if last in chain and not p.endswith("/valve/valve"):
            return {chain[last]: (True, 0)}
        if p.endswith("/valve/valve"):
            return {"save": (True, 0)}
        if p.endswith("/valve/valve/save"):
            return {n: (False, len(d)) for n, d in self.files.items()}
        return {}

    def read_file(self, p):
        n = p.rsplit("/", 1)[1]
        d = self.files[n]
        return d[:-1] if n == self.short else d


class Backup(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dest = os.path.join(self.tmp.name, "backups")

    def tearDown(self):
        self.tmp.cleanup()

    def test_copies_every_file_with_a_manifest(self):
        files = {"quick.sav": b"A" * 100, "quick.bmp": b"B" * 50, "c1a1.HL1": b"C"}
        folder, n, total = savenet.backup_saves(Fake(files), "valve", self.dest, "v0.2.0")
        self.assertEqual((n, total), (3, 151))
        self.assertTrue(os.path.basename(folder).endswith("-v0.2.0"))
        for name, data in files.items():
            with open(os.path.join(folder, name), "rb") as f:
                self.assertEqual(f.read(), data)
        m = savenet.read_manifest(folder)
        self.assertEqual(m["files"]["quick.sav"]["sha256"], hashlib.sha256(files["quick.sav"]).hexdigest())
        self.assertFalse([d for d in os.listdir(self.dest) if d.endswith(".partial")])

    def test_a_short_file_stops_it_and_leaves_no_backup(self):
        with self.assertRaises(savenet.SaveNetError) as cm:
            savenet.backup_saves(Fake({"a.sav": b"A" * 10, "b.sav": b"B" * 10}, short="b.sav"), "valve", self.dest, "v0.2.0")
        self.assertIn("not whole", str(cm.exception))
        self.assertEqual([d for d in os.listdir(self.dest) if not d.endswith(".partial")], [])

    def test_an_odd_file_name_is_refused(self):
        with self.assertRaises(savenet.SaveNetError):
            savenet.backup_saves(Fake({"..": b"x"}), "valve", self.dest, "v0.2.0")
        with self.assertRaises(savenet.SaveNetError):
            savenet.backup_saves(Fake({"a;b.sav": b"x"}), "valve", self.dest, "v0.2.0")

    def test_no_saves_is_none_and_creates_nothing(self):
        self.assertIsNone(savenet.backup_saves(Fake({}), "valve", self.dest, "v0.2.0"))
        self.assertFalse(os.path.exists(self.dest))

    def test_a_listing_that_fails_is_not_taken_for_no_saves(self):
        # the account may not list UDATA: that is "could not look", and nothing may be replaced on the strength of it
        for deny in ("UDATA", "FFFF0002", "/save"):
            with self.assertRaises(savenet.SaveNetError, msg=deny):
                savenet.backup_saves(Fake({"a.sav": b"A"}, deny=deny), "valve", self.dest, "v0.2.0")
        self.assertFalse(os.path.exists(self.dest))

    def test_a_console_with_no_e_drive_listed_has_conclusively_no_saves_there(self):
        self.assertIsNone(savenet.backup_saves(Fake({"a.sav": b"A"}, drives=("HDD0-C", "HDD0-F")), "valve", self.dest, "v0.2.0"))

    def test_a_drive_that_will_not_list_other_than_e_is_not_a_reason_to_stop(self):
        class F2(Fake):
            def listdir_strict(self, p):
                if p.startswith("/HDD0-Z"):
                    raise RuntimeError("550 not mounted")
                return Fake.listdir_strict(self, p)
        folder, n, _ = savenet.backup_saves(F2({"a.sav": b"A"}, drives=("HDD0-E", "HDD0-Z")), "valve", self.dest, "v0.2.0")
        self.assertEqual(n, 1)

    def test_it_never_overwrites_a_backup(self):
        import datetime
        now = datetime.datetime(2026, 11, 15, 12, 0, 0)
        savenet.backup_saves(Fake({"a.sav": b"A"}), "valve", self.dest, "v0.2.0", now=now)
        with self.assertRaises(savenet.SaveNetError):
            savenet.backup_saves(Fake({"a.sav": b"A"}), "valve", self.dest, "v0.2.0", now=now)


@unittest.skipUnless(FTPServer, "pyftpdlib not installed")
class OnAConsole(unittest.TestCase):
    """a real FTP server laid out like the console: games on F:, saves on E:"""

    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.saves = os.path.join(self.root, "E", "UDATA", "FFFF0002", "valve", "valve", "save")
        os.makedirs(self.saves)
        self.base = os.path.join(self.root, "F", "Games", "Half-Life")
        os.makedirs(os.path.join(self.base, "valve"))
        for n, d in (("quick.sav", b"S" * 300), ("quick.bmp", b"M" * 40)):
            with open(os.path.join(self.saves, n), "wb") as f:
                f.write(d)
        with open(os.path.join(self.base, "default.xbe"), "wb") as f:
            f.write(make_xbe(CODE + [(".extra", b"e")]))  # an installed build that is not a public one
        a = DummyAuthorizer()
        a.add_user("xbox", "xbox", self.root, perm="elradfmwMT")
        h = FTPHandler
        h.authorizer = a
        self.srv = FTPServer(("127.0.0.1", 0), h)
        self.port = self.srv.address[1]
        threading.Thread(target=self.srv.serve_forever, kwargs={"timeout": 0.1}, daemon=True).start()
        self.dest = os.path.join(tempfile.mkdtemp(), "bk")
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
        import r3d_packager
        self.pk = r3d_packager

    def tearDown(self):
        self.srv.close_all()

    def snapshot(self):
        out = {}
        for d, _, fs in os.walk(self.saves):
            for f in fs:
                p = os.path.join(d, f)
                with open(p, "rb") as fh:
                    out[f] = hashlib.sha256(fh.read()).hexdigest()
        return out

    def files(self):
        new = make_xbe(CODE)
        return [("default.xbe", new, len(new)), ("valve/new.txt", b"n", 1)]

    def run_upload(self, confirm, logs=None):
        with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
            return self.pk.upload(self.files(), "hl", "127.0.0.1", "/F/Games", "xbox", "xbox", None,
                                  (logs.append if logs is not None else (lambda m: None)), confirm=confirm, backup_dir=self.dest)

    def test_replacing_the_build_copies_the_saves_first_and_touches_none(self):
        before = self.snapshot()
        asked, logs = [], []
        self.run_upload(lambda t: asked.append(t) or True, logs)
        self.assertEqual(self.snapshot(), before)  # the console's saves are exactly as they were
        self.assertEqual(sorted(os.listdir(self.saves)), ["quick.bmp", "quick.sav"])  # nothing added or removed
        (folder,) = os.listdir(self.dest)
        self.assertIn("build-", folder)  # an installed build that is not public is named by its hash
        with open(os.path.join(self.dest, folder, "quick.sav"), "rb") as f:
            self.assertEqual(f.read(), b"S" * 300)
        self.assertIn("not one of the public builds", asked[0])
        self.assertIn("copied to this PC first", asked[0])
        self.assertTrue(any("Copied the console's saved games" in m for m in logs))
        self.assertEqual(open(os.path.join(self.base, "default.xbe.before-packager"), "rb").read()[:4], b"XBEH")

    def test_nothing_is_replaced_when_the_copy_cannot_be_made(self):
        old = open(os.path.join(self.base, "default.xbe"), "rb").read()
        with mock.patch.object(savenet, "backup_saves", side_effect=savenet.SaveNetError("disk full")):
            with self.assertRaises(xboxftp.UploadError) as cm:
                self.run_upload(lambda t: True)
        self.assertIn("nothing was replaced", str(cm.exception))
        self.assertEqual(open(os.path.join(self.base, "default.xbe"), "rb").read(), old)
        self.assertFalse(os.path.exists(os.path.join(self.base, "default.xbe.before-packager")))

    def test_cancelling_copies_nothing(self):
        with self.assertRaises(xboxftp.UploadError):
            self.run_upload(lambda t: False)
        self.assertFalse(os.path.exists(self.dest))

    def test_restore_saves_adds_and_never_deletes(self):
        with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
            self.run_upload(lambda t: True)
        (folder,) = os.listdir(self.dest)
        backup = os.path.join(self.dest, folder)
        # the game on the console moved on: one save changed, one was made since
        with open(os.path.join(self.saves, "quick.sav"), "wb") as f:
            f.write(b"CHANGED")
        with open(os.path.join(self.saves, "newer.sav"), "wb") as f:
            f.write(b"N")
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        try:
            n = savenet.restore_saves(x, backup, "/E/UDATA/FFFF0002/valve/valve/save")
        finally:
            x.close()
        self.assertEqual(n, 2)
        self.assertEqual(open(os.path.join(self.saves, "quick.sav"), "rb").read(), b"S" * 300)
        self.assertEqual(open(os.path.join(self.saves, "newer.sav"), "rb").read(), b"N")  # not deleted
        # the console's own copy of the save the backup replaced is kept beside it, and no temporary file is left
        self.assertEqual(open(os.path.join(self.saves, "quick.sav.before-restore"), "rb").read(), b"CHANGED")
        self.assertFalse([n for n in os.listdir(self.saves) if n.endswith(".restoring")])

    def test_an_interrupted_restore_leaves_the_consoles_save_whole(self):
        with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
            self.run_upload(lambda t: True)
        (folder,) = os.listdir(self.dest)
        with open(os.path.join(self.saves, "quick.sav"), "wb") as f:
            f.write(b"THE CONSOLES OWN")
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        real = x.write_file

        def cut(path, data):
            real(path, data[: len(data) // 2])  # half of it arrives, then the network goes
            raise OSError("connection reset")
        x.write_file = cut
        try:
            with self.assertRaises(OSError):
                savenet.restore_saves(x, os.path.join(self.dest, folder), "/E/UDATA/FFFF0002/valve/valve/save")
        finally:
            x.close()
        self.assertEqual(open(os.path.join(self.saves, "quick.sav"), "rb").read(), b"THE CONSOLES OWN")

    def test_a_restore_that_arrives_short_is_not_put_in_place(self):
        with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
            self.run_upload(lambda t: True)
        (folder,) = os.listdir(self.dest)
        with open(os.path.join(self.saves, "quick.sav"), "wb") as f:
            f.write(b"THE CONSOLES OWN")
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        real = x.write_file
        x.write_file = lambda path, data: real(path, data[:-1])  # the transfer "succeeds" but one byte is missing
        try:
            with self.assertRaises(savenet.SaveNetError) as cm:
                savenet.restore_saves(x, os.path.join(self.dest, folder), "/E/UDATA/FFFF0002/valve/valve/save")
        finally:
            x.close()
        self.assertIn("did not arrive whole", str(cm.exception))
        self.assertEqual(open(os.path.join(self.saves, "quick.sav"), "rb").read(), b"THE CONSOLES OWN")

    def test_an_unlistable_udata_stops_the_upload_before_anything_is_replaced(self):
        old = open(os.path.join(self.base, "default.xbe"), "rb").read()
        real = xboxftp.Xbox.listdir_strict

        def deny(x, path="/"):
            if "UDATA" in path:
                raise xboxftp.UploadError("550 access denied")
            return real(x, path)
        with mock.patch.object(xboxftp.Xbox, "listdir_strict", deny):
            with self.assertRaises(xboxftp.UploadError) as cm:
                self.run_upload(lambda t: True)
        self.assertIn("nothing was replaced", str(cm.exception))
        self.assertEqual(open(os.path.join(self.base, "default.xbe"), "rb").read(), old)
        self.assertFalse(os.path.exists(os.path.join(self.base, "default.xbe.before-packager")))

    def test_going_back_after_two_replacements_restores_the_build_before_the_last_one(self):
        build_a = open(os.path.join(self.base, "default.xbe"), "rb").read()
        self.run_upload(lambda t: True)                       # A -> B
        build_b = open(os.path.join(self.base, "default.xbe"), "rb").read()
        self.assertNotEqual(build_a, build_b)
        new_c = make_xbe(CODE + [(".c", b"c")])
        with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
            self.pk.upload([("default.xbe", new_c, len(new_c))], "hl", "127.0.0.1", "/F/Games", "xbox", "xbox", None,
                           lambda m: None, confirm=lambda t: True, backup_dir=self.dest)   # B -> C
        self.assertEqual(open(os.path.join(self.base, "default.xbe"), "rb").read(), new_c)
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        try:
            savenet.restore_previous_build(x, "/F/Games/Half-Life")
        finally:
            x.close()
        self.assertEqual(open(os.path.join(self.base, "default.xbe"), "rb").read(), build_b)  # B, not A

    def test_restore_refuses_a_backup_that_does_not_match_its_manifest(self):
        with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
            self.run_upload(lambda t: True)
        (folder,) = os.listdir(self.dest)
        with open(os.path.join(self.dest, folder, "quick.sav"), "ab") as f:
            f.write(b"x")
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        try:
            with self.assertRaises(savenet.SaveNetError):
                savenet.restore_saves(x, os.path.join(self.dest, folder), "/E/UDATA/FFFF0002/valve/valve/save")
        finally:
            x.close()
        self.assertEqual(open(os.path.join(self.saves, "quick.sav"), "rb").read(), b"S" * 300)

    def test_going_back_to_the_previous_build_renames_and_deletes_nothing(self):
        self.run_upload(lambda t: True)
        installed = open(os.path.join(self.base, "default.xbe"), "rb").read()
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        try:
            moved = savenet.restore_previous_build(x, "/F/Games/Half-Life")
        finally:
            x.close()
        self.assertTrue(moved.endswith("default.xbe.after-packager"))
        self.assertEqual(open(os.path.join(self.base, "default.xbe.after-packager"), "rb").read(), installed)
        self.assertNotEqual(open(os.path.join(self.base, "default.xbe"), "rb").read(), installed)
        self.assertFalse(os.path.exists(os.path.join(self.base, "default.xbe.before-packager")))

    def test_the_restore_command_puts_back_the_build_and_the_saves(self):
        import argparse
        import contextlib
        import io
        self.run_upload(lambda t: True)
        (folder,) = os.listdir(self.dest)
        with open(os.path.join(self.saves, "quick.sav"), "wb") as f:
            f.write(b"CHANGED")
        installed = open(os.path.join(self.base, "default.xbe"), "rb").read()
        a = argparse.Namespace(upload="127.0.0.1", ftp_user="xbox", ftp_password="xbox", title="hl",
                               remote_dir="/F/Games", restore_build=True, restore_saves=os.path.join(self.dest, folder))
        out = io.StringIO()
        with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
            with contextlib.redirect_stdout(out):
                self.assertEqual(self.pk.restore(a), 0)
        self.assertIn("nothing was deleted", out.getvalue())
        self.assertEqual(open(os.path.join(self.saves, "quick.sav"), "rb").read(), b"S" * 300)
        self.assertEqual(open(os.path.join(self.base, "default.xbe.after-packager"), "rb").read(), installed)

    def failing_second_rename(self, x):
        """the second rename of a swap fails, as a dropped connection would"""
        real, calls = x.rename_file, []

        def rename(old, new):
            calls.append((old, new))
            if len(calls) == 2:
                raise OSError("connection reset")
            real(old, new)
        x.rename_file = rename
        return calls

    def test_a_save_swap_that_fails_half_way_puts_the_original_back(self):
        with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
            self.run_upload(lambda t: True)
        (folder,) = os.listdir(self.dest)
        with open(os.path.join(self.saves, "quick.sav"), "wb") as f:
            f.write(b"THE CONSOLES OWN")
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        self.failing_second_rename(x)
        try:
            with self.assertRaises(savenet.SaveNetError) as cm:
                savenet.restore_saves(x, os.path.join(self.dest, folder), "/E/UDATA/FFFF0002/valve/valve/save")
        finally:
            x.close()
        self.assertIn("the original was put back", str(cm.exception))
        self.assertEqual(open(os.path.join(self.saves, "quick.sav"), "rb").read(), b"THE CONSOLES OWN")

    def test_a_build_swap_that_fails_half_way_leaves_default_xbe_in_place(self):
        self.run_upload(lambda t: True)
        installed = open(os.path.join(self.base, "default.xbe"), "rb").read()
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        self.failing_second_rename(x)
        try:
            with self.assertRaises(savenet.SaveNetError):
                savenet.restore_previous_build(x, "/F/Games/Half-Life")
        finally:
            x.close()
        self.assertEqual(open(os.path.join(self.base, "default.xbe"), "rb").read(), installed)  # never absent

    def test_a_swap_that_cannot_be_undone_says_where_every_file_is(self):
        self.run_upload(lambda t: True)
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        real, calls = x.rename_file, []

        def rename(old, new):
            calls.append(1)
            if len(calls) >= 2:
                raise OSError("connection lost")
            real(old, new)
        x.rename_file = rename
        try:
            with self.assertRaises(savenet.SaveNetError) as cm:
                savenet.restore_previous_build(x, "/F/Games/Half-Life")
        finally:
            x.close()
        msg = str(cm.exception)
        self.assertIn("back either", msg)
        self.assertIn("the original is under the name", msg)
        self.assertIn("default.xbe.after-packager", msg)

    def test_with_both_options_a_failing_saves_restore_leaves_the_build_alone(self):
        import argparse
        import contextlib
        import io
        self.run_upload(lambda t: True)
        (folder,) = os.listdir(self.dest)
        backup = os.path.join(self.dest, folder)
        with open(os.path.join(backup, "quick.sav"), "ab") as f:
            f.write(b"x")  # no longer matches its manifest
        installed = open(os.path.join(self.base, "default.xbe"), "rb").read()
        a = argparse.Namespace(upload="127.0.0.1", ftp_user="xbox", ftp_password="xbox", title="hl", remote_dir="/F/Games",
                               restore_build=True, restore_saves=backup)
        with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(self.pk.restore(a), 1)
        self.assertEqual(open(os.path.join(self.base, "default.xbe"), "rb").read(), installed)
        self.assertTrue(os.path.exists(os.path.join(self.base, "default.xbe.before-packager")))

    def test_the_restore_command_needs_a_console_and_a_games_folder(self):
        import argparse
        import contextlib
        import io
        a = argparse.Namespace(upload=None, ftp_user="xbox", ftp_password="xbox", title="hl", remote_dir=None,
                               restore_build=True, restore_saves=None)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(self.pk.restore(a), 2)
            a.upload = "127.0.0.1"
            with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
                self.assertEqual(self.pk.restore(a), 2)


if __name__ == "__main__":
    unittest.main()
