"""XS.7: tools/xbe_release.py. Run: python -m unittest discover -s tools -p "test_xbe_release.py" """
import contextlib
import io
import os
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_fixture_gate  # noqa: E402
import save_live_gate  # noqa: E402
import test_save_rules_check as rules_tests  # noqa: E402
import xbe_release as rel  # noqa: E402

COMMIT = "a" * 40


def git_ok(*args):
    return (0, COMMIT) if args[0] == "rev-parse" else (0, "")


class Release(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = self.tmp.name
        self.xbe_path = os.path.join(t, "default.xbe")
        self.xbe = rules_tests.small_xbe(0xFFFF0002)
        with open(self.xbe_path, "wb") as f:
            f.write(self.xbe)
        self.map = os.path.join(t, "default.map")
        with open(self.map, "wb") as f:
            f.write(b" Address  Publics by Value\n" + b"0001:00000000 ?GameDLLInit@@YAXXZ\n" * 60)
        self.ident = self.xbe_path + ".identity"
        self.write_identity(rel.sha(self.xbe))
        self.evidence = os.path.join(t, "ev.txt")
        self.write_evidence(rel.sha(self.xbe))

    def tearDown(self):
        self.tmp.cleanup()

    def write_identity(self, sha, modules="server+client+freevgui", commit=COMMIT, dirty="0", map_sha=None, old=False):
        with open(self.map, "rb") as f:
            msha = map_sha or rel.sha(f.read())
        with open(self.ident, "w", encoding="utf-8") as f:
            f.write("title=hl\nmodules=%s\nmainui=1\nsha256_xbe=%s\n" % (modules, sha))
            if not old:  # an identity from before XS.7 has no commit, dirty or sha256_map
                f.write("commit=%s\ndirty=%s\nsha256_map=%s\n" % (commit, dirty, msha))

    def write_evidence(self, sha, verdict="PASS", drop=None):
        with open(save_fixture_gate.EXPECTED, encoding="utf-8") as f:
            expected = save_fixture_gate.parse_expected(f.read())
        with open(self.evidence, "w", encoding="utf-8") as f:
            f.write("xbe %s\n" % sha)
            for h, (name, _) in expected.items():
                if name.lower().endswith(".sav") and name != drop:
                    f.write("fixture %s %s c1a4e 3.3 %s\n" % (h, name, verdict))

    def problems(self, **kw):
        args = dict(xbe_path=self.xbe_path, commit=COMMIT, map_path=self.map, identity_path=self.ident,
                    evidence=self.evidence, runner=git_ok, source_check=lambda: [])
        args.update(kw)
        return rel.problems(**args)

    def test_everything_in_order_passes(self):
        self.assertEqual(self.problems(), [])

    def test_a_short_commit_is_refused(self):
        self.assertIn("40-hex", self.problems(commit="abc123")[0])

    def test_a_checkout_at_another_commit_is_refused(self):
        p = self.problems(runner=lambda *a: (0, "b" * 40) if a[0] == "rev-parse" else (0, ""))
        self.assertTrue(any("check the build's commit out" in x for x in p))

    def test_a_commit_not_on_main_is_refused(self):
        p = self.problems(runner=lambda *a: (0, COMMIT) if a[0] == "rev-parse" else (1, ""))
        self.assertTrue(any("not on origin/main" in x for x in p))

    def test_another_title_id_is_refused(self):
        with open(self.xbe_path, "wb") as f:
            f.write(rules_tests.small_xbe(0x12345678))
        self.assertTrue(any("title id" in x for x in self.problems()))

    def test_a_source_that_broke_a_save_rule_is_refused(self):
        p = self.problems(source_check=lambda: ["SAVEGAME_VERSION is 0x72"])
        self.assertIn("SAVEGAME_VERSION is 0x72", p)

    def test_no_live_evidence_for_these_bytes_is_refused(self):
        self.write_evidence("0" * 64)
        self.assertTrue(any("another XBE" in x for x in self.problems()))

    def test_a_failed_or_missing_fixture_is_refused(self):
        self.write_evidence(rel.sha(self.xbe), verdict="FAIL")
        self.assertTrue(any("did not load" in x for x in self.problems()))
        self.write_evidence(rel.sha(self.xbe), drop="Half-Life-000.sav")
        self.assertTrue(any("was not loaded" in x for x in self.problems()))

    def test_a_missing_or_foreign_map_is_refused(self):
        os.remove(self.map)
        self.assertTrue(any("no link map" in x for x in self.problems()))
        with open(self.map, "wb") as f:
            f.write(b"not a map")
        self.assertTrue(any("not the link map" in x for x in self.problems()))

    def test_an_identity_for_another_xbe_is_refused(self):
        self.write_identity("1" * 64)
        self.assertTrue(any("would not be this build's" in x for x in self.problems()))

    def test_a_build_that_is_not_the_full_game_is_refused(self):
        self.write_identity(rel.sha(self.xbe), modules="server")
        self.assertTrue(any("not a bootable game build" in x for x in self.problems()))

    def test_an_xbe_built_from_another_commit_is_refused_even_with_its_own_valid_papers(self):
        # checked out at COMMIT, but the XBE (and its identity and evidence) are from commit B
        self.write_identity(rel.sha(self.xbe), commit="b" * 40)
        p = self.problems()
        self.assertTrue(any("built from bbbbbbbbbb, not from aaaaaaaaaa" in x for x in p))

    def test_a_dirty_build_is_refused(self):
        self.write_identity(rel.sha(self.xbe), dirty="1")
        self.assertTrue(any("tracked files changed" in x for x in self.problems()))

    def test_a_plausible_map_that_is_not_this_builds_is_refused(self):
        with open(self.map, "wb") as f:
            f.write(b" Address  Publics by Value\n" + b"0001:00000010 ?GameDLLInit@@YAXXZ\n" * 70)  # another real-looking map
        self.assertTrue(any("not the map of this build" in x for x in self.problems()))

    def test_an_identity_from_before_xs7_is_refused_with_the_way_out(self):
        self.write_identity(rel.sha(self.xbe), old=True)
        self.assertTrue(any("rebuild with the current tools/xbox-game-link-check.sh" in x for x in self.problems()))

    def test_the_link_script_writes_what_the_release_checks(self):
        with open(os.path.join(HERE, "xbox-game-link-check.sh"), encoding="utf-8") as f:
            s = f.read()
        for key in ("commit=%s", "dirty=%s", "sha256_map=%s", "sha256_xbe=%s"):
            self.assertIn(key, s)

    def test_the_notes_carry_the_hashes_and_the_save_baseline(self):
        with open(self.map, "rb") as f:
            m = f.read()
        text = rel.notes_text(self.xbe, COMMIT, "Extra line.", m, self.evidence)
        self.assertIn(rel.sha(self.xbe), text)
        self.assertIn(rel.sha(m), text)
        self.assertIn("fixture saves loaded and played", text)
        self.assertIn(COMMIT, text)
        self.assertIn("Extra line.", text)

    def test_publish_without_the_flag_runs_nothing_outward(self):
        args = ["publish", "--xbe", self.xbe_path, "--commit", COMMIT, "--map", self.map, "--identity", self.ident,
                "--evidence", self.evidence]
        with mock.patch.object(rel, "git", git_ok), mock.patch.object(rel.save_rules_check, "source_problems", lambda: []):
            with mock.patch.object(rel.subprocess, "run") as run:
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    rel.main(args)
                run.assert_not_called()  # no gh, no git beyond the injected one
        self.assertIn("Nothing was published", out.getvalue())
        self.assertIn("xbe-aaaaaaaa", out.getvalue())

    def test_a_refused_release_exits_with_the_reasons(self):
        self.write_evidence("0" * 64)
        args = ["check", "--xbe", self.xbe_path, "--commit", COMMIT, "--map", self.map, "--identity", self.ident,
                "--evidence", self.evidence]
        with mock.patch.object(rel, "git", git_ok), mock.patch.object(rel.save_rules_check, "source_problems", lambda: []):
            with self.assertRaises(SystemExit) as cm:
                rel.main(args)
        self.assertIn("must not be released", str(cm.exception.code))


if __name__ == "__main__":
    unittest.main()
