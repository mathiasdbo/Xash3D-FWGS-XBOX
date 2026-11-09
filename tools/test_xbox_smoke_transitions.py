"""
Tests for tools/xbox-smoke-transitions.py and tools/smoke/make-map-list.py.

Run:  python -m unittest tools/test_xbox_smoke_transitions.py

They cover what runs on the PC without a console: the transition lists are
chains the script can walk, the plan and the cfgs it writes (the dwell, the
god/notarget toggles only on a fresh map, the end marker only on the last
map), the resume and pilot options, and the BSP entity reader.
"""
import importlib.util
import os
import struct
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load(name, rel):
    spec = importlib.util.spec_from_file_location(name, os.path.join(_HERE, rel))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


smoke = _load("xbox_smoke_transitions", "xbox-smoke-transitions.py")
maplist = _load("make_map_list", os.path.join("smoke", "make-map-list.py"))

LISTS = ["hl-campaign.txt", "hl-hazard-course.txt"]


class Lists(unittest.TestCase):
    def test_every_list_is_a_chain_without_repeats(self):
        for name in LISTS:
            start, steps = smoke.read_list(os.path.join(_HERE, "smoke", name))
            seq, how = smoke.sequence(start, steps)  # dies unless each step starts where the last ended
            self.assertEqual(len(seq), len(set(seq)), name + " visits a map twice")
            self.assertEqual(how[0], "start")
            self.assertGreater(len(seq), 5)

    def test_campaign_starts_on_the_first_map_and_ends_somewhere_late(self):
        start, steps = smoke.read_list(os.path.join(_HERE, "smoke", "hl-campaign.txt"))
        self.assertEqual(start, "c0a0")
        self.assertIn("c5a1", [s[1] for s in steps])


class Plan(unittest.TestCase):
    STEPS = [("a", "b", "lm1"), ("b", "c", "-"), ("c", "d", "lm3")]

    def test_commands_follow_the_step_kind(self):
        chain = smoke.plan("a", self.STEPS)
        self.assertEqual([c[0] for c in chain], ["a", "b", "c", "d"])
        self.assertEqual(chain[0][2], 'changelevel2 "b" "lm1"')
        self.assertEqual(chain[1][2], 'map "c"')
        self.assertIsNone(chain[3][2])
        self.assertEqual([c[1] for c in chain], ["start", "transition", "direct", "transition"])

    def test_resume_starts_at_the_named_map(self):
        chain = smoke.plan("a", self.STEPS, "c")
        self.assertEqual([c[0] for c in chain], ["c", "d"])
        self.assertEqual(chain[0][1], "start")  # arrived by `map`, whatever the list says

    def test_count_cuts_a_pilot_run_and_ends_it(self):
        chain = smoke.plan("a", self.STEPS, count=2)
        self.assertEqual([c[0] for c in chain], ["a", "b"])
        self.assertIsNone(chain[-1][2])

    def test_a_broken_chain_is_refused(self):
        with self.assertRaises(SystemExit):
            smoke.sequence("a", [("a", "b", "x"), ("z", "c", "y")])


class Cfgs(unittest.TestCase):
    def lines(self, entry, dwell=1800):
        return smoke.cfg_lines("smk-t", dwell, [], entry, "smk-t-end")

    def test_a_transition_waits_then_changes_level_without_toggles(self):
        l = self.lines(("b", "transition", 'changelevel2 "c" "lm"'))
        self.assertEqual(l[-2:], ["wait 1800", 'changelevel2 "c" "lm"'])
        self.assertNotIn("god", l)  # god toggles: the player carried over already has it

    def test_a_fresh_map_gets_god_and_notarget_once(self):
        for how in ("start", "direct"):
            l = self.lines(("b", how, 'map "c"'))
            self.assertEqual(l.count("god"), 1)
            self.assertEqual(l.count("notarget"), 1)
            self.assertLess(l.index("god"), l.index("wait 1800"))

    def test_only_the_last_map_ends_the_run(self):
        mid = self.lines(("b", "transition", 'map "c"'))
        last = self.lines(("c", "transition", None))
        self.assertNotIn("quit", mid)
        self.assertEqual(last[-1], "quit")
        self.assertIn('host_frametime_label "smk-t-end"', last)
        self.assertLess(last.index("host_frametime_analysis 0"), last.index('host_frametime_label "smk-t-end"'))


class BspEntities(unittest.TestCase):
    def bsp(self, entities):
        text = entities.encode("latin-1") + b"\0"
        header = struct.pack("<i", 30) + struct.pack("<ii", 4 + 15 * 8, len(text)) + b"\0" * (14 * 8)
        return header + text

    def test_changelevels_are_read_from_the_entity_lump(self):
        ents = ('{ "classname" "worldspawn" }\n'
                '{ "classname" "trigger_changelevel" "map" "C1A0B" "landmark" "a_to_b" }\n'
                '{ "classname" "info_player_start" }\n')
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "c1a0.bsp"), "wb") as f:
                f.write(self.bsp(ents))
            with open(os.path.join(d, "c1a0b.bsp"), "wb") as f:
                f.write(self.bsp('{ "classname" "worldspawn" }'))
            g = maplist.changelevels(d)
            self.assertEqual(g["c1a0"], [("c1a0b", "a_to_b")])  # map names are lower-cased
            self.assertEqual(g["c1a0b"], [])
            self.assertEqual(maplist.walk(g, "c1a0"), [("c1a0", "c1a0b", "a_to_b")])

    def test_a_walk_that_jumps_back_uses_a_plain_load(self):
        g = {"a": [("b", "x"), ("c", "y")], "b": [], "c": []}
        # b is a dead end: reaching c from b has no transition
        self.assertEqual(maplist.walk(g, "a"), [("a", "b", "x"), ("b", "c", "-")])


if __name__ == "__main__":
    unittest.main()
