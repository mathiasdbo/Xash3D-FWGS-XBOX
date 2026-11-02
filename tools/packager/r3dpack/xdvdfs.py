"""Write an Xbox XISO (XDVDFS) image straight from a file tree.

No nxdk, no extract-xiso: the format is small enough to write directly, and
the packager has to run on a machine that has neither.

Layout written (the same shape extract-xiso -c makes):

    sector 0..31   zeros (the 64 KiB game-partition header area, unused here)
    sector 32      volume descriptor: magic, root directory sector and size,
                   FILETIME, magic again at +0x7EC
    sector 33..    directory tables, then file data, each on a sector boundary

A directory table is a binary search tree of entries packed into 2048-byte
sectors (an entry never straddles a sector). Each entry is

    u16 left   u16 right        offsets in 4-byte units from the table start
    u32 sector u32 size         first sector and byte length
    u8  attributes  u8 name_len  name  (padding to 4 bytes with 0xFF)

Names compare case-insensitively, which is how the console looks them up.
"""

import os
import struct
import time

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
VOLUME_SECTOR = 32
FIRST_SECTOR = 33
ATTR_DIR = 0x10
ATTR_FILE = 0x20
MAX_NAME = 255
MAX_TABLE = 0xFFFF * 4  # a u16 offset counts 4-byte units
EPOCH_DELTA = 11644473600  # seconds from 1601-01-01 to 1970-01-01


class XisoError(Exception):
    pass


class _Node:
    __slots__ = ("name", "is_dir", "source", "size", "children", "sector", "table")

    def __init__(self, name, is_dir, source=None, size=0):
        self.name = name
        self.is_dir = is_dir
        self.source = source  # a path, or bytes for an in-memory file
        self.size = size
        self.children = {}
        self.sector = 0
        self.table = b""


def _key(name):
    return name.upper()


def _sectors(n):
    return (n + SECTOR - 1) // SECTOR


def build_tree(files):
    """files: iterable of (relative path with '/', source, size). Returns the root."""
    root = _Node("", True)
    for rel, source, size in files:
        parts = [p for p in rel.split("/") if p]
        if not parts:
            raise XisoError("empty path in the file list")
        node = root
        for part in parts[:-1]:
            if len(part.encode("latin-1", "replace")) > MAX_NAME:
                raise XisoError("name too long: %s" % part)
            child = node.children.get(_key(part))
            if child is None:
                child = _Node(part, True)
                node.children[_key(part)] = child
            elif not child.is_dir:
                raise XisoError("%s is both a file and a folder" % part)
            node = child
        leaf = parts[-1]
        if len(leaf.encode("latin-1", "replace")) > MAX_NAME:
            raise XisoError("name too long: %s" % leaf)
        if _key(leaf) in node.children:
            raise XisoError("duplicate entry (names compare without case): %s" % rel)
        if size >= 1 << 32:
            raise XisoError("%s is 4 GiB or larger; an XISO file size is 32-bit" % rel)
        node.children[_key(leaf)] = _Node(leaf, False, source, size)
    return root


def _balanced(sorted_nodes):
    """Indices forming a balanced BST over a sorted list: root, left, right."""
    def build(lo, hi):
        if lo >= hi:
            return None
        mid = (lo + hi) // 2
        return (mid, build(lo, mid), build(mid + 1, hi))
    return build(0, len(sorted_nodes))


def _make_table(node):
    """Pack node's children into a directory table. Needs each child's sector set."""
    kids = [node.children[k] for k in sorted(node.children)]
    if not kids:
        return b""
    sizes = []
    for kid in kids:
        raw = 14 + len(kid.name.encode("latin-1", "replace"))
        sizes.append(((raw + 3) // 4) * 4)
    return _make_table_rooted(node, kids, sizes)


def _make_table_rooted(node, kids, sizes):
    """Same table, but the tree's root entry is placed first (the reader starts at 0)."""
    tree = _balanced(kids)
    order = []

    def walk(t):  # pre-order: root first, so offset 0 is the tree root
        if t is None:
            return
        order.append(t[0])
        walk(t[1])
        walk(t[2])

    walk(tree)
    offsets = {}
    pos = 0
    for idx in order:
        size = sizes[idx]
        if (pos % SECTOR) + size > SECTOR:
            pos = _sectors(pos) * SECTOR
        offsets[idx] = pos
        pos += size
    total = _sectors(pos) * SECTOR
    if total > MAX_TABLE:
        raise XisoError("a folder holds too many entries for one XDVDFS table")
    table = bytearray(b"\xff" * total)

    def put(t):
        if t is None:
            return 0
        idx, left, right = t
        lo, ro = put(left), put(right)
        kid = kids[idx]
        name = kid.name.encode("latin-1", "replace")
        start = kid.sector
        size = kid.size if not kid.is_dir else len(kid.table)
        if kid.is_dir and not kid.children:
            start, size = 0, 0
        attr = ATTR_DIR if kid.is_dir else ATTR_FILE
        off = offsets[idx]
        struct.pack_into("<HHIIBB", table, off, lo // 4, ro // 4, start, size, attr, len(name))
        table[off + 14:off + 14 + len(name)] = name
        return off

    put(tree)
    return bytes(table)


def _table_size(node):
    """Bytes a directory's table will take, independent of sector numbers."""
    kids = [node.children[k] for k in sorted(node.children)]
    if not kids:
        return 0
    pos = 0
    for kid in kids:
        raw = 14 + len(kid.name.encode("latin-1", "replace"))
        size = ((raw + 3) // 4) * 4
        if (pos % SECTOR) + size > SECTOR:
            pos = _sectors(pos) * SECTOR
        pos += size
    return _sectors(pos) * SECTOR


def _layout(root):
    """Assign sectors: every directory table first, then file data, depth first."""
    dirs = []
    files = []

    def walk(node):
        dirs.append(node)
        for k in sorted(node.children):
            kid = node.children[k]
            if kid.is_dir:
                walk(kid)
            else:
                files.append(kid)

    walk(root)
    sector = FIRST_SECTOR
    for d in dirs:
        n = _table_size(d)
        d.sector = sector if n else 0
        sector += n // SECTOR
    for f in files:
        if f.size == 0:
            f.sector = 0
            continue
        f.sector = sector
        sector += _sectors(f.size)
    # tables can only be encoded now that every sector is known; children first
    for d in reversed(dirs):
        d.table = b"\x00" * _table_size(d)  # right length for the parent's size field
    for d in reversed(dirs):
        d.table = _make_table(d)
    return dirs, files, sector


def write_xiso(files, out_path, progress=None, chunk=1 << 20):
    """files: list of (relative path, source, size); source is a path or bytes.

    progress(done_bytes, total_bytes, name) is called as file data is written.
    """
    root = build_tree(files)
    dirs, flist, total_sectors = _layout(root)
    total_bytes = sum(f.size for f in flist)
    root_size = len(root.table)
    if root_size == 0:
        raise XisoError("the image has no files")

    filetime = (int(time.time()) + EPOCH_DELTA) * 10_000_000
    vol = bytearray(SECTOR)
    vol[0:20] = MAGIC
    struct.pack_into("<IIQ", vol, 20, root.sector, root_size, filetime)
    vol[0x7EC:0x7EC + 20] = MAGIC

    tmp = out_path + ".part"
    done = 0
    try:
        with open(tmp, "wb") as out:
            out.write(b"\x00" * (VOLUME_SECTOR * SECTOR))
            out.write(vol)
            for d in dirs:
                if d.table:
                    out.seek(d.sector * SECTOR)
                    out.write(d.table)
            for f in flist:
                if f.size == 0:
                    continue
                out.seek(f.sector * SECTOR)
                if isinstance(f.source, (bytes, bytearray)):
                    out.write(f.source)
                    written = len(f.source)
                else:
                    written = 0
                    with open(f.source, "rb") as src:
                        while True:
                            buf = src.read(chunk)
                            if not buf:
                                break
                            out.write(buf)
                            written += len(buf)
                            if progress:
                                progress(done + written, total_bytes, f.name)
                if written != f.size:
                    raise XisoError("%s changed while it was being packed (%d bytes, expected %d)"
                                    % (f.name, written, f.size))
                done += written
                if progress:
                    progress(done, total_bytes, f.name)
            end = total_sectors * SECTOR
            out.seek(end)
            out.truncate(end)
        os.replace(tmp, out_path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise
    return total_sectors * SECTOR


# ---- reading: used by the tests and by the packager's own post-check ----

def read_xiso(path):
    """Walk an image and return {relative path: (sector, size)} - also validates it."""
    out = {}
    with open(path, "rb") as f:
        f.seek(VOLUME_SECTOR * SECTOR)
        head = f.read(SECTOR)
        if head[0:20] != MAGIC or head[0x7EC:0x7EC + 20] != MAGIC:
            raise XisoError("not an XDVDFS image (volume descriptor magic missing)")
        root_sector, root_size = struct.unpack_from("<II", head, 20)

        def table(sector, size):
            f.seek(sector * SECTOR)
            return f.read(size)

        def walk(tbl, prefix):
            seen = set()

            def entry(off):
                if off in seen:
                    raise XisoError("directory table loops")
                seen.add(off)
                left, right, sector, size, attr, nlen = struct.unpack_from("<HHIIBB", tbl, off)
                name = tbl[off + 14:off + 14 + nlen].decode("latin-1")
                if left:
                    entry(left * 4)
                rel = prefix + name
                if attr & ATTR_DIR:
                    out[rel + "/"] = (sector, size)
                    if size:
                        walk(table(sector, size), rel + "/")
                else:
                    out[rel] = (sector, size)
                if right:
                    entry(right * 4)

            if tbl:
                entry(0)

        walk(table(root_sector, root_size), "")
    return out
