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

    def build(self, uses, symbols):
        return slt.build_table(uses, symbols, self.GI)

    def test_rows_are_offsets_from_gamedllinit(self):
        rows = self.build({"f": [(True, 20)], "g": [(True, 24)]}, {"f": self.GI - 0x40, "g": self.GI + 0x10})
        self.assertEqual(rows, [(-0x40, "think", "f"), (0x10, "touch", "g")])

    def test_a_name_the_map_does_not_have_is_a_variable_not_a_function(self):
        self.assertEqual(self.build({"v": [(True, None)]}, {}), [])

    def test_a_fold_set_told_apart_by_field_is_accepted(self):
        rows = self.build({"a": [(True, 20)], "b": [(True, 204)]}, {"a": self.GI - 8, "b": self.GI - 8})
        self.assertEqual(rows, [(-8, "movedone", "b"), (-8, "think", "a")])

    def test_a_fold_set_with_the_same_field_stops_the_generator(self):
        with self.assertRaises(slt.TableError) as cm:
            self.build({"a": [(True, 20)], "b": [(True, 20)]}, {"a": self.GI - 8, "b": self.GI - 8})
        self.assertIn("fold set", str(cm.exception))

    def test_a_fold_set_with_an_unknown_field_stops_the_generator(self):
        with self.assertRaises(slt.TableError):
            self.build({"a": [(True, 20)], "b": [(True, None)]}, {"a": self.GI - 8, "b": self.GI - 8})

    def test_a_field_offset_nobody_named_stops_the_generator(self):
        with self.assertRaises(slt.TableError) as cm:
            self.build({"a": [(True, 36)]}, {"a": self.GI})
        self.assertIn("unrecognised field offset", str(cm.exception))

    def test_a_callback_outside_the_server_block_stops_the_generator(self):
        with self.assertRaises(slt.TableError) as cm:
            self.build({"?Think@CFoo@@QAEXXZ": [(True, 20)]}, {"?Think@CFoo@@QAEXXZ": 0x13F420})
        self.assertIn("outside the server block", str(cm.exception))

    def test_a_compiler_made_static_init_stub_outside_the_block_is_not_a_callback(self):
        rows = self.build({"??__Fg_Dir@@YAXXZ": [(True, None)]}, {"??__Fg_Dir@@YAXXZ": 0x373FC0})
        self.assertEqual(len(rows), 1)


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
        self.assertGreater(len(t["rows"]), 250)

    def test_the_known_fold_set_is_told_apart_by_field(self):
        t = slt.load_table()
        self.assertEqual(slt.resolve(t, -210368, "think"), ["?CallMonsterThink@CBaseMonster@@QAEXXZ"])
        self.assertEqual(slt.resolve(t, -210368, "movedone"), ["?CallHitBottom@CFuncPlat@@QAEXXZ"])
        self.assertEqual(len(slt.resolve(t, -210368)), 2)

    def test_a_saved_negative_offset_is_read_as_its_unsigned_wrap(self):
        t = slt.load_table()
        self.assertEqual(slt.resolve(t, (1 << 32) - 210368, "think"), slt.resolve(t, -210368, "think"))
        self.assertEqual(slt.resolve(t, 4294950224), slt.resolve(t, -17072))

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
        edited = t.replace("functions 299", "functions 300") + "fn %s %s ?Other@@YAXXZ\n" % (off, field)
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
