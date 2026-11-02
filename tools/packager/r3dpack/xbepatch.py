"""Edit an already linked XBE: its title and its boot logo.

The linker (cxbe) takes the logo as a build option and the repo's link script
does not pass one, so the packager sets it afterwards. The encoding is cxbe's
own (Logo.cpp, Xbe.h LogoRLE): the 100x17 grey image reduced to 4 bits, run
length coded, one byte for runs under 8 and two for longer ones.

Headers are mapped at the XBE's base address up to dwSizeofHeaders, and the
first section starts at the next page, so there is at most the rest of that
first page to grow into; a logo that does not fit there is refused rather
than made to push the sections along.
"""

import re
import struct

LOGO_W, LOGO_H = 100, 17
LOGO_PIXELS = LOGO_W * LOGO_H
PAGE = 0x1000


class XbeError(Exception):
    pass


def _check(d):
    if d[:4] != b"XBEH":
        raise XbeError("not an XBE")
    base = struct.unpack_from("<I", d, 0x104)[0]
    return base


def set_title(data, title):
    d = bytearray(data)
    base = _check(d)
    cert = struct.unpack_from("<I", d, 0x118)[0] - base
    enc = title.encode("utf-16-le")
    if len(enc) > 0x50 - 2:
        raise XbeError("title too long")
    d[cert + 0x0C:cert + 0x0C + 0x50] = enc + b"\0" * (0x50 - len(enc))
    return bytes(d)


def read_pgm(raw):
    """Binary PGM, 100x17, maxval 255 -> the 1700 pixel bytes."""
    m = re.match(rb"P5\s+(\d+)\s+(\d+)\s+(\d+)\s", raw)
    if not m:
        raise XbeError("the boot logo must be a binary PGM (P5)")
    if tuple(int(g) for g in m.groups()) != (LOGO_W, LOGO_H, 255):
        raise XbeError("the boot logo must be 100x17 with maxval 255")
    pix = raw[m.end():]  # exactly one whitespace byte follows maxval
    if len(pix) != LOGO_PIXELS:
        raise XbeError("the boot logo has %d pixel bytes, expected %d" % (len(pix), LOGO_PIXELS))
    return pix


def encode_logo(pixels):
    out = bytearray()
    i = 0
    n = len(pixels)
    while i < n:
        color = pixels[i] >> 4
        run = 1
        while i + run < n and run < 1023 and pixels[i + run] >> 4 == color:
            run += 1
        i += run
        if run < 8:
            out.append(1 | (run << 1) | (color << 4))
        else:
            v = 0 | (1 << 1) | (run << 2) | (color << 12)
            out += struct.pack("<H", v)
    return bytes(out)


def decode_logo(blob):
    """Inverse of encode_logo, to 4-bit values - used to check what was written."""
    out = []
    i = 0
    while i < len(blob):
        b = blob[i]
        if b & 1:
            out += [b >> 4] * ((b >> 1) & 7)
            i += 1
        else:
            v = struct.unpack_from("<H", blob, i)[0]
            if not v & 2:
                raise XbeError("bad logo code")
            out += [v >> 12] * ((v >> 2) & 0x3FF)
            i += 2
    return out


def set_logo(data, pgm):
    d = bytearray(data)
    base = _check(d)
    sizeof_headers = struct.unpack_from("<I", d, 0x108)[0]
    logo_addr, logo_size = struct.unpack_from("<II", d, 0x170)
    blob = encode_logo(read_pgm(pgm))
    if len(blob) <= logo_size:
        off = logo_addr - base  # same place, a smaller or equal image
    else:
        off = (sizeof_headers + 3) & ~3  # after everything else in the headers
        if off + len(blob) > PAGE:
            raise XbeError("the boot logo needs %d bytes; only %d are free in the header page"
                           % (len(blob), PAGE - off))
        new_headers = off + len(blob)
        # the slack must still be empty, or this would overwrite something
        if any(d[sizeof_headers:new_headers]):
            raise XbeError("the header page is not empty after the headers")
        struct.pack_into("<I", d, 0x108, new_headers)
    d[off:off + len(blob)] = blob
    struct.pack_into("<II", d, 0x170, base + off, len(blob))
    return bytes(d)


def get_logo(data):
    base = _check(data)
    addr, size = struct.unpack_from("<II", data, 0x170)
    return decode_logo(data[addr - base:addr - base + size])


# ---- title and save images ----

def make_xpr(dxt1, size):
    """An XPR0 container for one DXT1 texture of size x size, the way the dashboard's
    own TitleImage.xbx / SaveImage.xbx are laid out (checked against both)."""
    if len(dxt1) != size * size // 2:
        raise XbeError("DXT1 data for %dx%d must be %d bytes" % (size, size, size * size // 2))
    lg = size.bit_length() - 1
    if 1 << lg != size:
        raise XbeError("image size must be a power of two")
    fmt = 0x29 | (0x0C << 8) | (1 << 16) | (lg << 20) | (lg << 24)  # 2D, DXT1, one level
    head = bytearray(b"\xad" * 0x800)
    struct.pack_into("<4sIIIIIII", head, 0, b"XPR0", 0x800 + len(dxt1), 0x800,
                     0x00040001, 0, 0, fmt, 0)
    struct.pack_into("<I", head, 0x20, 0xFFFFFFFF)
    return bytes(head) + dxt1


def _sections(d, base):
    nsec = struct.unpack_from("<I", d, 0x11C)[0]
    secs = struct.unpack_from("<I", d, 0x120)[0] - base
    out = []
    for i in range(nsec):
        o = secs + i * 0x38
        fl, va, vs, ra, rs, na = struct.unpack_from("<6I", d, o)
        nm = bytes(d[na - base:d.index(b"\0", na - base)]).decode("latin-1")
        out.append({"name": nm, "flags": fl, "va": va, "vsize": vs, "raw": ra, "rsize": rs})
    return out


def list_sections(data):
    return _sections(data, _check(data))


def add_sections(data, new):
    """Append non-preloaded data sections (name, bytes) to the XBE: $$XTIMAGE etc.

    The section header array moves into the free space of the header page (the
    old one stays where it was, now unused), the data goes after the last
    section on a page boundary, and the image size grows to cover it. The
    kernel never needs to map them; the dashboard reads them from the file.
    """
    import hashlib
    d = bytearray(data)
    base = _check(d)
    secs = _sections(d, base)
    sizeof_headers = struct.unpack_from("<I", d, 0x108)[0]
    nsec = len(secs)
    arr_old = struct.unpack_from("<I", d, 0x120)[0] - base

    pos = (sizeof_headers + 3) & ~3
    arr_new = pos
    pos += (nsec + len(new)) * 0x38
    names_at = pos
    for name, _ in new:
        pos += len(name) + 1
    pos = (pos + 3) & ~3
    counters_at = pos
    pos += 4 * len(new)
    if pos > PAGE:
        raise XbeError("no room left in the header page for the image sections")
    if any(d[sizeof_headers:pos]):
        raise XbeError("the header page is not empty after the headers")

    d[arr_new:arr_new + nsec * 0x38] = d[arr_old:arr_old + nsec * 0x38]
    end_va = max(s["va"] + s["vsize"] for s in secs)
    va = (end_va + PAGE - 1) & ~(PAGE - 1)
    raw_at = (len(d) + PAGE - 1) & ~(PAGE - 1)
    d.extend(b"\0" * (raw_at - len(d)))
    n_at = names_at
    c_at = counters_at
    for i, (name, blob) in enumerate(new):
        d[n_at:n_at + len(name) + 1] = name.encode("latin-1") + b"\0"
        o = arr_new + (nsec + i) * 0x38
        digest = hashlib.sha1(struct.pack("<I", len(blob)) + blob).digest()
        struct.pack_into("<9I", d, o, 0, va, len(blob), raw_at, len(blob), base + n_at, 0,
                         base + c_at, base + c_at + 2)
        d[o + 36:o + 56] = digest
        d.extend(blob)
        d.extend(b"\0" * (-len(d) % PAGE))
        va += (len(blob) + PAGE - 1) & ~(PAGE - 1)
        raw_at = len(d)
        n_at += len(name) + 1
        c_at += 4
    struct.pack_into("<I", d, 0x11C, nsec + len(new))
    struct.pack_into("<I", d, 0x120, base + arr_new)
    struct.pack_into("<I", d, 0x108, pos)
    struct.pack_into("<I", d, 0x10C, va - base)
    return bytes(d)
