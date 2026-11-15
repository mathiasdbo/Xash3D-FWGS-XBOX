"""XS.5: tools/save_rules_check.py. Run: python -m unittest discover -s tools -p "test_save_rules_check.py" """
import os
import shutil
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_rules_check as rules  # noqa: E402


def small_xbe(title_id):
    d = bytearray(0x400)
    d[:4] = b"XBEH"
    struct.pack_into("<I", d, 0x104, 0x10000)          # base address
    struct.pack_into("<I", d, 0x118, 0x10000 + 0x200)  # certificate
    struct.pack_into("<I", d, 0x200 + 0x08, title_id)
    return bytes(d)


class Title(unittest.TestCase):
    def test_the_default_id_passes(self):
        self.assertEqual(rules.xbe_problems(small_xbe(0xFFFF0002)), [])

    def test_another_id_is_refused_with_both_ids_named(self):
        p = rules.xbe_problems(small_xbe(0x12345678))
        self.assertIn("0x12345678", p[0])
        self.assertIn("0xFFFF0002", p[0])
        with self.assertRaises(SystemExit):
            rules.require_xbe(small_xbe(0x12345678))

    def test_not_an_xbe_is_a_problem_not_a_crash(self):
        self.assertTrue(rules.xbe_problems(b"nope"))
        bad = bytearray(small_xbe(rules.TITLE_ID))
        struct.pack_into("<I", bad, 0x118, 0xFFFFFFF0)
        self.assertTrue(rules.xbe_problems(bytes(bad)))


class Source(unittest.TestCase):
    """the real files, copied, then broken one rule at a time"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.sv = os.path.join(self.tmp.name, "sv_save.c")
        self.hdrs = []
        shutil.copyfile(rules.SV_SAVE, self.sv)
        for p in rules.LIMIT_FILES:
            q = os.path.join(self.tmp.name, os.path.basename(p))
            shutil.copyfile(p, q)
            self.hdrs.append(q)

    def tearDown(self):
        self.tmp.cleanup()

    def problems(self):
        return rules.source_problems(rules.LIMITS, self.sv, self.hdrs)

    def edit(self, path, old, new):
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        self.assertIn(old, s)
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(s.replace(old, new, 1))

    def test_the_repository_passes(self):
        self.assertEqual(rules.source_problems(), [])

    def test_the_copy_passes(self):
        self.assertEqual(self.problems(), [])

    def test_a_newer_save_version_fails(self):
        self.edit(self.sv, "#define SAVEGAME_VERSION\t\t0x0071", "#define SAVEGAME_VERSION\t\t0x0072")
        self.assertIn("SAVEGAME_VERSION", self.problems()[0])

    def test_a_newer_client_save_version_fails(self):
        self.edit(self.sv, "0x0067", "0x0068")
        self.assertIn("CLIENT_SAVEGAME_VERSION", self.problems()[0])

    def test_a_renamed_version_macro_fails_instead_of_passing(self):
        self.edit(self.sv, "#define SAVEGAME_VERSION", "#define SAVEGAME_VERSION_X")
        self.assertTrue(any("SAVEGAME_VERSION" in p for p in self.problems()))

    def test_a_changed_header_tag_fails(self):
        self.edit(self.sv, "(('V'<<24)+('A'<<16)+('S'<<8)+'J')", "(('V'<<24)+('A'<<16)+('S'<<8)+'K')")
        self.assertTrue(any("SAVEGAME_HEADER" in p for p in self.problems()))

    def test_fewer_edicts_fails(self):
        defaults = [p for p in self.hdrs if p.endswith("defaults.h")][0]
        self.edit(defaults, "#define DEFAULT_MAX_EDICTS 1200", "#define DEFAULT_MAX_EDICTS 900")
        self.assertTrue(any("DEFAULT_MAX_EDICTS is 900" in p for p in self.problems()))

    def test_more_edicts_is_fine(self):
        defaults = [p for p in self.hdrs if p.endswith("defaults.h")][0]
        self.edit(defaults, "#define DEFAULT_MAX_EDICTS 1200", "#define DEFAULT_MAX_EDICTS 2000")
        self.assertEqual(self.problems(), [])

    def test_fewer_model_bits_fails(self):
        protocol = [p for p in self.hdrs if p.endswith("protocol.h")][0]
        self.edit(protocol, "#define MAX_MODEL_BITS\t\t12", "#define MAX_MODEL_BITS\t\t11")
        self.assertTrue(any("MAX_MODELS is 2048" in p for p in self.problems()))

    def test_fewer_decals_in_the_xbe_configuration_fails(self):
        # XASH_LOW_MEMORY == 1 is the Xbox branch: lowering that one is what counts
        common = [p for p in self.hdrs if p.endswith("common.h")][0]
        self.edit(common, "#elif XASH_LOW_MEMORY == 1\n#define MAX_DECALS\t\t512", "#elif XASH_LOW_MEMORY == 1\n#define MAX_DECALS\t\t256")
        self.assertTrue(any("MAX_DECALS is 256" in p for p in self.problems()))

    def test_a_limit_that_disappears_fails_instead_of_passing(self):
        protocol = [p for p in self.hdrs if p.endswith("protocol.h")][0]
        self.edit(protocol, "#define MAX_LIGHTSTYLES", "#define MAX_LIGHT_STYLES")
        self.assertTrue(any("MAX_LIGHTSTYLES" in p and "renamed" in p for p in self.problems()))


class Evaluate(unittest.TestCase):
    def test_shifts_and_macros(self):
        self.assertEqual(rules.evaluate("(1<<MAX_BITS)", {"MAX_BITS": "12"}), 4096)

    def test_anything_else_is_refused(self):
        with self.assertRaises(rules.RulesError):
            rules.evaluate("__import__('os')", {})

    def test_a_macro_loop_is_refused(self):
        with self.assertRaises(rules.RulesError):
            rules.evaluate("A", {"A": "B", "B": "A"})


if __name__ == "__main__":
    unittest.main()
