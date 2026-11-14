import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import save_legacy_table as slt  # noqa: E402

_spec = importlib.util.spec_from_file_location("xbox_gen_callback_table", os.path.join(HERE, "xbox-gen-callback-table.py"))
gen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen)


def obj_with(funcs, stores):
    """an i386 COFF object. funcs: names defined as functions in .text; stores: [(symbol, disp)] one
    `mov [eax+disp8], imm32` each with a DIR32 relocation against the symbol (defined here or not)."""
    names = list(funcs) + [s for s, _ in stores if s not in funcs]
    code = b""
    relocs = []
    for sym, disp in stores:
        code += (bytes([0xC7, 0x40, disp]) if disp is not None else b"\xB8") + b"\0\0\0\0"  # None: through a register
        relocs.append((len(code) - 4, names.index(sym), slt.DIR32))
    code = code or b"\xC3"
    hdr = 20 + 40
    rel_at = hdr + len(code)
    sec = struct.pack("<8sIIIIIIHHI", b".text", 0, 0, len(code), hdr, rel_at, 0, len(relocs), 0, 0x60000020)
    body = code + b"".join(struct.pack("<IIH", a, i, t) for a, i, t in relocs)
    symtab, strtab = b"", b""
    for n in names:
        o = len(strtab) + 4
        strtab += n.encode() + b"\0"
        defined = n in funcs
        symtab += struct.pack("<4sIIhHBB", b"\0\0\0\0", o, 0, 1 if defined else 0, 0x20 if defined else 0, 2, 0)
    return struct.pack("<HHIIIHH", 0x14C, 1, 0, hdr + len(body), len(names), 0, 0) + sec + body + symtab + \
        struct.pack("<I", len(strtab) + 4) + strtab


THINK = "sv_?Think@CFoo@@QAEXXZ"
OTHER = "sv_?Wake@CBar@@QAEXXZ"
TOUCH = "sv_?Touch@CFoo@@QAEXPAVCBaseEntity@@@Z"


class Names(unittest.TestCase):
    def test_neutral_is_what_the_engine_turns_a_mangled_name_into(self):
        self.assertEqual(gen.neutral("?Think@CFoo@@QAEXXZ"), "Think@CFoo")
        self.assertEqual(gen.neutral("?Use@CDoor@CBase@@QAEXXZ"), "Use@CDoor@CBase")
        for bad in ("_PM_Move", "?x", "Think@CFoo"):
            with self.assertRaises(gen.GenError):
                gen.neutral(bad)


class Callbacks(unittest.TestCase):
    def dir_with(self, tmp, objects):
        for i, (funcs, stores) in enumerate(objects):
            with open(os.path.join(tmp, "o%d.obj" % i), "wb") as f:
                f.write(obj_with(funcs, stores))

    def run_gen(self, objects, minimum=1):
        old = gen.MIN_CALLBACKS
        gen.MIN_CALLBACKS = minimum
        try:
            with tempfile.TemporaryDirectory() as tmp:
                self.dir_with(tmp, objects)
                return gen.callbacks(tmp)
        finally:
            gen.MIN_CALLBACKS = old

    def test_a_stored_function_is_named_by_its_neutral_name_and_keeps_its_symbol(self):
        self.assertEqual(self.run_gen([([THINK], [(THINK, 20)])]), {"Think@CFoo": THINK})

    def test_a_function_stored_in_another_object_than_the_one_defining_it_is_found(self):
        self.assertEqual(self.run_gen([([THINK], []), ([], [(THINK, 20)])]), {"Think@CFoo": THINK})

    def test_a_shape_only_member_with_no_readable_store_is_found(self):
        self.assertEqual(self.run_gen([([TOUCH], [(TOUCH, 24)]), ([OTHER], [(OTHER, 20)])]).keys(),
                         {"Touch@CFoo", "Wake@CBar"})

    def test_an_address_taken_function_that_is_not_a_callback_is_left_out(self):
        got = self.run_gen([(["_PM_Move", THINK], [("_PM_Move", None), (THINK, 20)])])
        self.assertEqual(list(got), ["Think@CFoo"])

    def test_a_non_member_stored_into_a_callback_field_stops_the_build(self):
        with self.assertRaises(gen.GenError):
            self.run_gen([(["_PM_Move"], [("_PM_Move", 20)])])

    def test_a_symbol_the_objects_do_not_define_is_not_a_callback(self):
        self.assertEqual(self.run_gen([([THINK], [(THINK, 20), (OTHER, 20)])]), {"Think@CFoo": THINK})

    def test_two_functions_with_one_neutral_name_stop_the_build(self):
        a, b = "sv_?Think@CFoo@@QAEXXZ", "sv_?Think@CFoo@@IAEXXZ"  # a public and a protected overload: one neutral name
        with self.assertRaises(gen.GenError) as cm:
            self.run_gen([([a, b], [(a, 20), (b, 20)])])
        self.assertIn("same neutral name", str(cm.exception))

    def test_too_few_callbacks_means_an_incomplete_object_set(self):
        with self.assertRaises(gen.GenError) as cm:
            self.run_gen([([THINK], [(THINK, 20)])], minimum=5)
        self.assertIn("incomplete object set", str(cm.exception))

    def test_an_empty_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(gen.GenError):
                gen.callbacks(tmp)


class Exports(unittest.TestCase):
    def test_the_callbacks_follow_the_exports_in_order(self):
        text = gen.write_exports("monster_foo\nGiveFnptrsToDll _GiveFnptrsToDll@8\n", {"B@C": "sv_?B@C@@QAEXXZ", "A@C": "sv_?A@C@@QAEXXZ"})
        self.assertEqual(text.splitlines()[2:], ["A@C sv_?A@C@@QAEXXZ", "B@C sv_?B@C@@QAEXXZ"])

    def test_a_callback_with_the_name_of_an_export_is_refused(self):
        with self.assertRaises(gen.GenError):
            gen.write_exports("Think@CFoo\n", {"Think@CFoo": THINK})

    def test_the_table_generator_gives_a_name_that_is_not_an_identifier_a_generated_one(self):
        bash = shutil.which("bash")
        if not bash:
            self.skipTest("needs bash")
        repo = os.path.dirname(HERE)
        with tempfile.TemporaryDirectory() as tmp:
            exports = os.path.join(tmp, "exports.txt")
            with open(exports, "w", newline="\n") as f:
                f.write("monster_foo\nThink@CFoo %s\n" % THINK)
            r = subprocess.run([bash, "tools/gen-library-tables.sh", tmp, "server:" + exports], cwd=repo,
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(os.path.join(tmp, "generated_lib_server_exports.c"), encoding="utf-8") as f:
                c = f.read()
            self.assertIn('extern void xs_export_2(void) asm("%s");' % THINK, c)
            self.assertIn('{ "Think@CFoo", &xs_export_2 },', c)
            self.assertIn('{ "monster_foo", &monster_foo },', c)  # plain names are unchanged


class MapCheck(unittest.TestCase):
    CBS = {"Think@CFoo": THINK, "Wake@CBar": OTHER}

    @staticmethod
    def line(sym, addr):
        return " 0001:%08x       %s         %016x     a.obj\n" % (addr, sym, addr)

    def test_each_at_its_own_address_is_sound(self):
        self.assertEqual(gen.check_map(self.line(THINK, 0x1000) + self.line(OTHER, 0x1010), self.CBS), [])

    def test_two_callbacks_folded_to_one_address_are_caught(self):
        p = gen.check_map(self.line(THINK, 0x1000) + self.line(OTHER, 0x1000), self.CBS)
        self.assertEqual(len(p), 1)
        self.assertIn("folded", p[0])

    def test_a_callback_missing_from_the_map_is_caught(self):
        self.assertEqual(len(gen.check_map(self.line(THINK, 0x1000), self.CBS)), 1)


if __name__ == "__main__":
    unittest.main()
