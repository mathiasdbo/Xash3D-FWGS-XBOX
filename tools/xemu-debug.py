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
   it, and neither software nor hardware breakpoints survive a
   `system_reset` (the XBE is reloaded over them). Catching a sub-second
   boot path live is not something this tool can promise; reading the
   aftermath is.

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
                                 0-0x4000000). Prints every match address.
  find-delta <hexbytes> <vaddr> Search PHYSICAL RAM for <hexbytes> and print
                                 physical-minus-<vaddr> for each hit - the
                                 load delta of that one page.
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

Addresses are hex, with or without 0x.
"""
import argparse
import re
import socket
import struct
import sys
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
        self.cmd("qSupported:multiprocess+;swbreak+;hwbreak+")

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
        return self.cmd("c", timeout=timeout)

    def step(self, timeout=15):
        return self.cmd("s", timeout=timeout)

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


def scan(read, start, end, chunk, target, label):
    """Generic overlapped scan; `read(addr, n)` returns bytes or raises."""
    found = []
    overlap = len(target) - 1
    prev_tail = b""
    addr = start
    while addr < end:
        n = min(chunk, end - addr)
        try:
            data = read(addr, n)
        except RuntimeError:
            prev_tail = b""
            addr += n
            continue
        hay = prev_tail + data
        idx = hay.find(target)
        while idx != -1:
            hit = addr - len(prev_tail) + idx
            found.append(hit)
            print(f"MATCH at {hit:#010x} ({label})")
            idx = hay.find(target, idx + 1)
        prev_tail = data[-overlap:] if overlap > 0 else b""
        addr += n
    return found


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
        found = scan(g.read_phys, start, end, chunk, target, "physical")
    else:
        start = parse_hex(a.start) if a.start else 0x10000
        end = parse_hex(a.end) if a.end else 0x400000
        chunk = a.chunk or 0x8000
        found = scan(g.read_mem, start, end, chunk, target, "virtual")
    print("matches:", [hex(x) for x in found] if found else "none")
    g.close()


def cmd_find_delta(a):
    g = GdbRemote(a.host, a.port)
    target = bytes.fromhex(a.pattern)
    vaddr = parse_hex(a.vaddr)
    found = scan(g.read_phys, 0x0, 0x4000000, 0x4000, target, "physical")
    if not found:
        print("no physical match - is that the unrelocated prefix of a function "
              "actually in this build (llvm-objdump -d the .obj), before any "
              "relocation-patched bytes?")
    for hit in found:
        print(f"physical {hit:#010x} - virtual {vaddr:#010x} = delta {hit - vaddr:#x} "
              f"(valid for THIS page only; the image is mapped page-by-page)")
    g.close()


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
