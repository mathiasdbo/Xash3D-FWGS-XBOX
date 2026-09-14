#!/usr/bin/env python3
"""
xemu-debug.py - live CPU / memory / GPU debugging of Original Xbox builds
running under xemu, over QEMU's GDB remote serial protocol, plus a
passthrough to the QEMU monitor for everything the protocol itself does
not cover (physical memory, device state, VM control).

Stdlib only (socket, struct, argparse, zlib). No pip.

Why this exists: the Xbox client's boot-time freeze (divergence #34) was
diagnosed by reading the halted CPU's exception record straight off the
faulting thread's stack with this tool - registers, stack, the formatted
error message, the `libs[]` table, the real bytes of `COM_LoadLibrary` -
after a full session of screenshot-driven bisection had pointed at the
wrong line. Read state; do not infer it. See docs/r3d/lessons.md.

## Getting a stub to talk to

1. Launch xemu normally, boot the build, and in xemu's Monitor window
   (`~`, or Debug > Monitor) run `gdbserver` (tcp::1234). This attaches to
   the machine as it is right now - the post-mortem case this tool was
   built for. Recommended.
2. `xemu.exe -s` (xemu forwards unknown flags to QEMU). Do NOT rely on
   `-S`: xemu's GUI resumes the machine on its own shortly after creating
   it. Catching a sub-second boot path live is done by connecting first,
   then `mon system_reset`, then arming: Z0 breakpoints are emulator-
   process state and DO survive both a reconnect and a `system_reset`
   (docs/r3d/lessons.md) - which also means stale ones from earlier
   scripts survive; kill and relaunch xemu before a verification that
   matters.
3. Before trusting ANY breakpoint address taken from a linker map, run
   `verify-image <the linked .exe>`: it compares EVERY file-backed byte
   of the read-only sections (.text, .rdata) against guest memory. xemu
   boots whatever `dvd_path` in its xemu.toml names - not necessarily the
   ISO you just packed - and a stale image makes every address lie
   consistently (breakpoints still "hit", on the wrong functions). A
   sampled check cannot rule that out: a build that differs only where
   it was not sampled passes, which is why there is no sampling mode.

## The address model (read from the guest's own page tables, `vmap`)

- A title's addresses are XBE-VIRTUAL, base 0x10000: exactly the linker
  map's `Rva+Base` column. `mem`/`search`/`sym` take these directly, no
  correction. `info mem` shows the image: 0x10000 header (rw), .text (r-),
  .rdata (r-), .data/.bss (rw) up to ~0x103d000 for this project's build.
- The kernel's 0x80000000+X alias is physical X but covers ONLY the kernel
  (0x80010000-0x8003d000 here). EIP inside a bugcheck sits there. A
  "cannot access memory" at a 0x80xxxxxx address past that is the alias
  ending, not evidence about the title.
- The image is mapped page-by-page into physical RAM (0-64 MB). Most pages
  of this build sit at physical = virtual + 0xB0000, but NOT all (`info
  tlb`: 0x1c000 -> 0xac000, 0x10000 -> 0xe0000). Never assume a constant
  delta: resolve through `vmap`, read virtually, or read physically (`xp`,
  `search --phys`). `find-delta` measures the delta for one address by
  locating a function's own bytes - report it as "for this page", never
  "for the image".
- Stacks live at 0xd00xxxxx (kernel-managed thread stacks, virtual).
  Read them with `mem`/`stack`.

## Commands

  regs                          CPU registers (one line).
  mem <vaddr> <len>             Virtual hex dump (GDB `m`).
  xp <paddr> <len>              PHYSICAL hex dump (monitor `xp`). Works for
                                 RAM (0-0x3ffffff) and MMIO (NV2A at
                                 0xfd000000-0xfdffffff).
  stack [len]                   Dwords from ESP up, labeled. Default 64.
                                 An x86 exception record on the stack reads
                                 ExceptionCode (0xC0000005 = access
                                 violation), Flags, Record, ExceptionAddress
                                 (0 = call through a NULL pointer), NumParams.
  search <hexbytes> [--phys] [--start A] [--end B] [--chunk N]
                                Byte-pattern scan. Virtual (default,
                                 0x10000-0x400000) or physical (--phys,
                                 0-0x4000000). Prints every match address
                                 AND how much was actually inspected. Exit
                                 0 = complete scan; 2 = some chunks were
                                 unreadable (listed; "none" then does NOT
                                 mean absent); 1 = nothing at all could be
                                 read, no verdict. Failure paths are unit
                                 tested: tools/test_xemu_debug.py.
  find-delta <hexbytes> <vaddr> Search PHYSICAL RAM for <hexbytes> and print
                                 physical-minus-<vaddr> for each hit - the
                                 load delta of that one page. Same coverage
                                 reporting and exit codes as search.
  sym <addr> --map <file> [--delta D]
                                Nearest preceding public .text symbol from an
                                 lld-link map, plus the next symbol (a large
                                 gap means a static/local function, or an
                                 address outside the module - kernel
                                 addresses resolve to nonsense here; check
                                 `vmap` first). --delta subtracts D before
                                 lookup (e.g. a physical address).
  vmap                          `info mem` + `info tlb` (what is mapped where).
  mon <command...>              Raw QEMU monitor command via qRcmd:
                                 `info registers`, `info pci`, `info qtree`,
                                 `system_reset`, `x/8xw 0x...`, ...
  gpu                           NV2A register snapshot over physical MMIO:
                                 PMC (BOOT_0 chip id, INTR_0, ENABLE),
                                 PFIFO (INTR_0, CACHE1 push/DMA put/get),
                                 PGRAPH (INTR, CTX_CONTROL, FIFO),
                                 PCRTC (START = framebuffer physical addr,
                                 INTR_0, RASTER), PRAMDAC (NVPLL, VPLL,
                                 GENERAL_CONTROL), PTIMER (TIME_0/1),
                                 PVIDEO (BUFFER, STOP), PFB (CFG0/1, CSTATUS).
  fbdump <out.png|out.ppm> [--width W --height H --bpp 32|16] [--addr P]
                                Read the framebuffer the GPU is scanning out
                                 (physical PCRTC_START unless --addr) and
                                 write it as PNG or PPM. Dimensions are NOT
                                 discoverable from PCRTC alone: pass the
                                 mode the title set with XVideoSetMode()
                                 (defaults 640x480x32, the common kernel/
                                 dashboard default). Wrong dims = a
                                 sheared image, not an error.
  screenshot <out.png>          Alias for fbdump.
  break <addr> [--steps N]      Software breakpoint, continue, single-step N
                                 (see the launch-timing caveat above).
  verify-image <exe> [--chunk N] Compare EVERY file-backed byte of the linked
                                 PE's read-only sections (.text, .rdata - not
                                 .data, which changes at runtime) with guest
                                 memory, N bytes per read (default 0x8000).
                                 Exit 0 = identity, every byte matches; 1 =
                                 the guest is not running this file (check
                                 xemu's dvd_path); 2 = some bytes unreadable,
                                 no identity verdict.
                                 libxboxkrnl's __imp_ thunk table in .rdata
                                 legitimately differs (ordinal in the file,
                                 loader-patched kernel address in the guest):
                                 each such dword must equal what the running
                                 kernel's own export table gives for that
                                 ordinal, or it is a mismatch.
  verify-image <exe> --addr A [B ...]
                                Spot check 16 bytes at explicit addresses -
                                 shows one breakpoint address disagreeing,
                                 but a pass here is NOT image identity. Exit
                                 1 = any mismatch, 2 = any unreadable.
  memstats                      The kernel's own page counters, read through
                                 the exported MmGlobalData (ordinal 102): the
                                 numbers MmQueryStatistics (ordinal 181) reports
                                 - available pages and busy pages per usage
                                 (stack, pool, image, contiguous, ...) - with a
                                 self-check that they sum to the 64 MiB.
  pools --map <file> [--min-bytes N]
                                The engine's own memory pools (zone.c's
                                 poolchain, located via the map's _poolcount/
                                 _poolchain), largest first - which subsystem a
                                 map's heap actually went to (ref_soft zone, the
                                 BSP, edicts, SoundLib, ...). memstats says how
                                 much; this says what.

Addresses are hex, with or without 0x.
"""
import argparse
import re
import socket
import struct
import sys
import time
import zlib

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 1234

REG_NAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
             "eip", "eflags", "cs", "ss", "ds", "es", "fs", "gs"]

NV2A_MMIO = 0xFD000000
# (block, offset, name) - offsets from NV2A_MMIO. Names follow xemu's own
# hw/xbox/nv2a register naming.
NV2A_REGS = [
    ("PMC",     0x000000, "BOOT_0 (chip id)"),
    ("PMC",     0x000100, "INTR_0"),
    ("PMC",     0x000140, "INTR_EN_0"),
    ("PMC",     0x000200, "ENABLE"),
    ("PFIFO",   0x002100, "INTR_0"),
    ("PFIFO",   0x002140, "INTR_EN_0"),
    ("PFIFO",   0x003200, "CACHE1_PUSH0"),
    ("PFIFO",   0x003240, "CACHE1_DMA_PUT"),
    ("PFIFO",   0x003244, "CACHE1_DMA_GET"),
    ("PFIFO",   0x003220, "CACHE1_DMA_PUSH"),
    ("PGRAPH",  0x400100, "INTR"),
    ("PGRAPH",  0x400140, "INTR_EN"),
    ("PGRAPH",  0x400144, "CTX_CONTROL"),
    ("PGRAPH",  0x400720, "FIFO"),
    ("PCRTC",   0x600100, "INTR_0"),
    ("PCRTC",   0x600140, "INTR_EN_0"),
    ("PCRTC",   0x600800, "START (framebuffer phys)"),
    ("PCRTC",   0x600808, "CONFIG"),
    ("PCRTC",   0x600810, "RASTER"),
    ("PRAMDAC", 0x680500, "NVPLL"),
    ("PRAMDAC", 0x680508, "VPLL"),
    ("PRAMDAC", 0x680600, "GENERAL_CONTROL"),
    ("PTIMER",  0x009400, "TIME_0"),
    ("PTIMER",  0x009410, "TIME_1"),
    ("PVIDEO",  0x008700, "BUFFER"),
    ("PVIDEO",  0x008704, "STOP"),
    ("PFB",     0x100200, "CFG0"),
    ("PFB",     0x100204, "CFG1"),
    ("PFB",     0x10020c, "CSTATUS"),
]

_HEXBYTE_RE = re.compile(r"0x([0-9a-fA-F]{2})")
_HEXWORD_RE = re.compile(r"0x([0-9a-fA-F]{8})")


class GdbRemote:
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self._handshake()

    def _handshake(self, deadline=2.0):
        # A prior connection that sent 'c'/'s' and then disconnected before
        # a stop-reply arrived (script killed, timed out, or crashed) leaves
        # that reply queued in the stub, which delivers it as the FIRST
        # packet on the NEXT connection, ahead of whatever that connection
        # actually asked for - shifting every subsequent reply by one packet
        # (the real qSupported reply then shows up later as an "<unexpected
        # packet>" in an unrelated read). A fixed window of silence is not
        # proof the connection starts clean - the stale reply can legitimately
        # arrive after that window closes - so instead of guessing a delay,
        # send qSupported and keep discarding (acking each one, since an
        # unacked packet can make the stub hold back whatever it queues next
        # - see _recv's own comment on this) anything that isn't actually the
        # qSupported reply, until one arrives or `deadline` is exhausted. A
        # stop-reply/console-output/error packet always starts with one of
        # S/T/W/X/O/E, or is exactly "OK" - none of those is how a real
        # qSupported reply starts ("PacketSize=...;qXfer:...", confirmed
        # against xemu's own gdbstub), so shape alone reliably tells a stale
        # packet apart from the one actually being waited for here.
        self._send("qSupported:multiprocess+;swbreak+;hwbreak+")
        start = time.monotonic()
        discarded = 0
        while True:
            remaining = deadline - (time.monotonic() - start)
            try:
                pkt = self._recv(timeout=max(remaining, 0.001))
            except socket.timeout:
                pkt = None
            if pkt is None:
                raise RuntimeError(
                    f"no qSupported reply within {deadline}s (discarded {discarded} "
                    "stale packet(s) first) - the connection may still be desynced")
            if pkt == "OK" or pkt[:1] in ("S", "T", "W", "X", "O", "E"):
                discarded += 1
                continue
            if discarded:
                sys.stderr.write(
                    f"xemu-debug: discarded {discarded} stale packet(s) left over from a "
                    "previous connection (likely an unacknowledged continue/step reply)\n")
            return pkt

    @staticmethod
    def _checksum(data):
        return sum(data.encode()) & 0xff

    def _send(self, data):
        self.sock.sendall(("$%s#%02x" % (data, self._checksum(data))).encode())

    def _recv(self, timeout=10):
        self.sock.settimeout(timeout)
        while True:
            c = self.sock.recv(1)
            if not c:
                return None
            if c in (b"+", b"-"):
                continue
            if c == b"$":
                buf = b""
                while True:
                    ch = self.sock.recv(1)
                    if ch == b"#":
                        self.sock.recv(2)  # checksum, unverified
                        # Ack, or the stub holds any further queued packet -
                        # qRcmd's multi-packet "O<hex>" console stream would
                        # otherwise stop after its first line.
                        self.sock.sendall(b"+")
                        return buf.decode(errors="replace")
                    buf += ch

    def cmd(self, data, timeout=10):
        self._send(data)
        return self._recv(timeout)

    def read_regs(self):
        r = self.cmd("g")
        if r is None or r.startswith("E"):
            raise RuntimeError(f"failed to read registers: {r!r}")
        vals, pos = {}, 0
        for n in REG_NAMES:
            chunk = r[pos:pos + 8]
            pos += 8
            if len(chunk) < 8:
                break
            vals[n] = struct.unpack("<I", bytes.fromhex(chunk))[0]
        return vals

    def read_mem(self, addr, length):
        r = self.cmd(f"m {addr:x},{length:x}", timeout=15)
        if r is None or r.startswith("E"):
            raise RuntimeError(
                f"virtual read of {length} bytes at {addr:#010x} refused ({r!r}): "
                "not mapped in the guest's page tables right now - see the "
                "address model in --help, try `vmap`, or read physically with `xp`")
        return bytes.fromhex(r)

    def monitor(self, command, timeout=10):
        # Reply is a stream of "O<hex>" console-output packets ended by a
        # bare "OK" or "E<code>"; those two are exact/prefix matches, not hex.
        r = self.cmd(f"qRcmd,{command.encode().hex()}", timeout=timeout)
        out = ""
        while r is not None and r != "OK" and not r.startswith("E"):
            if r.startswith("O") and len(r) > 1:
                try:
                    out += bytes.fromhex(r[1:]).decode(errors="replace")
                except ValueError:
                    out += f"<undecodable packet: {r!r}>\n"
            else:
                out += f"<unexpected packet: {r!r}>\n"
            r = self._recv(timeout)
        if r is not None and r.startswith("E"):
            out += f"<monitor error: {r}>\n"
        return out

    def read_phys(self, addr, length):
        """Physical read via the monitor's `xp` (byte granularity)."""
        out = self.monitor(f"xp /{length}xb {addr:#x}", timeout=30)
        data = bytes(int(m, 16) for m in _HEXBYTE_RE.findall(out))
        if len(data) != length:
            raise RuntimeError(
                f"physical read at {addr:#010x}: expected {length} bytes, got "
                f"{len(data)} - monitor said: {out.strip()[:200]!r}")
        return data

    def read_phys_word(self, addr):
        out = self.monitor(f"xp /1xw {addr:#x}", timeout=10)
        m = _HEXWORD_RE.search(out)
        if not m:
            raise RuntimeError(f"physical word read at {addr:#010x} failed: {out.strip()[:200]!r}")
        return int(m.group(1), 16)

    def set_breakpoint(self, addr):
        r = self.cmd(f"Z0,{addr:x},1")
        if r != "OK":
            raise RuntimeError(f"failed to set breakpoint at {addr:#x}: {r!r}")

    def remove_breakpoint(self, addr):
        return self.cmd(f"z0,{addr:x},1")

    def cont(self, timeout=60):
        try:
            return self.cmd("c", timeout=timeout)
        except socket.timeout:
            raise RuntimeError(
                f"no stop-reply within {timeout}s - the target is still running "
                "(the breakpoint has not been hit yet, or never will be on this "
                "path). The 'c' was already sent and is NOT undone by this "
                "timeout: the guest keeps running and the eventual stop-reply "
                "stays queued in the stub until the next connection drains it. "
                "Reconnect and try again with a longer --timeout, or use "
                "`mon \"info registers\"` / `mon \"cont\"` to inspect/drive the "
                "target via the QEMU monitor instead.") from None

    def step(self, timeout=15):
        try:
            return self.cmd("s", timeout=timeout)
        except socket.timeout:
            raise RuntimeError(f"no stop-reply within {timeout}s for single-step") from None

    def close(self):
        self.sock.close()


def parse_hex(s):
    s = s.strip().lower()
    if s.startswith("0x"):
        s = s[2:]
    return int(s, 16)


def fmt_regs(vals):
    order = ["eip", "esp", "ebp", "eax", "ebx", "ecx", "edx", "esi", "edi",
             "eflags", "cs", "ss", "ds", "es", "fs", "gs"]
    return " ".join(f"{n.upper()}={vals[n]:#010x}" for n in order if n in vals)


def hexdump(addr, data):
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{addr + i:#010x}:  {hexpart:<47s}  {asc}")
    return "\n".join(lines)


def load_map(path):
    """lld-link -map: public .text symbols as sorted (rvabase, name, obj)."""
    entries = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 4 or ":" not in parts[0]:
                continue
            if parts[0].split(":")[0] != "0001":  # .text only
                continue
            try:
                rvabase = int(parts[2], 16)
            except ValueError:
                continue
            entries.append((rvabase, parts[1], parts[3]))
    entries.sort(key=lambda e: e[0])
    return entries


def resolve_symbol(entries, target):
    prev = nxt = None
    for e in entries:
        if e[0] <= target:
            prev = e
        else:
            nxt = e
            break
    return prev, nxt


class ScanResult:
    """What a scan actually measured, not just what it found.

    A scan over memory that is partly (or entirely) unreadable must never
    look like a clean "no matches": this project has shipped tools that
    reported success without measuring anything before (docs/r3d/lessons.md),
    and "pattern absent" vs "zero bytes inspected" is exactly that mistake.
    """

    def __init__(self):
        self.found = []
        self.bytes_scanned = 0
        self.ok_chunks = 0
        self.failed_ranges = []   # (start, end, reason)

    @property
    def failed_chunks(self):
        return len(self.failed_ranges)

    @property
    def complete(self):
        return not self.failed_ranges

    @property
    def nothing_read(self):
        return self.ok_chunks == 0


def scan(read, start, end, chunk, target, label, log=print):
    """Overlapped byte-pattern scan; `read(addr, n)` returns bytes or raises
    RuntimeError. Every failed read is recorded, never dropped."""
    if not target:
        raise ValueError("empty search pattern")
    if chunk <= 0:
        raise ValueError("chunk size must be positive")
    res = ScanResult()
    overlap = len(target) - 1
    prev_tail = b""
    addr = start
    while addr < end:
        n = min(chunk, end - addr)
        try:
            data = read(addr, n)
        except RuntimeError as e:
            res.failed_ranges.append((addr, addr + n, str(e)))
            prev_tail = b""
            addr += n
            continue
        res.ok_chunks += 1
        res.bytes_scanned += len(data)
        hay = prev_tail + data
        idx = hay.find(target)
        while idx != -1:
            hit = addr - len(prev_tail) + idx
            res.found.append(hit)
            log(f"MATCH at {hit:#010x} ({label})")
            idx = hay.find(target, idx + 1)
        # Carry the last overlap bytes of the ACCUMULATED haystack, not of
        # this read alone: with chunks smaller than the pattern, a match
        # spanning three or more reads would otherwise never be seen while
        # the scan still reported itself complete (Codex review, round 2).
        prev_tail = hay[-overlap:] if overlap > 0 else b""
        addr += n
    return res


def report_scan(res, start, end, label, log=print):
    """Print the scan's coverage honestly. Returns the process exit code:
    0 only for a complete scan, 2 for a partial one, 1 if nothing was read."""
    if res.nothing_read:
        log(f"ERROR: scanned 0 bytes of {label} memory {start:#x}-{end:#x} - "
            f"every one of {res.failed_chunks} chunk read(s) failed. "
            f"'no matches' would be meaningless; nothing was inspected.")
        if res.failed_ranges:
            _, _, reason = res.failed_ranges[0]
            log(f"  first failure: {reason}")
        return 1
    log(f"matches: {[hex(x) for x in res.found] if res.found else 'none'}")
    log(f"scanned {res.bytes_scanned:#x} bytes in {res.ok_chunks} chunk(s)")
    if not res.complete:
        log(f"WARNING: INCOMPLETE scan - {res.failed_chunks} chunk(s) could not "
            f"be read, so 'none' above does not mean absent. Unread ranges:")
        for s, e, reason in res.failed_ranges[:12]:
            log(f"  {s:#010x}-{e:#010x}: {reason.splitlines()[0][:100]}")
        if res.failed_chunks > 12:
            log(f"  ... and {res.failed_chunks - 12} more")
        return 2
    return 0


def write_png(path, width, height, rgb_rows):
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff)
    raw = b"".join(b"\x00" + row for row in rgb_rows)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


# ---- commands -------------------------------------------------------------

def cmd_regs(a):
    g = GdbRemote(a.host, a.port)
    vals = g.read_regs()
    print(fmt_regs(vals))
    if vals.get("eax") == 0x1e and vals.get("ecx") == 0xc0000005:
        print("note: EAX=0x1E ECX=0xC0000005 is the KMODE_EXCEPTION_NOT_HANDLED / "
              "STATUS_ACCESS_VIOLATION bugcheck pair - run `stack 1024` and look "
              "for the exception record (0xC0000005 followed by ExceptionAddress).")
    g.close()


def cmd_mem(a):
    g = GdbRemote(a.host, a.port)
    addr = parse_hex(a.addr)
    print(hexdump(addr, g.read_mem(addr, a.length)))
    g.close()


def cmd_xp(a):
    g = GdbRemote(a.host, a.port)
    addr = parse_hex(a.addr)
    print(hexdump(addr, g.read_phys(addr, a.length)))
    g.close()


def cmd_stack(a):
    g = GdbRemote(a.host, a.port)
    esp = g.read_regs()["esp"]
    data = g.read_mem(esp, a.length)
    for i in range(0, len(data) - 3, 4):
        dw = struct.unpack("<I", data[i:i + 4])[0]
        note = ""
        if dw == 0xc0000005:
            note = "  <- STATUS_ACCESS_VIOLATION (exception record starts here?)"
        elif dw == 0x0000001e:
            note = "  <- 0x1E bugcheck code?"
        print(f"{esp + i:#010x}: {dw:#010x}{note}")
    g.close()


def cmd_search(a):
    g = GdbRemote(a.host, a.port)
    target = bytes.fromhex(a.pattern)
    if a.phys:
        start = parse_hex(a.start) if a.start else 0x0
        end = parse_hex(a.end) if a.end else 0x4000000
        chunk = a.chunk or 0x4000
        label = "physical"
        res = scan(g.read_phys, start, end, chunk, target, label)
    else:
        start = parse_hex(a.start) if a.start else 0x10000
        end = parse_hex(a.end) if a.end else 0x400000
        chunk = a.chunk or 0x8000
        label = "virtual"
        res = scan(g.read_mem, start, end, chunk, target, label)
    g.close()
    sys.exit(report_scan(res, start, end, label))


def cmd_find_delta(a):
    g = GdbRemote(a.host, a.port)
    target = bytes.fromhex(a.pattern)
    vaddr = parse_hex(a.vaddr)
    start, end = 0x0, 0x4000000
    res = scan(g.read_phys, start, end, 0x4000, target, "physical")
    g.close()
    code = report_scan(res, start, end, "physical")
    if code == 1:
        sys.exit(1)
    if not res.found:
        print("no physical match - is that the unrelocated prefix of a function "
              "actually in this build (llvm-objdump -d the .obj), before any "
              "relocation-patched bytes?" + (" Note the scan above was "
              "INCOMPLETE." if not res.complete else ""))
    for hit in res.found:
        print(f"physical {hit:#010x} - virtual {vaddr:#010x} = delta {hit - vaddr:#x} "
              f"(valid for THIS page only; the image is mapped page-by-page)")
    sys.exit(code)


def cmd_sym(a):
    entries = load_map(a.map)
    addr = parse_hex(a.addr)
    if a.delta:
        addr -= parse_hex(a.delta)
    prev, nxt = resolve_symbol(entries, addr)
    if addr >= 0x80000000:
        print(f"warning: {addr:#010x} is in the kernel's 0x80000000 alias; a title "
              f".map cannot symbolise it (see `vmap`). Result below is coincidence.")
    if prev:
        print(f"{addr:#010x} -> {prev[1]} + {addr - prev[0]:#x}  ({prev[2]})")
    else:
        print(f"{addr:#010x}: below every symbol in {a.map}")
    if nxt:
        gap = nxt[0] - (prev[0] if prev else 0)
        print(f"  next: {nxt[1]} at {nxt[0]:#x} ({nxt[2]}, span {gap:#x} bytes)")


def cmd_vmap(a):
    g = GdbRemote(a.host, a.port)
    print("=== info mem (virtual ranges) ===")
    print(g.monitor("info mem", timeout=20))
    print("=== info tlb (virtual -> physical, per page) ===")
    out = g.monitor("info tlb", timeout=30)
    print(out if not a.limit else "\n".join(out.splitlines()[:a.limit]))
    g.close()


def cmd_mon(a):
    g = GdbRemote(a.host, a.port)
    print(g.monitor(" ".join(a.command), timeout=30))
    g.close()


def cmd_gpu(a):
    g = GdbRemote(a.host, a.port)
    last = None
    for block, off, name in NV2A_REGS:
        if block != last:
            print(f"[{block}]")
            last = block
        try:
            v = g.read_phys_word(NV2A_MMIO + off)
            print(f"  {NV2A_MMIO + off:#010x} {name:<26s} = {v:#010x}")
        except RuntimeError as e:
            print(f"  {NV2A_MMIO + off:#010x} {name:<26s} = <unreadable: {e}>")
    g.close()


def cmd_fbdump(a):
    g = GdbRemote(a.host, a.port)
    fb = parse_hex(a.addr) if a.addr else g.read_phys_word(NV2A_MMIO + 0x600800)
    w, h, bpp = a.width, a.height, a.bpp
    bytespp = bpp // 8
    nbytes = w * h * bytespp
    print(f"framebuffer physical {fb:#010x}, {w}x{h}x{bpp} = {nbytes} bytes")
    data = bytearray()
    step = 0x8000
    for off in range(0, nbytes, step):
        data += g.read_phys(fb + off, min(step, nbytes - off))
    g.close()
    rows = []
    for y in range(h):
        row = bytearray()
        base = y * w * bytespp
        for x in range(w):
            i = base + x * bytespp
            if bpp == 32:   # X8R8G8B8 little-endian: B G R X
                row += bytes((data[i + 2], data[i + 1], data[i]))
            else:           # R5G6B5
                px = data[i] | (data[i + 1] << 8)
                row += bytes((((px >> 11) & 0x1f) << 3, ((px >> 5) & 0x3f) << 2, (px & 0x1f) << 3))
        rows.append(bytes(row))
    if a.out.lower().endswith(".ppm"):
        with open(a.out, "wb") as f:
            f.write(f"P6\n{w} {h}\n255\n".encode() + b"".join(rows))
    else:
        write_png(a.out, w, h, rows)
    print(f"wrote {a.out}")


def cmd_break(a):
    g = GdbRemote(a.host, a.port)
    addr = parse_hex(a.addr)
    print("BEFORE:", fmt_regs(g.read_regs()))
    g.set_breakpoint(addr)
    print(f"breakpoint at {addr:#010x}, continuing...")
    print("stop reply:", g.cont(timeout=a.timeout))
    vals = g.read_regs()
    print("AT STOP:", fmt_regs(vals))
    if vals.get("eip") != addr:
        print("!!! EIP != breakpoint - the machine had already passed this point "
              "(see the launch-timing caveat in --help).")
        g.close()
        return
    g.remove_breakpoint(addr)
    for i in range(a.steps):
        r = g.step()
        print(f"step {i + 1:3d} [{r}]: {fmt_regs(g.read_regs())}")
    g.close()


_IMAGE_SCN_MEM_WRITE = 0x80000000


def pe_sections(data):
    """(name, va, file_bytes, writable) per section of a 32-bit PE image
    file, with `va` = ImageBase + RVA (the 0x10000-based addresses the
    linker map and the guest use) and `file_bytes` the section's raw
    file-backed contents (never more than its virtual size)."""
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    imgbase = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    out = []
    for i in range(nsec):
        o = pe + 24 + optsz + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode(errors="replace")
        vsize, rva, rsize, rptr = struct.unpack_from("<IIII", data, o + 8)
        chars = struct.unpack_from("<I", data, o + 36)[0]
        raw = data[rptr:rptr + min(vsize, rsize)]
        out.append((name, imgbase + rva, raw, bool(chars & _IMAGE_SCN_MEM_WRITE)))
    return out


def pe_read_va(data, va, length):
    """Bytes of the PE file at virtual address `va`, or None when `va` is
    not file-backed (outside every section, or in a section's zero-filled
    tail)."""
    for _name, sva, raw, _w in pe_sections(data):
        if sva <= va < sva + len(raw):
            return raw[va - sva:va - sva + length]
    return None


KERNEL_BASE = 0x80010000


def pe_exports(read, base):
    """Ordinal -> virtual address of every export of the PE image mapped at
    `base` in the guest (used for xboxkrnl.exe at KERNEL_BASE, whose
    exports are by ordinal only). Raises RuntimeError if the headers do not
    parse - the caller must then say "no verdict", never guess."""
    hdr = read(base, 0x400)
    if hdr[:2] != b"MZ":
        raise RuntimeError(f"no MZ header at {base:#x}")
    pe = struct.unpack_from("<I", hdr, 0x3c)[0]
    if hdr[pe:pe + 4] != b"PE\0\0":
        raise RuntimeError(f"no PE signature at {base + pe:#x}")
    exp_rva, exp_size = struct.unpack_from("<II", hdr, pe + 24 + 96)
    if not exp_rva:
        raise RuntimeError("image has no export directory")
    d = read(base + exp_rva, 40)
    ord_base, nfuncs, _nnames, funcs_rva = struct.unpack_from("<IIII", d, 16)
    if not (0 < nfuncs < 4096):
        raise RuntimeError(f"implausible export count {nfuncs}")
    funcs = struct.unpack(f"<{nfuncs}I", read(base + funcs_rva, nfuncs * 4))
    return {ord_base + i: base + rva for i, rva in enumerate(funcs) if rva}


def verify_image(read, data, chunk=0x8000, resolve=None):
    """Compare EVERY file-backed byte of the PE's read-only sections (.text,
    .rdata, ... - anything without IMAGE_SCN_MEM_WRITE, since writable data
    legitimately changes at runtime) against guest memory, `chunk` bytes per
    read. Returns a list of (section, start_va, end_va, verdict) per chunk,
    verdict in {"ok", "mismatch", "unreadable"}. Only "every chunk ok" is
    identity; a single mismatch means the guest is not running this file
    (stale ISO, stale xemu dvd_path, a different build), and an unreadable
    chunk is "not measured", never "matches" - the whole point of this
    check is that a stale image makes breakpoints hit consistently on the
    wrong functions, so partial coverage would just move the blind spot."""
    results = []
    for name, sva, raw, writable in pe_sections(data):
        if writable or not raw:
            continue
        for off in range(0, len(raw), chunk):
            expect = raw[off:off + chunk]
            start = sva + off
            try:
                got = read(start, len(expect))
            except Exception:
                verdict = "unreadable"
            else:
                if got == expect:
                    verdict = "ok"
                elif resolve is not None and _only_kernel_thunks_differ(expect, got, start, resolve):
                    verdict = "thunks"
                else:
                    verdict = "mismatch"
            results.append((name, start, start + len(expect), verdict))
    return results


def _only_kernel_thunks_differ(expect, got, start, resolve):
    """True when every differing byte sits in a dword-aligned kernel import
    thunk that the Xbox loader legitimately patched: the file holds the
    ordinal (`0x800000NN`, libxboxkrnl's `__imp_*` table in .rdata) and the
    guest holds EXACTLY the address the running kernel exports for that
    ordinal (`resolve(ordinal)`, read from xboxkrnl's own export table) -
    not merely some kernel address, which a stale build importing a
    different function through the same slot would also satisfy. Any other
    difference is a real mismatch."""
    if len(expect) != len(got):
        return False
    i = 0
    while i < len(expect):
        if expect[i] == got[i]:
            i += 1
            continue
        d = (start + i) & ~3
        o = d - start
        if o < 0 or o + 4 > len(expect):
            return False
        f = struct.unpack_from("<I", expect, o)[0]
        g = struct.unpack_from("<I", got, o)[0]
        if f & 0xFFFFFE00 != 0x80000000 or resolve(f & 0x1FF) != g:
            return False
        i = o + 4
    return True


def spot_check_exit_code(matched, mismatched, unreadable):
    """1 = something disagreed; 2 = something could not be compared (no
    clean verdict, even if everything else matched); 0 = every requested
    address matched."""
    if mismatched:
        return 1
    if unreadable or not matched:
        return 2
    return 0


MM_USAGE_NAMES = ("Unknown", "Stack", "VirtualPageTable", "SystemPageTable", "Pool",
                  "VirtualMemory", "SystemMemory", "Image", "FsCache", "Contiguous",
                  "Debugger")
MM_GLOBAL_DATA_ORDINAL = 102


def kernel_memstats(read, resolve):
    """The kernel's own page accounting - the same counters
    MmQueryStatistics (xboxkrnl ordinal 181) reports, read directly through
    the exported MmGlobalData (ordinal 102): *AvailablePages, and
    AllocatedPagesByUsage[MmMaximumUsage] (4 KiB pages, per MMPFN_BUSY_TYPE).
    Returns (available_pages, {usage: pages})."""
    gd = resolve(MM_GLOBAL_DATA_ORDINAL)
    if gd is None:
        raise RuntimeError("kernel does not export MmGlobalData (ordinal 102)")
    fields = struct.unpack("<8I", read(gd, 32))
    avail_ptr, usage_ptr = fields[2], fields[3]
    available = struct.unpack("<I", read(avail_ptr, 4))[0]
    usage = struct.unpack(f"<{len(MM_USAGE_NAMES)}I", read(usage_ptr, 4 * len(MM_USAGE_NAMES)))
    return available, dict(zip(MM_USAGE_NAMES, usage))


def load_map_symbols(path):
    """Every symbol in an lld-link map, any section: name -> VA. (load_map()
    keeps only .text, which is what `sym` wants; data symbols live in 0003.)"""
    out = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 4 or ":" not in parts[0]:
                continue
            try:
                out[parts[1]] = int(parts[2], 16)
            except ValueError:
                continue
    return out


MEMPOOL_SIZE = 96   # engine/common/zone.c mempool_t, 32-bit: 8 x 4-byte fields + name[64]


def read_pools(read, poolcount_addr, poolchain_addr):
    """The engine's own memory pools (zone.c's poolchain array): a list of
    (real_bytes, payload_bytes, name), largest first, skipping freed slots
    (filename == NULL). Attribution the kernel's page counters cannot give:
    which subsystem a map's heap actually went to."""
    count = struct.unpack("<I", read(poolcount_addr, 4))[0]
    base = struct.unpack("<I", read(poolchain_addr, 4))[0]
    if not base or count > 4096:
        raise RuntimeError(f"implausible pool table: base={base:#x} count={count}")
    rows = []
    for i in range(count):
        d = read(base + i * MEMPOOL_SIZE, MEMPOOL_SIZE)
        _chain, _small, total, real, _last, fname, _line, _flags = struct.unpack_from("<8I", d, 0)
        if not fname:
            continue
        name = d[32:96].split(b"\0")[0].decode(errors="replace")
        rows.append((real, total, name))
    rows.sort(reverse=True)
    return rows


def cmd_pools(a):
    syms = load_map_symbols(a.map)
    try:
        pc, ph = syms["_poolcount"], syms["_poolchain"]
    except KeyError:
        raise RuntimeError(f"{a.map} has no _poolcount/_poolchain - is it this build's map?")
    g = GdbRemote(a.host, a.port)
    rows = read_pools(g.read_mem, pc, ph)
    g.close()
    total = sum(r for r, _, _ in rows)
    print(f"{len(rows)} pools, {total / 1048576:.2f} MiB real (malloc'd), largest first:")
    for real, payload, name in rows:
        if real >= a.min_bytes:
            print(f"  {real / 1048576:7.2f} MiB real  {payload / 1048576:7.2f} MiB payload  {name}")


def cmd_memstats(a):
    g = GdbRemote(a.host, a.port)
    exports = pe_exports(g.read_mem, KERNEL_BASE)
    available, usage = kernel_memstats(g.read_mem, exports.get)
    g.close()
    busy = sum(usage.values())
    print(f"available pages: {available:6d}  ({available * 4 / 1024:8.2f} MiB)")
    print(f"busy pages:      {busy:6d}  ({busy * 4 / 1024:8.2f} MiB), by usage:")
    for name, pages in usage.items():
        if pages:
            print(f"  {name:<17}{pages:6d}  ({pages * 4 / 1024:8.2f} MiB)")
    total = available + busy
    print(f"available + busy = {total} pages = {total * 4 / 1024:.2f} MiB"
          + ("  (= the 64 MiB console, counters consistent)" if total == 16384 else
             "  (NOT 16384: counters inconsistent or not a 64 MiB machine - do not trust)"))


def verify_addrs(read, data, addrs, length=16):
    """Spot check: (matched, mismatched, unreadable) address lists for 16
    bytes at each explicit address. A passing spot check is NOT identity -
    use verify_image() for that; this exists to point at one specific
    breakpoint address and see it disagree."""
    matched, mismatched, unreadable = [], [], []
    for a in addrs:
        expect = pe_read_va(data, a, length)
        if expect is None:
            unreadable.append(a)
            continue
        try:
            got = read(a, length)
        except Exception:
            unreadable.append(a)
            continue
        (matched if got == expect else mismatched).append(a)
    return matched, mismatched, unreadable


def cmd_verify_image(a):
    with open(a.exe, "rb") as f:
        data = f.read()
    g = GdbRemote(a.host, a.port)
    if a.addr:
        addrs = [parse_hex(x) for x in a.addr]
        matched, mismatched, unreadable = verify_addrs(g.read_mem, data, addrs)
        g.close()
        for x in matched:
            print(f"  ok         {x:#010x}")
        for x in mismatched:
            print(f"  MISMATCH   {x:#010x}")
        for x in unreadable:
            print(f"  unreadable {x:#010x}")
        print(f"verify-image: spot check of {len(addrs)} address(es): {len(matched)} match, "
              f"{len(mismatched)} mismatch, {len(unreadable)} unreadable. A spot check "
              "is not image identity - run without --addr for that.")
        sys.exit(spot_check_exit_code(matched, mismatched, unreadable))
    try:
        exports = pe_exports(g.read_mem, KERNEL_BASE)
    except (RuntimeError, struct.error) as e:
        g.close()
        print(f"verify-image: cannot read the running kernel's export table ({e}) - "
              "kernel import thunks cannot be resolved, no identity verdict.")
        sys.exit(2)
    results = verify_image(g.read_mem, data, chunk=a.chunk, resolve=exports.get)
    g.close()
    total = sum(e - s for _n, s, e, _v in results)
    by = {"ok": 0, "thunks": 0, "mismatch": 0, "unreadable": 0}
    for name, s, e, v in results:
        by[v] += e - s
        if v == "thunks":
            print(f"  ok         {name:<8} {s:#010x}-{e:#010x} (differs only in loader-patched "
                  "kernel import thunks: ordinal in file, kernel address in guest)")
        elif v != "ok":
            print(f"  {v:<10} {name:<8} {s:#010x}-{e:#010x}")
    print(f"verify-image: {by['ok'] + by['thunks']} of {total} read-only image bytes match "
          f"{a.exe}; {by['mismatch']} mismatch, {by['unreadable']} unreadable")
    if by["mismatch"]:
        print("verify-image: the guest is NOT running this file. Check what xemu "
              "actually boots (dvd_path in xemu.toml) against the ISO you just "
              "packed - breakpoints from this map are meaningless until it is.")
        sys.exit(1)
    if by["unreadable"] or not total:
        print("verify-image: not every byte could be compared - no identity verdict.")
        sys.exit(2)
    print("verify-image: identity confirmed - every file-backed read-only byte matches.")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub = p.add_subparsers(dest="subcommand", required=True)

    s = sub.add_parser("regs"); s.set_defaults(func=cmd_regs)

    s = sub.add_parser("mem"); s.add_argument("addr"); s.add_argument("length", type=lambda x: int(x, 0)); s.set_defaults(func=cmd_mem)
    s = sub.add_parser("xp"); s.add_argument("addr"); s.add_argument("length", type=lambda x: int(x, 0)); s.set_defaults(func=cmd_xp)
    s = sub.add_parser("stack"); s.add_argument("length", type=lambda x: int(x, 0), nargs="?", default=64); s.set_defaults(func=cmd_stack)

    s = sub.add_parser("search"); s.add_argument("pattern"); s.add_argument("--phys", action="store_true")
    s.add_argument("--start"); s.add_argument("--end"); s.add_argument("--chunk", type=lambda x: int(x, 0)); s.set_defaults(func=cmd_search)

    s = sub.add_parser("find-delta"); s.add_argument("pattern"); s.add_argument("vaddr"); s.set_defaults(func=cmd_find_delta)

    s = sub.add_parser("sym"); s.add_argument("addr"); s.add_argument("--map", required=True); s.add_argument("--delta"); s.set_defaults(func=cmd_sym)

    s = sub.add_parser("vmap"); s.add_argument("--limit", type=int, default=0); s.set_defaults(func=cmd_vmap)
    s = sub.add_parser("mon"); s.add_argument("command", nargs="+"); s.set_defaults(func=cmd_mon)
    s = sub.add_parser("gpu"); s.set_defaults(func=cmd_gpu)

    for name in ("fbdump", "screenshot"):
        s = sub.add_parser(name); s.add_argument("out")
        s.add_argument("--width", type=int, default=640); s.add_argument("--height", type=int, default=480)
        s.add_argument("--bpp", type=int, choices=(16, 32), default=32); s.add_argument("--addr")
        s.set_defaults(func=cmd_fbdump)

    s = sub.add_parser("break"); s.add_argument("addr"); s.add_argument("--steps", type=int, default=20)
    s.add_argument("--timeout", type=int, default=60); s.set_defaults(func=cmd_break)

    s = sub.add_parser("verify-image"); s.add_argument("exe")
    s.add_argument("--addr", nargs="*"); s.add_argument("--chunk", type=lambda x: int(x, 0), default=0x8000)
    s.set_defaults(func=cmd_verify_image)

    s = sub.add_parser("memstats"); s.set_defaults(func=cmd_memstats)
    s = sub.add_parser("pools"); s.add_argument("--map", required=True)
    s.add_argument("--min-bytes", type=lambda x: int(x, 0), default=16384); s.set_defaults(func=cmd_pools)

    a = p.parse_args()
    try:
        a.func(a)
    except (ConnectionRefusedError, socket.timeout, TimeoutError) as e:
        print(f"error: no GDB stub at {a.host}:{a.port} ({e}). Run `gdbserver` in "
              f"xemu's Monitor, or launch xemu with -s.", file=sys.stderr)
        sys.exit(1)
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
