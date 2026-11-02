import os, sys, tempfile, threading, unittest
import functools
from unittest import mock
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from r3dpack import xboxftp

try:
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler
    from pyftpdlib.servers import FTPServer
except ImportError:
    FTPServer = None


class Listing(unittest.TestCase):
    def test_unix_and_dos(self):
        got = xboxftp.parse_listing([
            "drwxr-xr-x   1 ftp ftp            0 Jan  1  2000 Games",
            "-rw-r--r--   1 ftp ftp         1234 Jan  1 12:00 default.xbe",
            "11-01-26  09:30AM       <DIR>          valve",
            "11-01-26  09:30AM               5120 liblist.gam",
        ])
        self.assertEqual(got["Games"], (True, 0))
        self.assertEqual(got["default.xbe"], (False, 1234))
        self.assertEqual(got["valve"], (True, 0))
        self.assertEqual(got["liblist.gam"], (False, 5120))


@unittest.skipUnless(FTPServer, "pyftpdlib not installed")
class Upload(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.root, "F", "Games"))
        os.makedirs(os.path.join(self.root, "G"))
        os.makedirs(os.path.join(self.root, "C", "Games"))
        os.makedirs(os.path.join(self.root, "E"))
        a = DummyAuthorizer()
        a.add_user("xbox", "xbox", self.root, perm="elradfmwMT")
        h = FTPHandler
        h.authorizer = a
        self.srv = FTPServer(("127.0.0.1", 0), h)
        self.port = self.srv.address[1]
        threading.Thread(target=self.srv.serve_forever, kwargs={"timeout": 0.1}, daemon=True).start()

    def tearDown(self):
        self.srv.close_all()

    def test_find_and_upload_and_skip(self):
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        self.assertEqual(x.find_game_folders(), [("/F/Games", True, True), ("/G/Games", False, True), ("/E/Games", False, False)])
        files = [("default.xbe", b"XBEH" * 10, 40), ("valve/maps/a.bsp", b"x" * 5000, 5000), ("valve/empty", b"", 0)]
        sent, skipped = x.upload(files, "/F/Games/Half-Life")
        self.assertEqual((sent, skipped), (3, 0))
        self.assertEqual(open(os.path.join(self.root, "F/Games/Half-Life/valve/maps/a.bsp"), "rb").read(), b"x" * 5000)
        sent, skipped = x.upload(files, "/F/Games/Half-Life")
        self.assertEqual((sent, skipped), (0, 3))
        self.assertTrue(x.exists("/F/Games/Half-Life"))
        x.close()

    def _existing_install(self):
        base = os.path.join(self.root, "F", "Games", "Half-Life")
        os.makedirs(os.path.join(base, "valve", "userconfig.d"))
        for rel, data in (("default.xbe", b"OLDBUILD"), ("default.xbe.perf11e", b"PERF11E!"),
                          ("valve/config.cfg", b"mine"), ("valve/userconfig.d/90-route.cfg", b"route"),
                          ("valve/liblist.gam", b"same")):
            with open(os.path.join(base, *rel.split("/")), "wb") as f:
                f.write(data)
        return base

    def _new_files(self, xbe=b"NEWBUILD"):
        return [("default.xbe", xbe, len(xbe)), ("valve/config.cfg", b"pc config!!", 11),
                ("valve/liblist.gam", b"same", 4), ("valve/new.txt", b"n", 1)]

    def test_survey_of_a_fresh_folder_replaces_nothing(self):
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        plan = x.survey(self._new_files(), "/F/Games/Half-Life")
        self.assertFalse(plan.exists)
        self.assertFalse(plan.needs_confirmation)
        x.close()

    def test_survey_of_an_existing_install(self):
        self._existing_install()
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        plan = x.survey(self._new_files(), "/F/Games/Half-Life")
        self.assertTrue(plan.exists and plan.needs_confirmation and plan.xbe_differs)
        self.assertEqual(plan.replace, ["default.xbe"])
        self.assertEqual(plan.keep, ["valve/config.cfg"])
        self.assertEqual(plan.new, ["valve/new.txt"])
        self.assertEqual(plan.same, 1)
        x.close()

    def test_a_different_build_of_the_same_size_is_found(self):
        self._existing_install()
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        plan = x.survey(self._new_files(xbe=b"NEWBUILD"[:8].replace(b"N", b"X")), "/F/Games/Half-Life")
        self.assertEqual(plan.replace, ["default.xbe"])  # 8 bytes both, different content
        x.close()

    def test_upload_keeps_settings_and_backs_up_the_old_xbe(self):
        base = self._existing_install()
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        files = self._new_files()
        plan = x.survey(files, "/F/Games/Half-Life")
        x.upload(files, "/F/Games/Half-Life", plan=plan)
        rd = lambda rel: open(os.path.join(base, *rel.split("/")), "rb").read()  # noqa: E731
        self.assertEqual(rd("default.xbe"), b"NEWBUILD")
        self.assertEqual(rd("default.xbe.before-packager"), b"OLDBUILD")
        self.assertEqual(rd("default.xbe.perf11e"), b"PERF11E!")  # the user's own backups are untouched
        self.assertEqual(rd("valve/config.cfg"), b"mine")
        self.assertEqual(rd("valve/userconfig.d/90-route.cfg"), b"route")
        self.assertEqual(rd("valve/new.txt"), b"n")
        x.close()

    def test_a_second_backup_does_not_overwrite_the_first(self):
        base = self._existing_install()
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        x.upload(self._new_files(), "/F/Games/Half-Life", plan=x.survey(self._new_files(), "/F/Games/Half-Life"))
        files2 = self._new_files(xbe=b"BUILD-TWO")
        x.upload(files2, "/F/Games/Half-Life", plan=x.survey(files2, "/F/Games/Half-Life"))
        self.assertEqual(open(os.path.join(base, "default.xbe.before-packager"), "rb").read(), b"OLDBUILD")
        self.assertEqual(open(os.path.join(base, "default.xbe.before-packager-2"), "rb").read(), b"NEWBUILD")
        x.close()

    def test_helper_stops_without_confirmation_and_changes_nothing(self):
        base = self._existing_install()
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
        import r3d_packager
        files = self._new_files()
        args = ("hl", "127.0.0.1", "/F/Games", "xbox", "xbox", None, lambda m: None)
        with mock.patch.object(xboxftp, "Xbox", functools.partial(xboxftp.Xbox, port=self.port)):
            with self.assertRaises(xboxftp.UploadError) as cm:
                r3d_packager.upload(files, *args)
            self.assertIn("--overwrite", str(cm.exception))
            self.assertEqual(open(os.path.join(base, "default.xbe"), "rb").read(), b"OLDBUILD")
            with self.assertRaises(xboxftp.UploadError):  # the user says no
                r3d_packager.upload(files, *args, confirm=lambda t: False)
            self.assertEqual(open(os.path.join(base, "default.xbe"), "rb").read(), b"OLDBUILD")
            r3d_packager.upload(files, *args, confirm=lambda t: True)  # the user says yes
            self.assertEqual(open(os.path.join(base, "default.xbe"), "rb").read(), b"NEWBUILD")

    def test_stale_bsp_of_baked_maps_removed_others_kept(self):
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        old = [("valve/maps/a.bsp", b"x", 1), ("valve/maps/b.bsp", b"x", 1), ("valve/maps/a.cfg", b"x", 1)]
        x.upload(old, "/F/Games/Half-Life")
        new = [("valve/a.mpak", b"m", 1), ("valve/maps/a.cfg", b"x", 1)]
        x.upload(new, "/F/Games/Half-Life")
        gone = x.remove_stale_bsp(new, "/F/Games/Half-Life")
        self.assertEqual(gone, ["valve/maps/a.bsp"])
        left = sorted(os.listdir(os.path.join(self.root, "F/Games/Half-Life/valve/maps")))
        self.assertEqual(left, ["a.cfg", "b.bsp"])  # b has no archive, a.cfg is not a .bsp
        x.close()


if __name__ == "__main__":
    unittest.main()
