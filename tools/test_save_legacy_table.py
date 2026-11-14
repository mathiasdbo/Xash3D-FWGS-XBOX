import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_legacy_table as slt  # noqa: E402


def coff(sections, symbols):
    """a minimal i386 COFF object. sections: [(name, flags, bytes, [(offset, symbol index, reloc type)])];
    symbols: [names] (long names go to the string table). Every symbol is undefined (section 0)."""
    body = b""
    hdrs = b""
    off = 20 + 40 * len(sections)
    placed = []
    for name, flags, data, relocs in sections:
        raw = off
        off += len(data)
        rel = off
        off += 10 * len(relocs)
        placed.append((name, flags, data, relocs, raw, rel))
    for name, flags, data, relocs, raw, rel in placed:
        hdrs += struct.pack("<8sIIIIIIHHI", name.encode(), 0, 0, len(data), raw if data else 0, rel if relocs else 0, 0,
                            len(relocs), 0, flags)
        body += data + b"".join(struct.pack("<IIH", a, s, t) for a, s, t in relocs)
    strtab = b""
    symtab = b""
    for n in symbols:
        o = len(strtab) + 4
        strtab += n.encode() + b"\0"
        symtab += struct.pack("<4sIIhHBB", b"\0\0\0\0", o, 0, 0, 0, 2, 0)
    return struct.pack("<HHIIIHH", 0x14C, len(sections), 0, off, len(symbols), 0, 0) + hdrs + body + symtab + \
        struct.pack("<I", len(strtab) + 4) + strtab


CODE, DATA = 0x60000020, 0xC0000040


def store(disp):
    return bytes([0xC7, 0x40, disp]) + b"\0\0\0\0"  # mov dword ptr [eax+disp8], imm32  (the reloc patches the imm32)


class Objects(unittest.TestCase):
    def test_a_store_of_a_functions_address_into_a_think_slot(self):
        obj = coff([(".text", CODE, store(20), [(3, 0, slt.DIR32)])], ["?Think@CFoo@@QAEXXZ"])
        self.assertEqual(slt.address_taken(obj), [("?Think@CFoo@@QAEXXZ", True, 20)])

    def test_the_displacement_names_the_field(self):
        for disp, field in ((20, "think"), (24, "touch"), (28, "use"), (32, "blocked"), (204, "movedone")):
            self.assertEqual(slt.FIELDS[disp], field)
        wide = bytes([0xC7, 0x80]) + struct.pack("<I", 204) + b"\0\0\0\0"
        obj = coff([(".text", CODE, wide, [(6, 0, slt.DIR32)])], ["?Done@CFoo@@QAEXXZ"])
        self.assertEqual(slt.address_taken(obj), [("?Done@CFoo@@QAEXXZ", True, 204)])

    def test_an_address_that_goes_through_a_register_has_no_field(self):
        code = bytes([0xB8, 0, 0, 0, 0])  # mov eax, imm32
        obj = coff([(".text", CODE, code, [(1, 0, slt.DIR32)])], ["?Use@CFoo@@QAEXXZ"])
        self.assertEqual(slt.address_taken(obj), [("?Use@CFoo@@QAEXXZ", True, None)])

    def test_a_vtable_entry_is_not_an_assignment(self):
        obj = coff([(".rdata", 0x40000040, b"\0\0\0\0", [(0, 0, slt.DIR32)])], ["?Spawn@CFoo@@UAEXXZ"])
        self.assertEqual(slt.address_taken(obj), [])

    def test_calls_are_not_address_taking(self):
        obj = coff([(".text", CODE, b"\xE8\0\0\0\0", [(1, 0, 0x14)])], ["?Callee@@YAXXZ"])  # REL32
        self.assertEqual(slt.address_taken(obj), [])

    def test_a_pointer_in_data_counts(self):
        obj = coff([(".data", DATA, b"\0\0\0\0", [(0, 0, slt.DIR32)])], ["?Entry@@YAXXZ"])
        self.assertEqual(slt.address_taken(obj), [("?Entry@@YAXXZ", False, None)])


class Building(unittest.TestCase):
    GI = 0x2AFEF0
    THINK = "?Think@CFoo@@QAEXXZ"
    TOUCH = "?Touch@CFoo@@QAEXPAVCBaseEntity@@@Z"
    USE = "?Use@CFoo@@QAEXPAVCBaseEntity@@0W4USE_TYPE@@M@Z"

    def build(self, uses, symbols):
        return slt.build_table(uses, symbols, self.GI)

    def test_rows_are_offsets_from_gamedllinit_with_the_field_the_store_names(self):
        rows = self.build({self.THINK: [(True, 20)], self.TOUCH: [(True, 24)]},
                          {self.THINK: self.GI - 0x40, self.TOUCH: self.GI + 0x10})
        self.assertEqual(rows, [(-0x40, "think", self.THINK), (0x10, "touch", self.TOUCH)])

    def test_a_member_whose_address_went_through_a_register_is_listed_by_its_shape(self):
        rows = self.build({self.THINK: [(True, None)], self.USE: [(True, None)], self.TOUCH: [(False, None)]},
                          {self.THINK: self.GI, self.USE: self.GI + 16, self.TOUCH: self.GI + 32})
        self.assertEqual([(f, n) for _, f, n in rows], [("~think,movedone", self.THINK), ("~use", self.USE),
                                                        ("~touch,blocked", self.TOUCH)])

    def test_things_that_are_not_callbacks_are_left_out(self):
        # exports, dispatch functions, initialisers, virtuals with no store: their address is taken, they are not callbacks
        names = ["_PM_Move", "_CreateInterface", "?GameDLLInit@@YAXXZ", "?DispatchThink@@YAXPAUedict_s@@@Z",
                 "??__Fg_MapsToLoad@@YAXXZ", "__GLOBAL__sub_I_client.cpp", "?Spawn@CFoo@@UAEXXZ",
                 "?Static@CFoo@@SAXXZ", "?Other@CFoo@@QAEHXZ", "?Pair@CFoo@@QAEXHH@Z"]
        uses = {n: [(True, None)] for n in names}
        self.assertEqual(self.build(uses, {n: self.GI + 8 * i for i, n in enumerate(names)}), [])

    def test_a_name_the_map_does_not_have_is_a_variable_not_a_function(self):
        self.assertEqual(self.build({self.THINK: [(True, None)]}, {}), [])

    def test_a_fold_set_told_apart_by_field_is_accepted(self):
        a, b = "?A@CFoo@@QAEXXZ", "?B@CBar@@QAEXXZ"
        rows = self.build({a: [(True, 20)], b: [(True, 204)]}, {a: self.GI - 8, b: self.GI - 8})
        self.assertEqual(rows, [(-8, "movedone", b), (-8, "think", a)])

    def test_a_fold_set_with_the_same_field_stops_the_generator(self):
        a, b = "?A@CFoo@@QAEXXZ", "?B@CBar@@QAEXXZ"
        with self.assertRaises(slt.TableError) as cm:
            self.build({a: [(True, 20)], b: [(True, 20)]}, {a: self.GI - 8, b: self.GI - 8})
        self.assertIn("fold set", str(cm.exception))

    def test_a_fold_set_with_a_field_known_for_only_one_member_stops_the_generator(self):
        a, b = "?A@CFoo@@QAEXXZ", "?B@CBar@@QAEXXZ"
        with self.assertRaises(slt.TableError):
            self.build({a: [(True, 20)], b: [(True, None)]}, {a: self.GI - 8, b: self.GI - 8})

    def test_a_field_offset_nobody_named_stops_the_generator(self):
        with self.assertRaises(slt.TableError) as cm:
            self.build({self.THINK: [(True, 36)]}, {self.THINK: self.GI})
        self.assertIn("unrecognised field offset", str(cm.exception))

    def test_a_store_into_a_field_the_shape_cannot_fit_stops_the_generator(self):
        with self.assertRaises(slt.TableError) as cm:
            self.build({self.THINK: [(True, 28)]}, {self.THINK: self.GI})  # a void() member stored as a use callback
        self.assertIn("not a member function of that shape", str(cm.exception))

    def test_a_callback_outside_the_server_block_stops_the_generator(self):
        with self.assertRaises(slt.TableError) as cm:
            self.build({self.THINK: [(True, 20)]}, {self.THINK: 0x13F420})
        self.assertIn("outside the server block", str(cm.exception))


class Census(unittest.TestCase):
    MAP = "\n".join(["0001:00000000 sv_?A@CFoo@@QAEXXZ 0000000000300000 f a.obj",
                     "0001:00000010 sv_?B@CFoo@@QAEXXZ 0000000000300010 f b.obj",
                     "0001:00000020 _PM_Move 0000000000300020 f c.obj",
                     "0001:00000030 sv_?A2@CBar@@QAEXXZ 0000000000300030 f a.obj"]) + "\n"

    def test_the_server_objects_are_the_ones_with_renamed_symbols(self):
        self.assertEqual(slt.server_objects(self.MAP), {"a.obj", "b.obj"})

    def test_a_complete_set_passes_and_extra_objects_are_fine(self):
        self.assertEqual(slt.census_problems({"a.obj", "b.obj"}, ["a.obj", "b.obj", "tempmonster.obj"]), [])

    def test_removing_one_object_makes_generation_fail(self):
        p = slt.census_problems({"a.obj", "b.obj"}, ["a.obj"])
        self.assertEqual(len(p), 1)
        self.assertIn("b.obj", p[0])

    def run_generate(self, objects):
        """runs `generate` over a directory holding the named objects; returns (exit code, stderr, output file exists)"""
        import contextlib
        import hashlib
        import io
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            obj = coff([(".text", CODE, store(20), [(3, 0, slt.DIR32)])], ["?Think@CFoo@@QAEXXZ"])
            os.makedirs(os.path.join(tmp, "objs"))
            for name in objects:
                with open(os.path.join(tmp, "objs", name), "wb") as f:
                    f.write(obj)
            mp = os.path.join(tmp, "m.map")
            with open(mp, "w") as f:
                f.write(self.MAP + "0001:00000040 ?GameDLLInit@@YAXXZ 0000000000300040 f a.obj\n")
            xbe = os.path.join(tmp, "x.xbe")
            with open(xbe, "wb") as f:
                f.write(b"x")
            saved = slt.SOURCE_XBE_SHA256, slt.MIN_SERVER_OBJECTS
            slt.SOURCE_XBE_SHA256, slt.MIN_SERVER_OBJECTS = hashlib.sha256(b"x").hexdigest(), 2
            err = io.StringIO()
            try:
                with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                    code = slt.main(["generate", os.path.join(tmp, "objs"), "--map", mp, "--xbe", xbe, "--out",
                                     os.path.join(tmp, "o.txt")])
            finally:
                slt.SOURCE_XBE_SHA256, slt.MIN_SERVER_OBJECTS = saved
            return code, err.getvalue(), os.path.exists(os.path.join(tmp, "o.txt"))

    def test_generation_refuses_an_object_directory_with_one_object_removed(self):
        code, err, wrote = self.run_generate(["a.obj"])
        self.assertEqual(code, 1)
        self.assertIn("b.obj", err)
        self.assertIn("not scanned", err)
        self.assertFalse(wrote)

    def test_generation_with_every_object_present_goes_through(self):
        code, err, wrote = self.run_generate(["a.obj", "b.obj"])
        self.assertEqual((code, err, wrote), (0, "", True))


class EngineTable(unittest.TestCase):
    """XS.3: the C table the engine reads is the frozen XS.1 table, nothing else"""

    def test_the_committed_include_is_what_the_table_generates(self):
        with open(slt.C_INC, encoding="utf-8", newline="") as f:
            self.assertEqual(f.read().replace("\r\n", "\n"), slt.emit_c(slt.load_table()))

    def test_every_offset_is_a_row_and_the_fold_is_null(self):
        text = slt.emit_c(slt.load_table())
        rows = [l for l in text.splitlines() if l.startswith("\t{")]
        t = slt.load_table()
        self.assertEqual(len(rows), len({o for o, _, _ in t["rows"]}))
        self.assertIn("\t{ -210368, NULL },", rows)
        self.assertEqual(sum(1 for l in rows if "NULL" in l), 1)

    def test_names_are_neutral_and_agree_with_the_generators(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("g", os.path.join(HERE, "xbox-gen-callback-table.py"))
        g = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(g)
        for _, _, name in slt.load_table()["rows"]:
            self.assertEqual(slt.neutral(name), g.neutral(name))
            self.assertNotIn("?", slt.neutral(name))
        with self.assertRaises(slt.TableError):
            slt.neutral("_PM_Move")

    def test_emit_c_check_fails_on_an_edited_include(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "x.inc")
            with open(p, "w", newline="\n") as f:
                f.write(slt.emit_c(slt.load_table()).replace('"', "'", 1))
            self.assertEqual(slt.main(["emit-c", "--out", p, "--check"]), 1)


class Committed(unittest.TestCase):
    def text(self):
        with open(slt.TABLE, encoding="utf-8") as f:
            return f.read()

    def test_the_committed_table_is_sound_and_pinned(self):
        self.assertEqual(slt.table_problems(self.text()), [])
        self.assertEqual(slt.text_digest(self.text()), slt.TABLE_SHA256)

    def test_the_digest_ignores_line_endings(self):
        self.assertEqual(slt.text_digest(self.text().replace("\n", "\r\n")), slt.text_digest(self.text()))

    def test_it_names_the_three_public_builds_save_layout(self):
        t = slt.load_table()
        self.assertEqual(t["build"], (slt.SOURCE_BUILD, slt.SOURCE_XBE_SHA256))
        self.assertEqual(t["count"], len(t["rows"]))
        self.assertGreater(len(t["rows"]), 200)

    def test_the_known_fold_set_is_told_apart_by_field(self):
        t = slt.load_table()
        self.assertEqual(slt.resolve(t, -210368, "think"), ["?CallMonsterThink@CBaseMonster@@QAEXXZ"])
        self.assertEqual(slt.resolve(t, -210368, "movedone"), ["?CallHitBottom@CFuncPlat@@QAEXXZ"])
        self.assertEqual(len(slt.resolve(t, -210368)), 2)

    def test_a_saved_negative_offset_is_read_as_its_unsigned_wrap(self):
        t = slt.load_table()
        self.assertEqual(slt.resolve(t, (1 << 32) - 210368, "think"), slt.resolve(t, -210368, "think"))
        self.assertEqual(slt.resolve(t, 4294950224), slt.resolve(t, -17072))

    def test_a_function_resolves_only_for_a_field_it_can_hold(self):
        t = slt.load_table()
        think = [(o, n) for o, f, n in t["rows"] if f == "think"][0]
        self.assertEqual(slt.resolve(t, think[0], "think"), [think[1]])
        self.assertEqual(slt.resolve(t, think[0], "use"), [])
        self.assertEqual(slt.resolve(t, think[0], "touch"), [])

    def test_exports_and_initialisers_are_not_in_the_table(self):
        names = {n for _, _, n in slt.load_table()["rows"]}
        for n in ("_PM_Move", "_PM_Init", "_CreateInterface", "?GameDLLInit@@YAXXZ", "?DispatchThink@@YAXPAUedict_s@@@Z",
                  "??__Fg_GameDirectory@@YAXXZ", "__GLOBAL__sub_I_filesystem_utils.cpp"):
            self.assertNotIn(n, names)
        self.assertFalse([n for n in names if not n.startswith("?") or n.startswith("??")])

    def test_an_exports_offset_resolves_to_nothing_in_any_field(self):
        # PM_Move / CreateInterface / the initialisers sit at offsets the table does not hold (checked against the map
        # of the build the table is made from, whose addresses are outside the repository, so by name above, and here
        # by shape: every row is a member function of a callback shape)
        for _, f, n in slt.load_table()["rows"]:
            self.assertTrue(slt.MEMBER.match(n), n)
            self.assertIn(slt.MEMBER.match(n).group("sig"), slt.SIGNATURES, n)

    def test_an_offset_the_table_does_not_hold_resolves_to_nothing(self):
        self.assertEqual(slt.resolve(slt.load_table(), 12345), [])

    def test_an_edited_table_is_refused(self):
        text = self.text()
        for edited in (text + "# comment\n", text.replace("think", "touch", 1)):
            self.assertTrue(any("pinned digest" in p for p in slt.table_problems(edited)))

    def test_a_table_with_two_indistinguishable_functions_at_one_offset_is_refused(self):
        t = self.text()
        line = [l for l in t.splitlines() if l.startswith("fn ")][0]
        _, off, field, _name = line.split()
        n = slt.load_table()["count"]
        edited = t.replace("functions %d" % n, "functions %d" % (n + 1)) + "fn %s %s ?Other@CFoo@@QAEXXZ\n" % (off, field)
        self.assertTrue(any("cannot be told apart" in p for p in slt.table_problems(edited, None)))

    def test_a_table_from_another_build_is_refused(self):
        edited = self.text().replace(slt.SOURCE_XBE_SHA256, "0" * 64)
        self.assertTrue(any("does not say it was made from" in p for p in slt.table_problems(edited, None)))

    def test_every_saved_callback_in_the_console_fixtures_resolves(self):
        folder = os.environ.get("R3D_SAVE_FIXTURES")
        if not folder or not os.path.isdir(folder):
            self.skipTest("R3D_SAVE_FIXTURES names a folder of .sav files (they stay outside the repository)")
        import re
        t = slt.load_table()
        seen = set()
        for dp, _, files in os.walk(folder):
            for fn in files:
                if fn.lower().endswith(".sav"):
                    with open(os.path.join(dp, fn), "rb") as f:
                        seen.update(int(m) for m in re.findall(rb"ofs:(\d+)", f.read()))
        self.assertTrue(seen, "no ofs: strings in the fixtures")
        self.assertEqual([o for o in seen if not slt.resolve(t, o)], [])


if __name__ == "__main__":
    unittest.main()
