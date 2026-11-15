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
        return rules.source_problems(self.sv, self.hdrs)

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

    def test_the_model_bits_do_not_matter_where_the_xbox_branch_sets_the_value(self):
        # MAX_MODELS is 1024 for XASH_LOW_MEMORY == 1 whatever the general bits say: the effective value is what is checked
        protocol = [p for p in self.hdrs if p.endswith("protocol.h")][0]
        self.edit(protocol, "#define MAX_MODEL_BITS\t\t12", "#define MAX_MODEL_BITS\t\t8")
        self.assertEqual(self.problems(), [])

    def test_fewer_decals_in_the_xbe_configuration_fails(self):
        # XASH_LOW_MEMORY == 1 is the Xbox branch: lowering that one is what counts
        common = [p for p in self.hdrs if p.endswith("common.h")][0]
        self.edit(common, "#elif XASH_LOW_MEMORY == 1\n#define MAX_DECALS\t\t512", "#elif XASH_LOW_MEMORY == 1\n#define MAX_DECALS\t\t256")
        self.assertTrue(any("MAX_DECALS is 256" in p for p in self.problems()))

    def test_the_effective_xbox_value_counts_not_the_first_definition(self):
        # MAX_MODELS is 4096 at first and 1024 after the XASH_LOW_MEMORY == 1 branch undefines and redefines it
        protocol = [p for p in self.hdrs if p.endswith("protocol.h")][0]
        self.edit(protocol, "#define MAX_MODELS\t\t\t1024", "#define MAX_MODELS\t\t\t256")
        self.assertTrue(any("MAX_MODELS is 256" in p for p in self.problems()))

    def test_lowering_the_engine_and_a_copy_of_the_minimum_still_fails(self):
        # the minimums are not in a file the same change can edit: only the engine side can be changed here
        common = [p for p in self.hdrs if p.endswith("common.h")][0]
        self.edit(common, "#elif XASH_LOW_MEMORY == 1\n#define MAX_DECALS\t\t512", "#elif XASH_LOW_MEMORY == 1\n#define MAX_DECALS\t\t256")
        self.assertTrue(any("MAX_DECALS is 256" in p for p in self.problems()))
        self.assertFalse(os.path.exists(os.path.join(os.path.dirname(rules.__file__), "save-limits.txt")))

    def test_undef_removes_a_definition(self):
        d = rules.defines("#define A 1\n#undef A\n#define B 2\n#define B 3\n")
        self.assertNotIn("A", d)
        self.assertEqual(d["B"], "3")

    def test_a_limit_that_disappears_fails_instead_of_passing(self):
        protocol = [p for p in self.hdrs if p.endswith("protocol.h")][0]
        self.edit(protocol, "#define MAX_LIGHTSTYLES", "#define MAX_LIGHT_STYLES")
        self.assertTrue(any("MAX_LIGHTSTYLES" in p and "renamed" in p for p in self.problems()))


class PublicBuild(unittest.TestCase):
    """the minimums are what the public v0.2.0 source had: read from git history, so lowering a number in the checker fails here"""

    def show(self, path):
        import subprocess
        r = subprocess.run(["git", "-C", rules.REPO, "show", "%s:%s" % (rules.PUBLIC_COMMIT, path)], capture_output=True, text=True)
        if r.returncode:
            self.skipTest("the public commit %s is not in this clone" % rules.PUBLIC_COMMIT)
        return r.stdout

    def test_the_minimums_are_the_public_builds_values(self):
        table = {}
        for path in ("engine/common/protocol.h", "engine/common/common.h", "common/defaults.h"):
            for k, v in rules.defines(self.show(path)).items():
                table.setdefault(k, v)
        for name, want in rules.MINIMUMS.items():
            self.assertEqual(rules.evaluate(table[name], table), want, name)
        sv = rules.defines(self.show("engine/server/sv_save.c"))
        for name, want in rules.FORMAT.items():
            self.assertEqual(rules.evaluate(sv[name], sv), want, name)


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
