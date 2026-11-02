import os, struct, sys, unittest
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from r3dpack import xbepatch as x

HERE = os.path.dirname(__file__)


def fake_xbe(logo_room=40):
    """A minimal XBE header: magic, base, cert, empty header page after it."""
    d = bytearray(0x1000)
    d[0:4] = b"XBEH"
    struct.pack_into("<I", d, 0x104, 0x10000)
    struct.pack_into("<I", d, 0x108, 0x400)       # sizeof headers
    struct.pack_into("<I", d, 0x118, 0x10000 + 0x178)  # certificate
    struct.pack_into("<II", d, 0x170, 0x10000 + 0x300, logo_room)
    return bytes(d)


class T(unittest.TestCase):
    def pgm(self, fn):
        pix = bytes(fn(i) for i in range(x.LOGO_PIXELS))
        return b"P5\n100 17\n255\n" + pix

    def test_codec_roundtrip(self):
        pix = bytes((i * 7 + (i // 13) * 29) & 0xFF for i in range(x.LOGO_PIXELS))
        self.assertEqual(x.decode_logo(x.encode_logo(pix)), [b >> 4 for b in pix])

    def test_long_runs(self):
        pix = bytes(1700 * [0x30])
        self.assertEqual(x.decode_logo(x.encode_logo(pix)), [3] * 1700)

    def test_set_logo_grows_into_header_page(self):
        d = fake_xbe(logo_room=4)
        out = x.set_logo(d, self.pgm(lambda i: 255 if (i // 5) % 2 else 0))
        self.assertEqual(x.get_logo(out), [15 if (i // 5) % 2 else 0 for i in range(1700)])
        self.assertGreater(struct.unpack_from("<I", out, 0x108)[0], 0x400)

    def test_refuses_what_does_not_fit(self):
        d = bytearray(fake_xbe(logo_room=4))
        struct.pack_into("<I", d, 0x108, 0xFF0)
        with self.assertRaises(x.XbeError):
            x.set_logo(bytes(d), self.pgm(lambda i: (i * 37) & 0xFF))

    def test_shipped_logo_and_title(self):
        with open(os.path.join(HERE, "..", "assets", "boot-logo.pgm"), "rb") as f:
            pgm = f.read()
        out = x.set_logo(x.set_title(fake_xbe(4), "Half-Life"), pgm)
        self.assertEqual(x.get_logo(out), [b >> 4 for b in x.read_pgm(pgm)])
        self.assertEqual(out[0x178 + 0x0C:0x178 + 0x0C + 18].decode("utf-16-le"), "Half-Life")

    def test_bad_pgm(self):
        with self.assertRaises(x.XbeError):
            x.read_pgm(b"P5\n99 17\n255\n" + bytes(99 * 17))


if __name__ == "__main__":
    unittest.main()


class Sections(unittest.TestCase):
    def xbe(self):
        d = bytearray(0x3000)
        d[0:4] = b"XBEH"
        import struct as s
        s.pack_into("<I", d, 0x104, 0x10000)
        s.pack_into("<I", d, 0x108, 0x2a0)
        s.pack_into("<I", d, 0x10C, 0x2000)
        s.pack_into("<I", d, 0x118, 0x10000 + 0x178)
        s.pack_into("<II", d, 0x11C, 1, 0x10000 + 0x1c0)
        s.pack_into("<6I", d, 0x1c0, 6, 0x11000, 0x1800, 0x1000, 0x1800, 0x10000 + 0x200)
        d[0x200:0x206] = b".text\0"
        return bytes(d) + b"\0" * 0x1000

    def test_make_xpr_layout(self):
        xpr = x.make_xpr(bytes(8192), 128)
        self.assertEqual(xpr[:4], b"XPR0")
        self.assertEqual(struct.unpack_from("<II", xpr, 4), (0x2800, 0x800))
        self.assertEqual(struct.unpack_from("<I", xpr, 0x18)[0], 0x07710C29)
        self.assertEqual(struct.unpack_from("<I", x.make_xpr(bytes(2048), 64), 0x18)[0], 0x06610C29)

    def test_add_sections(self):
        d = self.xbe()
        out = x.add_sections(d, [("$$XTIMAGE", b"A" * 10240), ("$$XSIMAGE", b"B" * 4096)])
        secs = x.list_sections(out)
        self.assertEqual([s["name"] for s in secs], [".text", "$$XTIMAGE", "$$XSIMAGE"])
        for s, blob in zip(secs[1:], (b"A" * 10240, b"B" * 4096)):
            self.assertEqual(out[s["raw"]:s["raw"] + s["rsize"]], blob)
            self.assertEqual(s["raw"] % 0x1000, 0)
            self.assertEqual(s["va"] % 0x1000, 0)
        self.assertGreater(secs[1]["va"], secs[0]["va"] + secs[0]["vsize"] - 1)
        self.assertGreaterEqual(secs[2]["va"], secs[1]["va"] + 10240)
        base = 0x10000
        self.assertEqual(struct.unpack_from("<I", out, 0x10C)[0] + base, secs[2]["va"] + 0x1000)
        self.assertEqual(x.list_sections(out)[0]["raw"], 0x1000)  # first section untouched
