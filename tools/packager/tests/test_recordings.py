"""The frame-time recordings on the console: list, copy to the PC, remove (r3dpack/recordings.py, r3d_packager --recordings)."""
import argparse
import contextlib
import functools
import hashlib
import io
import os
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from r3dpack import recordings as rec, savenet, xboxftp  # noqa: E402

try:
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler
    from pyftpdlib.servers import FTPServer
except ImportError:
    FTPServer = None

NAMES = ["ft_play-v013_b96e1c1d_%03d.bin" % i for i in range(3)] + ["ft_bmi-720-perf8_f412ad98_%03d.bin" % i for i in range(2)]


@unittest.skipUnless(FTPServer, "pyftpdlib not installed")
class OnAConsole(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        game = os.path.join(self.root, "E", "UDATA", "FFFF0002", "valve", "valve")
        self.ft = os.path.join(game, "frametime")
        self.save = os.path.join(game, "save")
        self.shots = os.path.join(game, "scrshots")
        for d in (self.ft, self.save, self.shots):
            os.makedirs(d)
        for i, n in enumerate(NAMES):
            with open(os.path.join(self.ft, n), "wb") as f:
                f.write(bytes([i]) * (1000 + 100 * i))
        with open(os.path.join(self.ft, "notes.txt"), "wb") as f:  # not a recording: never touched
            f.write(b"keep me")
        with open(os.path.join(self.save, "quick.sav"), "wb") as f:
            f.write(b"S" * 500)
        with open(os.path.join(self.shots, "shot0000.bmp"), "wb") as f:
            f.write(b"B" * 300)
        a = DummyAuthorizer()
        a.add_user("xbox", "xbox", self.root, perm="elradfmwMT")
        h = FTPHandler
        h.authorizer = a
        self.srv = FTPServer(("127.0.0.1", 0), h)
        self.port = self.srv.address[1]
        threading.Thread(target=self.srv.serve_forever, kwargs={"timeout": 0.1}, daemon=True).start()
        self.dest = os.path.join(tempfile.mkdtemp(), "rec")
        self.x = xboxftp.Xbox("127.0.0.1", port=self.port)
        self.x.connect()

    def tearDown(self):
        self.x.close()
        self.srv.close_all()

    def snap(self, d):
        out = {}
        for n in sorted(os.listdir(d)):
            with open(os.path.join(d, n), "rb") as f:
                out[n] = hashlib.sha256(f.read()).hexdigest()
        return out

    def test_summary_counts_only_recordings(self):
        s = rec.summary(self.x, "valve")
        self.assertEqual(s["files"], 5)
        self.assertEqual(s["bytes"], sum(1000 + 100 * i for i in range(5)))

    def test_no_recordings_is_none_not_an_error(self):
        for n in NAMES:
            os.remove(os.path.join(self.ft, n))
        self.assertEqual(rec.summary(self.x, "valve")["files"], 0)
        self.assertIsNone(rec.copy_to_pc(self.x, "valve", self.dest))
        self.assertIsNone(rec.remove_all(self.x, "valve", self.dest))

    def test_copy_changes_nothing_on_the_console(self):
        before = self.snap(self.ft)
        folder, files = rec.copy_to_pc(self.x, "valve", self.dest)
        self.assertEqual(self.snap(self.ft), before)
        self.assertEqual(len(files), 5)
        for n in NAMES:
            with open(os.path.join(folder, n), "rb") as f, open(os.path.join(self.ft, n), "rb") as g:
                self.assertEqual(f.read(), g.read())
        self.assertTrue(os.path.exists(os.path.join(folder, rec.MANIFEST)))
        self.assertFalse([d for d in os.listdir(self.dest) if d.endswith(".partial")])

    def test_remove_copies_first_then_deletes_only_the_recordings(self):
        saves, shots = self.snap(self.save), self.snap(self.shots)
        folder, removed, left, freed = rec.remove_all(self.x, "valve", self.dest)
        self.assertEqual((removed, left), (5, []))
        self.assertEqual(freed, sum(1000 + 100 * i for i in range(5)))
        self.assertEqual(sorted(os.listdir(self.ft)), ["notes.txt"])  # the recordings are gone, the other file is not
        self.assertEqual(self.snap(self.save), saves)  # the saved games, untouched
        self.assertEqual(self.snap(self.shots), shots)
        self.assertEqual(len([n for n in os.listdir(folder) if n.endswith(".bin")]), 5)  # and they are on the PC

    def test_a_short_copy_stops_before_anything_is_deleted(self):
        real = self.x.ftp.retrbinary
        calls = []

        def short(cmd, cb, *a):
            calls.append(cmd)
            return real(cmd, (lambda b: cb(b[:-1])) if len(calls) == 3 else cb, *a)  # the third file arrives a byte short
        self.x.ftp.retrbinary = short
        before = self.snap(self.ft)
        with self.assertRaises(savenet.SaveNetError) as cm:
            rec.remove_all(self.x, "valve", self.dest)
        self.assertIn("not whole", str(cm.exception))
        self.assertEqual(self.snap(self.ft), before)

    def test_a_file_that_grew_since_the_copy_is_left(self):
        real = rec.copy_to_pc

        def copy_then_grow(*a, **k):
            r = real(*a, **k)
            with open(os.path.join(self.ft, NAMES[0]), "ab") as f:
                f.write(b"more")  # the recorder wrote more after the copy
            return r
        with mock.patch.object(rec, "copy_to_pc", copy_then_grow):
            folder, removed, left, freed = rec.remove_all(self.x, "valve", self.dest)
        self.assertEqual(left, [NAMES[0]])
        self.assertEqual(removed, 4)
        self.assertTrue(os.path.exists(os.path.join(self.ft, NAMES[0])))

    def test_a_file_that_grows_after_it_was_claimed_is_given_its_name_back_not_deleted(self):
        real = self.x.rename_file
        grown = []

        def rename_then_grow(old, new):
            real(old, new)
            if new.endswith(rec.QUARANTINE) and not grown:  # the first claimed file grows (and closes) after the claim
                with open(os.path.join(self.ft, os.path.basename(new)), "ab") as f:
                    f.write(b"tail written after the copy")
                grown.append(os.path.basename(old))
        self.x.rename_file = rename_then_grow
        folder, removed, left, freed = rec.remove_all(self.x, "valve", self.dest)
        self.assertEqual(left, grown)
        self.assertEqual(removed, 4)
        self.assertTrue(os.path.exists(os.path.join(self.ft, grown[0])))  # kept, under its own name, with its tail
        self.assertFalse([n for n in os.listdir(self.ft) if n.endswith(rec.QUARANTINE)])

    def test_a_file_the_console_will_not_rename_is_left_where_it_is(self):
        real = self.x.rename_file

        def refuse_one(old, new):
            if os.path.basename(old) == NAMES[1]:
                raise OSError("550 sharing violation")  # open for writing
            real(old, new)
        self.x.rename_file = refuse_one
        folder, removed, left, freed = rec.remove_all(self.x, "valve", self.dest)
        self.assertEqual((removed, left), (4, [NAMES[1]]))
        self.assertTrue(os.path.exists(os.path.join(self.ft, NAMES[1])))

    def test_files_left_by_a_cut_run_are_still_recordings_and_are_removed_next_time(self):
        os.rename(os.path.join(self.ft, NAMES[0]), os.path.join(self.ft, NAMES[0] + rec.QUARANTINE))
        self.assertEqual(rec.summary(self.x, "valve")["files"], 5)
        folder, removed, left, freed = rec.remove_all(self.x, "valve", self.dest)
        self.assertEqual((removed, left), (5, []))
        self.assertEqual(sorted(os.listdir(self.ft)), ["notes.txt"])

    def test_two_copies_in_the_same_second_are_both_kept(self):
        import datetime
        now = datetime.datetime(2026, 11, 15, 12, 0, 0)
        f1, _ = rec.copy_to_pc(self.x, "valve", self.dest, now=now)
        f2, _ = rec.copy_to_pc(self.x, "valve", self.dest, now=now)
        self.assertNotEqual(f1, f2)
        self.assertTrue(os.path.isdir(f1) and os.path.isdir(f2))

    def test_no_room_on_the_pc_refuses_and_changes_nothing(self):
        before = self.snap(self.ft)
        with mock.patch.object(rec.shutil, "disk_usage", return_value=mock.Mock(free=10)):
            with self.assertRaises(savenet.SaveNetError):
                rec.remove_all(self.x, "valve", self.dest)
        self.assertEqual(self.snap(self.ft), before)

    def test_a_listing_that_fails_deletes_nothing(self):
        real = xboxftp.Xbox.listdir_strict

        def deny(x, path="/"):
            if path.endswith("/frametime"):
                raise xboxftp.UploadError("550 access denied")
            return real(x, path)
        before = self.snap(self.ft)
        with mock.patch.object(xboxftp.Xbox, "listdir_strict", deny):
            with self.assertRaises(savenet.SaveNetError):
                rec.remove_all(self.x, "valve", self.dest)
        self.assertEqual(self.snap(self.ft), before)

    def test_the_command_line(self):
        def run(mode):
            a = argparse.Namespace(upload="127.0.0.1", ftp_user="xbox", ftp_password="xbox", title="hl", recordings=mode,
                                   recordings_dest=self.dest)
            import r3d_packager
            out, err = io.StringIO(), io.StringIO()
            with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
                with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                    rc = r3d_packager.recordings(a)
            self.assertEqual(err.getvalue() if rc else "", "")  # an error says what it was
            return rc, out.getvalue()
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
        rc, text = run("list")
        self.assertEqual(rc, 0)
        self.assertIn("5 recording files", text)
        self.assertEqual(len(os.listdir(self.ft)), 6)
        rc, text = run("copy")
        self.assertIn("nothing was deleted", text)
        self.assertEqual(len(os.listdir(self.ft)), 6)
        rc, text = run("remove")
        self.assertEqual(rc, 0)
        self.assertIn("then removed 5 files", text)
        self.assertEqual(sorted(os.listdir(self.ft)), ["notes.txt"])
        rc, text = run("remove")
        self.assertIn("no frame-time recordings", text)

    def test_the_command_line_needs_a_console(self):
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
        import r3d_packager
        a = argparse.Namespace(upload=None, ftp_user="x", ftp_password="x", title="hl", recordings="list", recordings_dest=None)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(r3d_packager.recordings(a), 2)


class Names(unittest.TestCase):
    def test_only_recorder_names_match(self):
        for ok in ("ft_play-v013_b96e1c1d_070.bin", "ft_x4a_0123abcd_000.bin"):
            self.assertTrue(rec.REC_NAME.match(ok), ok)
        for bad in ("quick.sav", "ft_x.bin", "ft_a_b96e1c1d_070.bin.part", "../ft_a_b96e1c1d_070.bin", "ft_a_ZZZZZZZZ_070.bin",
                    "notes.txt", "ft_a_b96e1c1d_7.bin"):
            self.assertFalse(rec.REC_NAME.match(bad), bad)


if __name__ == "__main__":
    unittest.main()
