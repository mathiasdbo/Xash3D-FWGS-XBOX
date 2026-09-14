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
import socket
import threading
import time
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


class HandshakeSynchronization(unittest.TestCase):
    """A prior connection that sent 'c'/'s' and disconnected before the
    stop-reply arrived (killed, timed out, crashed) leaves that reply
    queued in the stub, which delivers it as the first packet(s) on the
    NEXT connection, ahead of that connection's own qSupported reply.
    _handshake() must recognize and discard those by shape (not by
    guessing a delay is long enough) and keep waiting for the real
    qSupported reply specifically. Reproduced with a loopback socketpair
    standing in for the stub, no emulator needed."""

    def setUp(self):
        self.client, self.stub = socket.socketpair()
        self.addCleanup(self.client.close)
        self.addCleanup(self.stub.close)
        self.conn = object.__new__(xd.GdbRemote)
        self.conn.sock = self.client

    def test_no_stale_packets_returns_the_real_reply_directly(self):
        self.stub.sendall(b"$PacketSize=4000#00")
        self.assertEqual(self.conn._handshake(deadline=1.0), "PacketSize=4000")

    def test_stale_packet_is_acked_then_discarded_and_the_real_reply_is_returned(self):
        # A real stub withholds whatever it queues next until the previous
        # packet is acked (see _recv's own comment on this) - both packets
        # are staged up front here, so returning the real reply at all
        # proves the stale one was actually consumed, and the ack count
        # below proves it was acked rather than raw-byte-drained.
        self.stub.sendall(b"$T02thread:p01.01;#03")
        self.stub.sendall(b"$PacketSize=4000#00")
        result = self.conn._handshake(deadline=1.0)
        self.assertEqual(result, "PacketSize=4000")
        self.stub.settimeout(1)
        sent_to_stub = self.stub.recv(4096)
        # The query itself legitimately contains literal '+' characters
        # (qSupported feature flags) - check for the two bare single-byte
        # ACKs that must follow it, not a raw '+' count over everything.
        query = b"$qSupported:multiprocess+;swbreak+;hwbreak+#" + f"{xd.GdbRemote._checksum('qSupported:multiprocess+;swbreak+;hwbreak+'):02x}".encode()
        self.assertEqual(sent_to_stub, query + b"++", "the stale packet and the real reply must each get exactly one bare ACK")

    def test_real_reply_arriving_after_a_delay_is_still_caught(self):
        # Codex review round 2: a fixed short window of silence is not
        # proof the connection starts clean - the real reply can
        # legitimately arrive after that window closes. Send it from a
        # background thread well past what the old fixed window was.
        def send_late():
            time.sleep(0.3)
            self.stub.sendall(b"$PacketSize=4000#00")
        t = threading.Thread(target=send_late)
        t.start()
        try:
            self.assertEqual(self.conn._handshake(deadline=2.0), "PacketSize=4000")
        finally:
            t.join()

    def test_no_reply_ever_arriving_fails_loudly_after_the_deadline(self):
        started = time.time()
        with self.assertRaises(RuntimeError):
            self.conn._handshake(deadline=0.3)
        self.assertLess(time.time() - started, 1.5, "must fail near the deadline, not hang indefinitely")


def _fake_pe(imgbase, sections):
    """Minimal 32-bit PE: `sections` is [(name, rva, raw_bytes, writable)],
    each mapped at ImageBase+rva with the raw bytes placed at a file offset
    of our choosing."""
    import struct
    nsec = len(sections)
    optsz = 224
    hdr_end = 0x40 + 4 + 20 + optsz + nsec * 40
    data = bytearray(hdr_end)
    struct.pack_into("<I", data, 0x3c, 0x40)
    data[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", data, 0x40 + 6, nsec)
    struct.pack_into("<H", data, 0x40 + 20, optsz)
    struct.pack_into("<I", data, 0x40 + 24 + 28, imgbase)
    body = bytearray()
    for i, (name, rva, raw, writable) in enumerate(sections):
        o = 0x40 + 24 + optsz + i * 40
        data[o:o + 8] = name.encode().ljust(8, b"\0")
        rptr = hdr_end + len(body)
        struct.pack_into("<IIII", data, o + 8, len(raw), rva, len(raw), rptr)
        struct.pack_into("<I", data, o + 36, 0x80000000 if writable else 0x60000020)
        body += raw
    return bytes(data + body)


class VerifyImage(unittest.TestCase):
    """verify_image() exists because a whole round of 'verified live' results
    was taken against an ISO xemu was not actually booting: every mapped
    address still hit a breakpoint, just in the wrong function. Codex review
    of its first version found that sampling 12 symbols certifies a stale
    build whose differences lie elsewhere - so the check covers every
    file-backed read-only byte, a mismatch anywhere is a mismatch, and
    'could not read' is never 'matches'."""

    def setUp(self):
        self.text = bytes(range(0x100)) * 8          # 2 KiB of "code"
        self.rdata = bytes(range(0xff, -1, -1)) * 2   # 512 B read-only data
        self.data = b"\x11" * 256                     # writable, must be ignored
        self.pe = _fake_pe(0x10000, [(".text", 0x1000, self.text, False),
                                     (".rdata", 0x2000, self.rdata, False),
                                     (".data", 0x3000, self.data, True)])
        self.image = bytearray(0x4000)
        self.image[0x1000:0x1000 + len(self.text)] = self.text
        self.image[0x2000:0x2000 + len(self.rdata)] = self.rdata
        self.image[0x3000:0x3000 + len(self.data)] = b"\x22" * 256  # runtime-modified .data

    def _read(self, image, fail=()):
        return _mem_reader(bytes(image), 0x10000, fail)

    def _verdicts(self, results):
        return sorted({v for _n, _s, _e, v in results})

    def test_identical_readonly_sections_are_identity_even_if_data_changed(self):
        res = xd.verify_image(self._read(self.image), self.pe, chunk=0x200)
        self.assertEqual(self._verdicts(res), ["ok"])
        self.assertEqual(sum(e - s for _n, s, e, _v in res), len(self.text) + len(self.rdata),
                         "every read-only byte is covered, and only those")

    def test_a_single_changed_byte_far_from_any_symbol_is_a_mismatch(self):
        stale = bytearray(self.image)
        stale[0x1000 + 0x6f3] ^= 0xff   # one byte, mid-.text - the partial-change case
        res = xd.verify_image(self._read(stale), self.pe, chunk=0x200)
        bad = [(s, e) for _n, s, e, v in res if v == "mismatch"]
        self.assertEqual(bad, [(0x11600, 0x11800)], "exactly the chunk holding the changed byte")

    def test_a_changed_rdata_byte_is_a_mismatch_too(self):
        stale = bytearray(self.image)
        stale[0x2000 + 3] ^= 0x01
        res = xd.verify_image(self._read(stale), self.pe, chunk=0x200)
        self.assertIn("mismatch", self._verdicts(res))

    def _thunk_fixture(self):
        import struct
        # An __imp_ table: the file holds ordinals 0x80000034.., the loader
        # writes kernel addresses over them. Put two in .rdata and patch them
        # with what a (fake) running kernel exports for those ordinals.
        pe_rdata = bytearray(self.rdata)
        struct.pack_into("<II", pe_rdata, 0x40, 0x80000034, 0x800000a1)
        pe = _fake_pe(0x10000, [(".text", 0x1000, self.text, False),
                                (".rdata", 0x2000, bytes(pe_rdata), False)])
        image = bytearray(self.image)
        image[0x2000:0x2000 + len(pe_rdata)] = pe_rdata
        struct.pack_into("<II", image, 0x2040, 0x80012cdc, 0x80014368)
        resolve = {0x34: 0x80012cdc, 0xa1: 0x80014368, 0x35: 0x80012d10}.get
        return pe, image, resolve

    def test_loader_patched_kernel_thunks_resolved_by_the_kernel_are_not_a_mismatch(self):
        pe, image, resolve = self._thunk_fixture()
        res = xd.verify_image(self._read(image), pe, chunk=0x200, resolve=resolve)
        self.assertEqual([(s, v) for _n, s, _e, v in res if v != "ok"], [(0x12000, "thunks")])

    def test_thunk_holding_a_different_kernel_function_is_a_mismatch(self):
        import struct
        pe, image, resolve = self._thunk_fixture()
        # Codex review round 2: the slot for ordinal 0x34 holding the address of
        # ordinal 0x35 - a valid, in-range kernel address, but the wrong import -
        # is exactly what a stale build with a different import looks like.
        wrong = bytearray(image); struct.pack_into("<I", wrong, 0x2040, resolve(0x35))
        self.assertIn("mismatch", self._verdicts(xd.verify_image(self._read(wrong), pe, chunk=0x200, resolve=resolve)))

    def test_thunk_differences_without_a_resolver_are_mismatches(self):
        pe, image, _resolve = self._thunk_fixture()
        self.assertIn("mismatch", self._verdicts(xd.verify_image(self._read(image), pe, chunk=0x200)))

    def test_non_thunk_difference_next_to_a_thunk_is_still_a_mismatch(self):
        import struct
        pe, image, resolve = self._thunk_fixture()
        image2 = bytearray(image); image2[0x2000 + 0x10] ^= 0x01
        self.assertIn("mismatch", self._verdicts(xd.verify_image(self._read(image2), pe, chunk=0x200, resolve=resolve)))
        image3 = bytearray(image); struct.pack_into("<I", image3, 0x2040, 0x00401000)
        self.assertIn("mismatch", self._verdicts(xd.verify_image(self._read(image3), pe, chunk=0x200, resolve=resolve)))

    def test_spot_check_exit_code_never_succeeds_with_unreadable_addresses(self):
        self.assertEqual(xd.spot_check_exit_code([1, 2], [], []), 0)
        self.assertEqual(xd.spot_check_exit_code([1], [2], []), 1)
        self.assertEqual(xd.spot_check_exit_code([1], [], [2]), 2, "one unreadable is not a clean pass")
        self.assertEqual(xd.spot_check_exit_code([1], [2], [3]), 1, "a mismatch outranks unreadable")
        self.assertEqual(xd.spot_check_exit_code([], [], []), 2)


    def test_unreadable_chunk_is_reported_not_counted_as_ok(self):
        res = xd.verify_image(self._read(self.image, fail={0x11400}), self.pe, chunk=0x200)
        self.assertEqual([(s, v) for _n, s, _e, v in res if v != "ok"], [(0x11400, "unreadable")])

    def test_spot_check_reports_match_mismatch_and_unreadable_separately(self):
        stale = bytearray(self.image)
        stale[0x1000 + 0x80] ^= 0xff
        m, mm, u = xd.verify_addrs(self._read(stale, fail={0x11100}), self.pe,
                                   [0x11000, 0x11080, 0x11100, 0x5000])
        self.assertEqual((m, mm, u), ([0x11000], [0x11080], [0x11100, 0x5000]))

    def test_pe_read_va_maps_imagebase_plus_rva_to_file_bytes(self):
        self.assertEqual(xd.pe_read_va(self.pe, 0x11010, 4), self.text[0x10:0x14])
        self.assertIsNone(xd.pe_read_va(self.pe, 0x5000, 4), "outside every section is None, not garbage")


def _fake_kernel(base, ord_base, func_rvas, globals_rva=0x3000):
    """A fake xboxkrnl.exe image mapped at `base`: MZ/PE headers, an export
    directory (ordinal-only, like the real kernel) and a data area."""
    import struct
    img = bytearray(0x4000)
    img[:2] = b"MZ"
    struct.pack_into("<I", img, 0x3c, 0x80)
    img[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", img, 0x80 + 20, 224)
    exp_rva = 0x1000
    struct.pack_into("<II", img, 0x80 + 24 + 96, exp_rva, 40)
    funcs_rva = exp_rva + 40
    struct.pack_into("<IIII", img, exp_rva + 16, ord_base, len(func_rvas), 0, funcs_rva)
    for i, rva in enumerate(func_rvas):
        struct.pack_into("<I", img, funcs_rva + i * 4, rva)
    return img


class Pools(unittest.TestCase):
    """read_pools() attributes a map's heap per engine pool - the number
    memstats cannot give. It must skip freed slots and refuse garbage."""

    def _image(self, pools):
        import struct
        base = 0x400000
        img = bytearray(0x1000 + len(pools) * xd.MEMPOOL_SIZE)
        struct.pack_into("<I", img, 0x10, len(pools))   # poolcount at base+0x10
        struct.pack_into("<I", img, 0x14, base + 0x1000)  # poolchain at base+0x14
        for i, (name, real, payload, fname) in enumerate(pools):
            o = 0x1000 + i * xd.MEMPOOL_SIZE
            struct.pack_into("<8I", img, o, 0, 0, payload, real, 0, fname, 0, 0)
            img[o + 32:o + 32 + len(name)] = name.encode()
        return _mem_reader(bytes(img), base), base + 0x10, base + 0x14

    def test_pools_sorted_largest_first_skipping_freed_slots(self):
        read, pc, ph = self._image([("Zone Engine", 400, 380, 0x1234),
                                    ("freed", 999999, 999999, 0),
                                    ("ref_soft zone", 12000, 11900, 0x1234)])
        self.assertEqual(xd.read_pools(read, pc, ph),
                         [(12000, 11900, "ref_soft zone"), (400, 380, "Zone Engine")])

    def test_garbage_table_raises_instead_of_walking_memory(self):
        import struct
        img = bytearray(0x100); struct.pack_into("<II", img, 0x10, 100000, 0x500000)
        with self.assertRaises(RuntimeError):
            xd.read_pools(_mem_reader(bytes(img), 0x400000), 0x400010, 0x400014)

    def test_load_map_symbols_reads_data_sections_too(self):
        import tempfile, os
        with tempfile.NamedTemporaryFile("w", suffix=".map", delete=False) as f:
            f.write(" 0001:00001000       _R_Init            0000000000012000     x.obj\n")
            f.write(" 0003:00002000       _poolchain         0000000000af6774     engine_common_zone.obj\n")
            path = f.name
        try:
            syms = xd.load_map_symbols(path)
        finally:
            os.unlink(path)
        self.assertEqual(syms["_poolchain"], 0xaf6774)
        self.assertEqual(syms["_R_Init"], 0x12000)


class KernelExportsAndMemstats(unittest.TestCase):
    BASE = 0x80010000

    def test_exports_map_ordinals_to_addresses_and_skip_holes(self):
        img = _fake_kernel(self.BASE, 1, [0x2000, 0, 0x2100])
        exp = xd.pe_exports(_mem_reader(bytes(img), self.BASE), self.BASE)
        self.assertEqual(exp, {1: self.BASE + 0x2000, 3: self.BASE + 0x2100})

    def test_garbage_at_the_kernel_base_raises_instead_of_guessing(self):
        with self.assertRaises(RuntimeError):
            xd.pe_exports(_mem_reader(b"\0" * 0x1000, self.BASE), self.BASE)

    def test_memstats_reads_available_and_per_usage_pages_through_mmglobaldata(self):
        import struct
        img = _fake_kernel(self.BASE, 100, [0, 0, 0x3000])   # ordinal 102 = MmGlobalData at rva 0x3000
        avail_va, usage_va = self.BASE + 0x3100, self.BASE + 0x3200
        struct.pack_into("<8I", img, 0x3000, 0, 0, avail_va, usage_va, 0, 0, 0, 0)
        struct.pack_into("<I", img, 0x3100, 9000)
        usage = [0, 300, 40, 20, 500, 4000, 100, 1500, 0, 900, 24]
        struct.pack_into("<11I", img, 0x3200, *usage)
        read = _mem_reader(bytes(img), self.BASE)
        exp = xd.pe_exports(read, self.BASE)
        available, by = xd.kernel_memstats(read, exp.get)
        self.assertEqual(available, 9000)
        self.assertEqual(by["Image"], 1500)
        self.assertEqual(sum(by.values()) + available, 16384, "fixture is a consistent 64 MiB machine")


if __name__ == "__main__":
    unittest.main()
