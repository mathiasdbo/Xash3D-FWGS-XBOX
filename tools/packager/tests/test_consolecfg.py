import os
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from r3dpack import consolecfg as c  # noqa: E402
from r3dpack import target as tg  # noqa: E402
from r3dpack import xboxftp  # noqa: E402

try:
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler
    from pyftpdlib.servers import FTPServer
except ImportError:
    FTPServer = None


class Render(unittest.TestCase):
    def test_only_set_values_are_written(self):
        t = c.render({"r3d_vid_mode": "1", "nv2a_r_anisotropy": "8"})
        self.assertIn('r3d_vid_mode "1"', t)
        self.assertIn('nv2a_r_anisotropy "8"', t)
        self.assertNotIn("nv2a_r_aa", t)
        self.assertTrue(t.startswith("// Managed by"))

    def test_round_trip(self):
        values = {"r3d_vid_mode": "1", "host_frametime_analysis": "2", "host_frametime_label": "office-run"}
        startup = {"map": "c0a0", "cheats": True, "give_suit": True}
        back = c.read_managed(c.render(values, startup))
        self.assertEqual(back, (values, startup))

    def test_empty_settings_write_a_header_only(self):
        self.assertEqual(c.parse_assignments(c.render({})), [])

    def test_bad_input_is_refused(self):
        for values, startup in (({"r3d_vid_mode": "5"}, {}), ({"nope": "1"}, {}),
                                ({"host_frametime_label": "has space"}, {}),
                                ({"host_frametime_label": "x" * 23}, {}),
                                ({}, {"map": "c0a0; quit"}), ({}, {"map": "c0a0", "give_suit": True})):
            with self.assertRaises(c.ConfigError):
                c.render(values, startup)

    def test_every_choice_default_is_one_of_its_choices(self):
        for s in c.SETTINGS:
            if s["kind"] == "choice":
                self.assertIn(s["default"], [v for v, _ in s["choices"]], s["key"])

    def test_presets_are_valid(self):
        for name in c.PRESETS:
            values, startup = c.preset(name)
            c.render(values, startup)


class DeadZones(unittest.TestCase):
    ZEROS = ("config.cfg (menu)", "joy_yaw_deadzone \"0\"\njoy_pitch_deadzone \"0\"\n"
                                  "joy_side_deadzone \"0\"\njoy_forward_deadzone \"0\"\n")

    def test_the_four_dead_zones_are_settings_with_the_xbox_default(self):
        got = [s for s in c.SETTINGS if s["group"] == "Controller"]
        self.assertEqual(sorted(s["key"] for s in got), sorted(c.DEAD_ZONE_KEYS))
        for s in got:
            self.assertEqual(s["default"], c.DEAD_ZONE_XBOX)
            self.assertIn(c.DEAD_ZONE_XBOX, [v for v, _ in s["choices"]])

    def test_the_default_is_the_engines_xbox_default(self):
        import re
        repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
        with open(os.path.join(repo, "common", "defaults.h"), encoding="utf-8") as f:
            m = re.search(r'#if XASH_XBOX && !defined\( DEFAULT_JOY_DEADZONE \).*?#define DEFAULT_JOY_DEADZONE "(\d+)"',
                          f.read(), re.S)
        self.assertIsNotNone(m, "common/defaults.h no longer defines the Xbox dead zone the way this test reads it")
        self.assertEqual(c.DEAD_ZONE_XBOX, m.group(1))

    def test_the_fix_preset_writes_all_four_and_nothing_else(self):
        values, startup = c.preset("Fix stick drift")
        self.assertEqual(values, {k: "8689" for k in c.DEAD_ZONE_KEYS})
        text = c.render(values, startup)
        for k in c.DEAD_ZONE_KEYS:
            self.assertIn('%s "8689"' % k, text)
        self.assertEqual(c.read_managed(text)[0], values)  # what is written reads back

    def test_a_pcs_zero_dead_zones_are_flagged(self):
        notes = c.Effective([self.ZEROS]).notes({}, {})
        self.assertEqual(len(notes), 1)
        self.assertIn("Fix stick drift", notes[0])
        for k in c.DEAD_ZONE_KEYS:
            self.assertIn(k, notes[0])

    def test_the_warning_goes_once_the_fix_is_on_screen_or_written(self):
        eff = c.Effective([self.ZEROS])
        self.assertEqual(eff.notes(c.preset("Fix stick drift")[0], {}), [])  # chosen, not yet written
        later = c.Effective([self.ZEROS, ("50-packager.cfg", c.render(c.preset("Fix stick drift")[0]))])
        self.assertEqual(later.notes({}, {}), [])  # written: userconfig.d runs after config.cfg

    def test_a_dead_zone_the_user_chose_is_left_alone(self):
        ok = c.Effective([("config.cfg (menu)", "joy_yaw_deadzone \"10000\"\njoy_pitch_deadzone \"8689\"\n")])
        self.assertEqual(ok.notes({}, {}), [])
        some = c.Effective([("config.cfg (menu)", "joy_yaw_deadzone \"0\"\njoy_pitch_deadzone \"8689\"\n")])
        self.assertEqual([k for k, _, _ in some.drift({})], ["joy_yaw_deadzone"])
        self.assertEqual(c.Effective([]).notes({}, {}), [])  # nothing set anywhere: the engine default applies


class Parse(unittest.TestCase):
    def test_forms_and_comments(self):
        got = c.parse_assignments('r3d_vid_mode "1"\nnv2a_r_aa 3 // x\nseta cl_showfps 2\nbind w "+forward"\n// map c9\n')
        self.assertEqual(got, [("r3d_vid_mode", "1"), ("nv2a_r_aa", "3"), ("cl_showfps", "2")])

    def test_crlf(self):
        self.assertEqual(c.parse_assignments('r3d_snd_hw "1"\r\nmap c0a0\r\n'), [("r3d_snd_hw", "1"), ("map", "c0a0")])


class Effective(unittest.TestCase):
    FILES = [("config.cfg (menu)", 'r3d_vid_mode "0"\nnv2a_r_aa "1"\n'),
             ("50-packager.cfg", 'r3d_vid_mode "1"\nnv2a_r_anisotropy "8"\n'),
             ("90-route.cfg", 'r3d_vid_mode 0\nmap c0a0\n')]

    def test_last_file_wins_and_names_its_source(self):
        e = c.Effective(self.FILES)
        self.assertEqual(e.value["r3d_vid_mode"], ("0", "90-route.cfg"))
        self.assertEqual(e.value["nv2a_r_aa"], ("1", "config.cfg (menu)"))

    def test_override_and_map_warnings(self):
        e = c.Effective(self.FILES)
        notes = e.notes({"r3d_vid_mode": "1", "nv2a_r_anisotropy": "8"}, {})
        self.assertTrue(any("r3d_vid_mode" in n and "90-route.cfg" in n for n in notes))
        self.assertTrue(any("loads map c0a0" in n for n in notes))
        self.assertFalse(any("nv2a_r_anisotropy" in n for n in notes))

    def test_no_warnings_when_nothing_overrides(self):
        e = c.Effective(self.FILES[:2])
        self.assertEqual(e.notes({"r3d_vid_mode": "1"}, {}), [])


class Diff(unittest.TestCase):
    def test_diff(self):
        old = c.render({"r3d_vid_mode": "0", "nv2a_r_aa": "1"})
        new = c.render({"r3d_vid_mode": "1", "nv2a_r_anisotropy": "8"})
        self.assertEqual(c.diff(old, new), ["- nv2a_r_aa (was 1)", "+ nv2a_r_anisotropy 8", "~ r3d_vid_mode 0 -> 1"])


class FolderTargetTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        os.makedirs(os.path.join(self.tmp.name, "valve", "userconfig.d"))
        for n, d in (("90-route.cfg", b'r3d_vid_mode 0\nmap c0a0\n'), ("91-fps.cfg.off", b"cl_showfps 2\n")):
            with open(os.path.join(self.tmp.name, "valve", "userconfig.d", n), "wb") as f:
                f.write(d)
        self.t = tg.FolderTarget(self.tmp.name)

    def test_lists_files_with_state(self):
        self.assertEqual([(n, e) for n, e, _ in self.t.userconfig_files("valve")], [("90-route.cfg", True), ("91-fps.cfg.off", False)])

    def test_write_keeps_a_backup_and_other_files_alone(self):
        tg.write_settings(self.t, "valve", {"r3d_vid_mode": "1"}, {})
        rel = "valve/userconfig.d/50-packager.cfg"
        first = self.t.read(rel)
        self.assertIn(b'r3d_vid_mode "1"', first)
        self.assertIsNone(self.t.read(rel + ".before-packager"))  # nothing to back up the first time
        tg.write_settings(self.t, "valve", {"r3d_vid_mode": "0"}, {})
        self.assertEqual(self.t.read(rel + ".before-packager"), first)
        self.assertEqual(self.t.read("valve/userconfig.d/90-route.cfg"), b'r3d_vid_mode 0\nmap c0a0\n')

    def test_toggle_renames_and_refuses_to_overwrite(self):
        tg.write_settings(self.t, "valve", {}, {}, {"91-fps.cfg.off": True, "90-route.cfg": False})
        names = {n: e for n, e, _ in self.t.userconfig_files("valve")}
        self.assertEqual(names, {"50-packager.cfg": True, "90-route.cfg.off": False, "91-fps.cfg": True})
        with open(os.path.join(self.tmp.name, "valve", "userconfig.d", "91-fps.cfg.off"), "wb") as f:
            f.write(b"again")
        with self.assertRaises(tg.TargetError):
            self.t.toggle("valve", "91-fps.cfg", True)

    def test_state_reports_the_override(self):
        tg.write_settings(self.t, "valve", {"r3d_vid_mode": "1"}, {})
        st = tg.load_state(self.t, "valve")
        self.assertEqual(st["values"], {"r3d_vid_mode": "1"})
        self.assertTrue(st["effective"].notes(st["values"], st["startup"]))


@unittest.skipUnless(FTPServer, "pyftpdlib not installed")
class FtpTargetTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        base = os.path.join(self.root, "F", "Games", "Half-Life", "valve", "userconfig.d")
        os.makedirs(base)
        with open(os.path.join(base, "90-route.cfg"), "wb") as f:
            f.write(b"r3d_vid_mode 0\nmap c0a0\n")
        with open(os.path.join(self.root, "F", "Games", "Half-Life", "default.xbe"), "wb") as f:
            f.write(b"XBEH")
        nested = os.path.join(self.root, "F", "Games", "r3d-test", "Half-Life")
        os.makedirs(nested)
        with open(os.path.join(nested, "default.xbe"), "wb") as f:
            f.write(b"XBEH")
        live = os.path.join(self.root, "E", "UDATA", "FFFF0002", "valve", "valve")
        os.makedirs(live)
        with open(os.path.join(live, "config.cfg"), "wb") as f:
            f.write(b'nv2a_r_aa "2"\r\n')
        a = DummyAuthorizer()
        a.add_user("xbox", "xbox", self.root, perm="elradfmwMT")
        h = FTPHandler
        h.authorizer = a
        self.srv = FTPServer(("127.0.0.1", 0), h)
        self.port = self.srv.address[1]
        threading.Thread(target=self.srv.serve_forever, kwargs={"timeout": 0.1}, daemon=True).start()

    def tearDown(self):
        self.srv.close_all()

    def test_finds_installs_at_two_depths(self):
        x = xboxftp.Xbox("127.0.0.1", port=self.port)
        x.connect()
        self.assertEqual(x.find_installs("/F/Games"), ["/F/Games/Half-Life", "/F/Games/r3d-test/Half-Life"])
        # another game with a default.xbe but no valve folder is not offered
        other = os.path.join(self.root, "F", "Games", "Halo")
        os.makedirs(other)
        with open(os.path.join(other, "default.xbe"), "wb") as f:
            f.write(b"XBEH")
        self.assertEqual(x.find_installs("/F/Games", needs="valve"), ["/F/Games/Half-Life"])
        x.close()

    def test_read_write_toggle_and_the_live_config(self):
        t = tg.FtpTarget("127.0.0.1", "/F/Games/Half-Life", port=self.port)
        t.open()
        st = tg.load_state(t, "valve")
        self.assertEqual(st["effective"].value["nv2a_r_aa"], ("2", "config.cfg (menu)"))  # from the save folder
        tg.write_settings(t, "valve", {"nv2a_r_anisotropy": "8"}, {"map": "c0a0"}, {"90-route.cfg": False})
        st = tg.load_state(t, "valve")
        self.assertEqual(st["values"], {"nv2a_r_anisotropy": "8"})
        self.assertEqual({n: e for n, e, _ in st["files"]}, {"50-packager.cfg": True, "90-route.cfg.off": False})
        self.assertTrue(os.path.exists(os.path.join(self.root, "F/Games/Half-Life/valve/userconfig.d/90-route.cfg.off")))
        t.close()


if __name__ == "__main__":
    unittest.main()
