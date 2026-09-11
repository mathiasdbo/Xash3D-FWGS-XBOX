"""
Failure-path tests for tools/xemu-debug.py's memory scan.

Run:  python -m unittest tools/test_xemu_debug.py

Why these exist: Codex review of the tool's first version found that
scan() dropped every failed chunk read and search/find-delta then printed
an unqualified "matches: none" - "pattern absent" indistinguishable from
"zero bytes inspected", the exact silent-success failure this repository's
measurement rules exist to prevent. Every test here drives scan() with a
fake reader (no emulator needed) and asserts the failure is REPORTED, not
that the happy path works.
"""
import importlib.util
import os
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SPEC = importlib.util.spec_from_file_location("xemu_debug", os.path.join(_HERE, "xemu-debug.py"))
xd = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(xd)


def _mem_reader(image, base, fail=()):
    """Reader over a bytes image mapped at `base`; raises for chunks whose
    start address is in `fail` (a set), mimicking an unmapped page."""
    def read(addr, n):
        if addr in fail:
            raise RuntimeError(f"virtual read of {n} bytes at {addr:#010x} refused ('E14')")
        off = addr - base
        return image[off:off + n]
    return read


class ScanFailurePaths(unittest.TestCase):
    def setUp(self):
        self.log = []
        self.image = bytes(range(256)) * 256  # 64 KiB = 16 chunks of 0x1000
        self.base = 0x10000

    def _scan(self, target, fail=(), chunk=0x1000):
        read = _mem_reader(self.image, self.base, fail)
        return xd.scan(read, self.base, self.base + len(self.image), chunk, target, "test", log=self.log.append)

    def test_every_read_failing_is_not_a_clean_result(self):
        every_chunk = {self.base + i for i in range(0, len(self.image), 0x1000)}
        res = self._scan(b"\x01\x02\x03", fail=every_chunk)
        self.assertTrue(res.nothing_read)
        self.assertEqual(res.bytes_scanned, 0)
        self.assertEqual(res.failed_chunks, 16)
        self.assertEqual(res.found, [])
        out = []
        code = xd.report_scan(res, self.base, self.base + len(self.image), "test", log=out.append)
        self.assertEqual(code, 1)
        text = "\n".join(out)
        self.assertIn("scanned 0 bytes", text)
        self.assertNotIn("matches:", text, "an unread scan must never print a matches line")

    def test_partial_failure_is_flagged_and_names_the_unread_range(self):
        # Pattern lives in chunk 2 (0x12000..); chunk 1 (0x11000) is unreadable.
        res = self._scan(b"\x10\x11\x12\x13", fail={self.base + 0x1000})
        self.assertFalse(res.nothing_read)
        self.assertFalse(res.complete)
        self.assertEqual(res.failed_chunks, 1)
        self.assertIn(self.base + 0x10, res.found)          # still found where readable
        self.assertNotIn(self.base + 0x1010, res.found)     # not claimed in the unread chunk
        out = []
        code = xd.report_scan(res, self.base, self.base + len(self.image), "test", log=out.append)
        self.assertEqual(code, 2)
        text = "\n".join(out)
        self.assertIn("INCOMPLETE", text)
        self.assertIn("0x00011000-0x00012000", text)
        self.assertIn("does not mean absent", text)

    def test_complete_clean_scan_exits_zero_and_reports_coverage(self):
        res = self._scan(b"\x10\x11\x12\x13")
        self.assertTrue(res.complete)
        self.assertEqual(res.bytes_scanned, len(self.image))
        self.assertEqual(res.ok_chunks, 16)
        out = []
        code = xd.report_scan(res, self.base, self.base + len(self.image), "test", log=out.append)
        self.assertEqual(code, 0)
        self.assertIn(f"scanned {len(self.image):#x} bytes in 16 chunk(s)", "\n".join(out))

    def test_match_straddling_a_chunk_boundary_is_found_once(self):
        # 0x1000-byte chunks over a 256-byte cycle: bytes 0xfe 0xff 0x00 0x01
        # straddle every 256-byte boundary, including each chunk boundary.
        res = self._scan(b"\xfe\xff\x00\x01", chunk=0x1000)
        hits = [h for h in res.found if h == self.base + 0x1000 - 2]
        self.assertEqual(len(hits), 1, "boundary match must be found exactly once via the overlap")

    def test_empty_pattern_is_rejected(self):
        with self.assertRaises(ValueError):
            self._scan(b"")

    def test_non_positive_chunk_is_rejected(self):
        with self.assertRaises(ValueError):
            self._scan(b"\x01\x02", chunk=0)

    def test_pattern_longer_than_chunk_is_found_across_many_reads(self):
        # Codex review, round 2: with chunk < len(pattern) the overlap used to
        # carry only the previous read, so a match spanning 3+ reads was never
        # seen while the scan still claimed completeness and exited 0.
        image = b"\x00" * 10 + b"\xde\xad\xbe\xef" + b"\x00" * 10
        read = _mem_reader(image, 0x2000)
        res = xd.scan(read, 0x2000, 0x2000 + len(image), 1, b"\xde\xad\xbe\xef", "test", log=self.log.append)
        self.assertTrue(res.complete)
        self.assertEqual(res.found, [0x2000 + 10], "a 4-byte pattern read 1 byte at a time must be found exactly once")
        out = []
        self.assertEqual(xd.report_scan(res, 0x2000, 0x2000 + len(image), "test", log=out.append), 0)


if __name__ == "__main__":
    unittest.main()
