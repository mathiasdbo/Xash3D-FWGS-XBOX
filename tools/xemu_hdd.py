"""Read-only access to xemu's hard disk image (qcow2 holding the Xbox's FATX
partitions) - list a directory or copy a file off the emulated E: drive.

    python tools/xemu_hdd.py [--qcow2 PATH] ls  /UDATA/FFFF0002/valve
    python tools/xemu_hdd.py [--qcow2 PATH] get /UDATA/FFFF0002/valve/x.bin out.bin

Run it with xemu stopped: xemu writes the image while it runs, and a read
then can see half-written metadata. Nothing here ever writes the image.

Limits (each one raises rather than returning wrong bytes): no backing
file, no compressed clusters, no encryption; partition E: at the retail
offset only (0xABE80000, 0x1312D6000 bytes). Written for X9-X step 12
(decal/screenshot checks) and moved here for the frame-time recorder
(tools/frametime-report.py fetch-xemu).
"""
import argparse
import os
import struct
import sys

DEFAULT_QCOW2 = r"G:\Games\Xemu\xbox_hdd.qcow2"
PART_E = (0xABE80000, 0x1312D6000)  # retail partition 1 (E:), offset and size


class HddError(Exception):
    pass


class Qcow2:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(104)
        magic, ver, bfo, bfs, cbits, size, crypt, l1size, l1off = struct.unpack(">IIQIIQIIQ", h[:48])
        if magic != 0x514649FB:
            raise HddError("%s is not a qcow2 image" % path)
        if bfo != 0:
            raise HddError("qcow2 backing files are not supported")
        if crypt != 0:
            raise HddError("encrypted qcow2 images are not supported")
        self.cbits = cbits
        self.csize = 1 << cbits
        self.size = size
        self.l2bits = cbits - 3
        self.f.seek(l1off)
        self.l1 = struct.unpack(">%dQ" % l1size, self.f.read(8 * l1size))
        self.l2cache = {}

    def close(self):
        self.f.close()

    def _cluster(self, off):
        l1i = off >> (self.cbits + self.l2bits)
        l2i = (off >> self.cbits) & ((1 << self.l2bits) - 1)
        if l1i >= len(self.l1):
            return None
        l2off = self.l1[l1i] & 0x00FFFFFFFFFFFE00
        if not l2off:
            return None
        if l2off not in self.l2cache:
            self.f.seek(l2off)
            self.l2cache[l2off] = struct.unpack(">%dQ" % (1 << self.l2bits), self.f.read(self.csize))
        e = self.l2cache[l2off][l2i]
        if (e >> 62) & 1:
            raise HddError("compressed qcow2 cluster at offset 0x%x" % off)
        d = e & 0x00FFFFFFFFFFFE00
        if (e & 1) and not d:
            return "zero"
        return d or None

    def read(self, off, n):
        out = bytearray()
        while n > 0:
            c = self._cluster(off)
            inoff = off & (self.csize - 1)
            take = min(n, self.csize - inoff)
            if c is None or c == "zero":
                out += bytes(take)
            else:
                self.f.seek(c + inoff)
                chunk = self.f.read(take)
                if len(chunk) != take:
                    raise HddError("short read in the qcow2 image at 0x%x" % (c + inoff))
                out += chunk
            off += take
            n -= take
        return bytes(out)


class Fatx:
    def __init__(self, q, base, size):
        self.q = q
        hdr = q.read(base, 4096)
        if hdr[:4] != b"FATX":
            raise HddError("no FATX signature at 0x%x" % base)
        spc = struct.unpack("<I", hdr[8:12])[0]
        self.clus = spc * 512
        nclus = size // self.clus
        self.fat32 = nclus >= 0xFFF0
        fatsz = (nclus + 1) * (4 if self.fat32 else 2)
        fatsz = (fatsz + 4095) & ~4095
        self.fatoff = base + 0x1000
        self.data = self.fatoff + fatsz
        self.fat = q.read(self.fatoff, fatsz)

    def nxt(self, c):
        if self.fat32:
            v = struct.unpack_from("<I", self.fat, c * 4)[0]
            return None if v >= 0xFFFFFFF0 else v
        v = struct.unpack_from("<H", self.fat, c * 2)[0]
        return None if v >= 0xFFF0 else v

    def chain(self, c):
        seen = set()
        while c is not None and c != 0:
            if c in seen:
                raise HddError("FATX cluster chain loops at %d" % c)
            seen.add(c)
            yield c
            c = self.nxt(c)

    def clusdata(self, c):
        return self.q.read(self.data + (c - 1) * self.clus, self.clus)

    def entries(self, c):
        """(name, attr, first cluster, size) of every live entry of a directory."""
        for cl in self.chain(c):
            d = self.clusdata(cl)
            for i in range(0, len(d), 64):
                nl = d[i]
                if nl in (0x00, 0xFF):
                    return
                if nl == 0xE5:
                    continue
                attr = d[i + 1]
                name = d[i + 2:i + 2 + nl].decode("latin-1")
                first, size = struct.unpack_from("<II", d, i + 44)
                yield name, attr, first, size

    def lookup(self, path):
        c, attr, size = 1, 0x10, 0
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            for name, a, first, sz in self.entries(c):
                if name.lower() == part.lower():
                    c, attr, size = first, a, sz
                    break
            else:
                raise HddError("not found: %s (in %s)" % (part, path))
        return c, attr, size

    def listdir(self, path):
        c, attr, _ = self.lookup(path)
        if not attr & 0x10:
            raise HddError("%s is not a directory" % path)
        return [(n, bool(a & 0x10), sz) for n, a, first, sz in self.entries(c)]

    def readfile(self, path):
        c, attr, size = self.lookup(path)
        if attr & 0x10:
            raise HddError("%s is a directory" % path)
        out = bytearray()
        for cl in self.chain(c):
            out += self.clusdata(cl)
            if len(out) >= size:
                break
        if len(out) < size:
            raise HddError("%s: cluster chain ends at %d of %d bytes" % (path, len(out), size))
        return bytes(out[:size])


def open_e(qcow2=DEFAULT_QCOW2):
    q = Qcow2(qcow2)
    return Fatx(q, *PART_E)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--qcow2", default=DEFAULT_QCOW2)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("ls")
    p.add_argument("path")
    p = sub.add_parser("get")
    p.add_argument("path")
    p.add_argument("out")
    a = ap.parse_args(argv)
    try:
        fx = open_e(a.qcow2)
        if a.cmd == "ls":
            for name, is_dir, size in fx.listdir(a.path):
                print("%s %10d %s" % ("d" if is_dir else "-", size, name))
        else:
            data = fx.readfile(a.path)
            with open(a.out, "wb") as f:
                f.write(data)
            print("wrote %d bytes to %s" % (len(data), a.out))
    except (HddError, OSError) as e:
        print("xemu_hdd: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
