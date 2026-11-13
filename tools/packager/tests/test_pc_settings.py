import os
import re
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
PACKAGER = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(PACKAGER))
sys.path.insert(0, PACKAGER)
from r3dpack import stage  # noqa: E402
import make_payload  # noqa: E402

CONFIG = os.path.join(REPO, "assets", "config")


def cfg_values(name):
    """{cvar: value} of a cfg file, last assignment wins"""
    out = {}
    with open(os.path.join(CONFIG, name), encoding="utf-8") as f:
        for line in f:
            line = line.split("//")[0].strip()
            m = re.match(r'(\w+)\s+"?([^"\s]*)"?$', line)
            if m:
                out[m.group(1)] = m.group(2)
    return out


class PcFilesStayOff(unittest.TestCase):
    PC = ("config.cfg", "autoexec.cfg", "controller.cfg", "joystick.cfg", "userconfig.cfg", "steam_autocloud.vdf",
          "controller_configs/ps4_controller_config_gyro.vdf", "controller_configs/hl1_manifest.vdf")
    KEEP = ("liblist.gam", "language.cfg", "violence.cfg", "skill.cfg", "maps/c0a0.bsp", "gfx/env/sky.tga")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        for gd in ("valve",):
            for n in self.PC + self.KEEP + ("valve.rc",):
                p = os.path.join(self.tmp.name, gd, *n.split("/"))
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "wb") as f:
                    f.write(b"pc " + n.encode())

    def plan(self, extra=()):
        ok = {"ok": True, "problems": [], "notes": []}
        logs = []
        with mock.patch.object(stage, "inspect", return_value=ok), \
                mock.patch.object(stage.payload_mod, "load", return_value=({}, list(extra))):
            files, _ = stage.plan(self.tmp.name, "hl", None, logs.append, None)
        return {r: s for r, s, _ in files}, logs

    def test_pc_settings_and_steam_input_files_are_left_off(self):
        got, logs = self.plan()
        for n in self.PC:
            self.assertNotIn("valve/" + n, got, n)
        self.assertTrue(any("Left out PC settings" in l and "config.cfg" in l and "controller_configs/" in l
                            for l in logs), logs)

    def test_the_pc_saved_games_are_left_off(self):
        for n in ("SAVE/quick.sav", "SAVE/quick.bmp", "SAVE/c1a0.HL1", "save/autosave.sav"):
            p = os.path.join(self.tmp.name, "valve", *n.split("/"))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(b"pc save")
        got, logs = self.plan()
        self.assertEqual([r for r in got if r.lower().startswith("valve/save/")], [])
        self.assertTrue(any("saved games" in l for l in logs), logs)
        self.assertTrue(stage._is_pc_save("save/quick.sav"))
        self.assertFalse(stage._is_pc_save("save"))  # a plain file of that name is not the folder
        self.assertFalse(stage._is_pc_save("maps/save/x.bsp"))  # only the top level of a game folder
        self.assertFalse(stage._is_pc_save("savegame.cfg"))

    def test_everything_else_in_the_game_folder_is_kept(self):
        got, _ = self.plan()
        for n in self.KEEP:
            self.assertIn("valve/" + n, got, n)

    def test_the_xbox_valve_rc_replaces_the_pc_one(self):
        xbox_rc = ("valve/valve.rc", b"xbox", 4)
        got, _ = self.plan([xbox_rc])
        self.assertEqual(got["valve/valve.rc"], b"xbox")

    def test_the_names_match_whatever_case_the_install_uses(self):
        self.assertTrue(stage._is_pc_only("config.cfg"))
        self.assertTrue(stage._is_pc_only("controller_configs/x.vdf"))
        self.assertFalse(stage._is_pc_only("controller_configs"))  # a plain file of that name is not the folder
        self.assertFalse(stage._is_pc_only("gfx/config.cfg"))  # only the top level of a game folder
        self.assertFalse(stage._is_pc_only("language.cfg"))


class XboxConfigFiles(unittest.TestCase):
    def test_the_payload_ships_the_xbox_files(self):
        out = make_payload.repo_files("valve")
        for disc, src in (("valve/valve.rc", "valve.rc"), ("valve/xbox_config.cfg", "xbox_config.cfg"),
                          ("valve/xbox_controller.cfg", "xbox_controller.cfg"),
                          ("valve/xbox_sound.cfg", "xbox_sound.cfg")):
            self.assertIn(disc, out)
            self.assertTrue(os.path.isfile(os.path.join(REPO, out[disc])), out[disc])
            self.assertEqual(os.path.basename(out[disc]), src)

    def test_the_startup_chain(self):
        with open(os.path.join(CONFIG, "valve.rc"), encoding="utf-8") as f:
            rc = [l.split("//")[0].strip() for l in f]
        self.assertIn("exec xbox_config.cfg", rc)
        self.assertIn("stuffcmds", rc)  # the engine only accepts it from a .rc
        self.assertNotIn("exec controller.cfg", rc)
        self.assertNotIn("exec autoexec.cfg", rc)
        with open(os.path.join(CONFIG, "xbox_config.cfg"), encoding="utf-8") as f:
            cfg = [l.split("//")[0].strip() for l in f]
        self.assertIn("exec xbox_controller.cfg", cfg)
        self.assertIn("exec xbox_sound.cfg", cfg)

    def test_the_controller_file_keeps_the_xbox_dead_zone(self):
        with open(os.path.join(REPO, "common", "defaults.h"), encoding="utf-8") as f:
            m = re.search(r'#if XASH_XBOX && !defined\( DEFAULT_JOY_DEADZONE \).*?#define DEFAULT_JOY_DEADZONE "(\d+)"',
                          f.read(), re.S)
        self.assertIsNotNone(m, "common/defaults.h no longer defines the Xbox dead zone the way this test reads it")
        v = cfg_values("xbox_controller.cfg")
        for axis in ("side", "forward", "pitch", "yaw"):
            self.assertEqual(v["joy_%s_deadzone" % axis], m.group(1), axis)  # never the PC's 0
        self.assertEqual(v["joy_enable"], "1")


if __name__ == "__main__":
    unittest.main()
