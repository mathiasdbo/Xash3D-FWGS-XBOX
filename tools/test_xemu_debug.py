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


if __name__ == "__main__":
    unittest.main()
