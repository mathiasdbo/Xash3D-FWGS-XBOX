#!/usr/bin/env python3
"""Generate this fork's own Xbox menu art into assets/gfx/shell/.

    python3 tools/xbox-menu-art.py [--output <dir>] [--check] [--scale N]

What this exists for
--------------------
Every other menu asset on an Xbox disc has a source that can be re-run:
the background tiles come from `tools/asset-studio --bake-menu-art`, the
font and `colors.lst` are tracked files staged by `tools/xbox-xiso.sh`.
The controller diagram the Controller screen needs (fork-plan.md XM item
5) had no such source - a hand-exported bitmap would be the one menu
asset nobody could regenerate or review as a diff. This script is that
source: the diagram is defined here as geometry, and the .tga is output,
not input.

It also re-renders `legend_button.tga` on request. That file is already
tracked and already shipped on a verified disc, so it is NOT rewritten by
default; `--check` validates its format rather than demanding byte
equality with a fresh render, because a byte mismatch there would only
mean "made by a different tool", not "wrong".

Row order, verified rather than assumed
---------------------------------------
`engine/common/imagelib/img_tga.c:137-148`: when bit 5 of the descriptor
byte is set the loader fills the target buffer forward, so the file's
first row is the image's TOP row; when it is clear the loader starts at
the last row and steps backwards, so the file's first row is the image's
BOTTOM row. Retail's own art (checked directly: `resource/logo*.tga`,
`resource/background/*.tga`) and `UiPacker::writeTga` both use 0x08,
bottom-up. New art here matches retail, so anything this script writes is
interchangeable with a retail file on the same load path.

Dependencies: none. Standard library only, deliberately - this runs on
whatever host packs a disc, and a bake that needs a pip install is a bake
that stops being run.
"""

import argparse
import math
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# TGA
# ---------------------------------------------------------------------------

def encode_tga(pixels, width, height, has_alpha):
    """Serialise an uncompressed 32-bit truecolor TGA, bottom-up (descriptor 0x08).

    `pixels` is a flat bytearray of RGBA in normal top-down scan order; the
    rows are emitted reversed here so the file matches retail's convention.

    Split out from `write_tga` so `--check` can compare against the bytes this
    would produce without putting a file anywhere: Codex review found the first
    version wrote a `duke.tga.check` beside the asset and deleted it, which
    silently destroys a pre-existing file of that name, races another check
    running at the same time, and cannot run at all against a read-only
    directory - none of which a mode documented as writing nothing should do.
    """
    header = struct.pack(
        "<BBBHHBHHHHBB",
        0,              # id length
        0,              # no colormap
        2,              # uncompressed truecolor
        0, 0, 0,        # colormap spec
        0, 0,           # x/y origin
        width, height,
        32,             # bits per pixel
        0x08 if has_alpha else 0x00,
    )
    out = bytearray(header)
    for y in range(height - 1, -1, -1):
        row = pixels[y * width * 4:(y + 1) * width * 4]
        for x in range(width):
            o = x * 4
            out += bytes((row[o + 2], row[o + 1], row[o], row[o + 3]))
    return bytes(out)


def write_tga(path, pixels, width, height, has_alpha):
    """Write what `encode_tga` produces."""
    data = encode_tga(pixels, width, height, has_alpha)
    with open(path, "wb") as f:
        f.write(data)
    return len(data)


def read_tga(path):
    """Read an uncompressed 32- or 24-bit truecolor TGA. Returns (rgba, w, h).

    Handles both row orders. RLE (types 9-11) is refused rather than
    half-supported: no file this fork writes or reads uses it, and a
    silently wrong decode is worse than a refusal.
    """
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 18:
        raise ValueError("%s: shorter than a TGA header" % path)
    id_len = data[0]
    image_type = data[2]
    width, height = struct.unpack("<HH", data[12:16])
    bpp = data[16]
    descriptor = data[17]
    if image_type != 2:
        raise ValueError("%s: image type %d not supported (only uncompressed truecolor)" % (path, image_type))
    if bpp not in (24, 32):
        raise ValueError("%s: %d bits per pixel not supported" % (path, bpp))
    stride = bpp // 8
    off = 18 + id_len
    need = width * height * stride
    if len(data) - off < need:
        raise ValueError("%s: truncated, %d pixel bytes missing" % (path, need - (len(data) - off)))
    top_down = bool(descriptor & 0x20)
    rgba = bytearray(width * height * 4)
    for row in range(height):
        y = row if top_down else height - 1 - row
        src = off + row * width * stride
        dst = y * width * 4
        for x in range(width):
            s = src + x * stride
            d = dst + x * 4
            rgba[d] = data[s + 2]
            rgba[d + 1] = data[s + 1]
            rgba[d + 2] = data[s]
            rgba[d + 3] = data[s + 3] if stride == 4 else 255
    return rgba, width, height


# ---------------------------------------------------------------------------
# Signed-distance rasteriser
# ---------------------------------------------------------------------------
#
# Every shape below is an exact signed distance in design units, so the
# antialiasing is the distance itself (coverage = 0.5 - d clamped) rather
# than supersampling. That keeps this fast enough to be pure Python and
# makes the diagram resolution-independent: --scale renders the same
# geometry larger without touching a single coordinate.


def sd_circle(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) - r


def sd_round_box(px, py, cx, cy, hw, hh, r):
    dx = abs(px - cx) - (hw - r)
    dy = abs(py - cy) - (hh - r)
    ax, ay = max(dx, 0.0), max(dy, 0.0)
    return math.hypot(ax, ay) + min(max(dx, dy), 0.0) - r


def sd_capsule(px, py, ax, ay, bx, by, r):
    pax, pay = px - ax, py - ay
    bax, bay = bx - ax, by - ay
    denom = bax * bax + bay * bay
    h = 0.0 if denom == 0 else max(0.0, min(1.0, (pax * bax + pay * bay) / denom))
    return math.hypot(pax - bax * h, pay - bay * h) - r


def sd_ellipse(px, py, cx, cy, rx, ry):
    # Cheap approximation, good enough for the small pill-shaped buttons and
    # never used for anything whose silhouette carries meaning.
    dx, dy = (px - cx) / rx, (py - cy) / ry
    d = math.hypot(dx, dy)
    return (d - 1.0) * min(rx, ry)


def smin(a, b, k):
    if k <= 0:
        return min(a, b)
    h = max(0.0, min(1.0, 0.5 + 0.5 * (b - a) / k))
    return b * (1 - h) + a * h - k * h * (1 - h)


class Canvas:
    """A fixed-size RGBA canvas painted by signed-distance shapes."""

    def __init__(self, width, height, scale, background):
        self.w = width
        self.h = height
        self.scale = scale
        self.px = bytearray(width * height * 4)
        br, bg, bb, ba = background
        for i in range(0, len(self.px), 4):
            self.px[i] = br
            self.px[i + 1] = bg
            self.px[i + 2] = bb
            self.px[i + 3] = ba

    def paint(self, sdf, color, bbox):
        """Composite `color` wherever `sdf` is inside, over the given design-space bbox."""
        s = self.scale
        x0 = max(0, int(bbox[0] * s) - 2)
        y0 = max(0, int(bbox[1] * s) - 2)
        x1 = min(self.w, int(bbox[2] * s) + 3)
        y1 = min(self.h, int(bbox[3] * s) + 3)
        cr, cg, cb = color
        ca = color[3] if len(color) > 3 else 255
        for y in range(y0, y1):
            dy = (y + 0.5) / s
            base = y * self.w * 4
            for x in range(x0, x1):
                dx = (x + 0.5) / s
                # Distance is in design units; convert to pixels so the
                # antialiasing ramp is always one pixel wide whatever --scale is.
                cov = 0.5 - sdf(dx, dy) * s
                if cov <= 0.0:
                    continue
                if cov > 1.0:
                    cov = 1.0
                a = cov * (ca / 255.0)
                o = base + x * 4
                inv = 1.0 - a
                self.px[o] = int(cr * a + self.px[o] * inv + 0.5)
                self.px[o + 1] = int(cg * a + self.px[o + 1] * inv + 0.5)
                self.px[o + 2] = int(cb * a + self.px[o + 2] * inv + 0.5)
                na = a + (self.px[o + 3] / 255.0) * inv
                self.px[o + 3] = int(na * 255 + 0.5)


# ---------------------------------------------------------------------------
# The Duke
# ---------------------------------------------------------------------------
#
# Design space is 2000 x 1110, the same space the design canvas's own
# Controller artboard uses, so a coordinate can be compared against the
# mock directly. Output size is that space scaled - 320x178 at 1x, which is
# what the Controller screen draws at 640x480.
#
# The four face buttons are left as plain coloured discs with no letters
# baked in. Two reasons, both deliberate: at 1x a face button is 13 px
# across, where a baked glyph is mush; and mainui already draws pad letters
# the right way - CMenuLegend composites the real font over a tintable
# circle (Legend.cpp:157), which stays translatable and stays sharp. The
# Controller screen labels each button in text beside the diagram anyway.

DESIGN_W, DESIGN_H = 2000.0, 1110.0

# Face colours are Legend.cpp's own literals (s_legendFaceColor), not
# picked again here - the diagram and the legend row must not drift apart.
FACE_A = (0x6D, 0xBE, 0x45)
FACE_B = (0xD9, 0x32, 0x2D)
FACE_X = (0x2F, 0x79, 0xC8)
FACE_Y = (0xF0, 0xC2, 0x1B)

BODY = (0x2B, 0x2B, 0x2B)
BODY_EDGE = (0x0E, 0x0E, 0x0E)
PLATE = (0x34, 0x34, 0x34)
STRIP = (0x24, 0x24, 0x24)
WELL = (0x1C, 0x1C, 0x1C)
STICK = (0x33, 0x33, 0x33)
STICK_TOP = (0x2A, 0x2A, 0x2A)
DPAD = (0x2E, 0x2E, 0x2E)
JEWEL = (0x0B, 0x0B, 0x0B)
JEWEL_RING = (0x3D, 0x6B, 0x1C)
JEWEL_X = (0x9B, 0xE3, 0x2E)
WHITE_BTN = (0xE9, 0xE9, 0xE9)
BLACK_BTN = (0x17, 0x17, 0x17)
SMALL_BTN = (0x22, 0x22, 0x22)


def body_sdf(grow=0.0):
    """The controller silhouette: two shoulders, a centre plate and two grips."""
    def f(x, y):
        d = sd_round_box(x, y, 1000, 380, 440 + grow, 300 + grow, 160)
        d = smin(d, sd_circle(x, y, 640, 330, 265 + grow), 90)
        d = smin(d, sd_circle(x, y, 1360, 330, 265 + grow), 90)
        d = smin(d, sd_capsule(x, y, 600, 480, 515, 960, 165 + grow), 110)
        d = smin(d, sd_capsule(x, y, 1400, 480, 1485, 960, 165 + grow), 110)
        return d
    return f


def plate_sdf():
    def f(x, y):
        d = sd_round_box(x, y, 1000, 330, 400, 240, 150)
        d = smin(d, sd_circle(x, y, 645, 305, 228), 80)
        d = smin(d, sd_circle(x, y, 1355, 305, 228), 80)
        return d
    return f


def dpad_sdf(cx, cy, arm, thick, r):
    def f(x, y):
        return min(sd_round_box(x, y, cx, cy, arm, thick, r),
                   sd_round_box(x, y, cx, cy, thick, arm, r))
    return f


def draw_duke(canvas):
    c = canvas

    # Silhouette: a dark edge first, the body fill inside it.
    c.paint(body_sdf(14), BODY_EDGE, (330, 20, 1670, 1110))
    c.paint(body_sdf(), BODY, (350, 40, 1650, 1100))
    c.paint(plate_sdf(), PLATE, (380, 40, 1620, 620))

    # The raised centre strip under the jewel.
    c.paint(lambda x, y: sd_round_box(x, y, 1000, 300, 145, 250, 60), STRIP, (830, 30, 1170, 570))

    # Shoulder triggers, seen edge-on at the top of the shell.
    for tx in (565, 1435):
        c.paint(lambda x, y, tx=tx: sd_round_box(x, y, tx, 62, 68, 26, 22), BODY_EDGE, (tx - 80, 20, tx + 80, 100))
        c.paint(lambda x, y, tx=tx: sd_round_box(x, y, tx, 60, 62, 20, 18), STRIP, (tx - 80, 20, tx + 80, 100))

    # Left analogue stick.
    c.paint(lambda x, y: sd_circle(x, y, 650, 295, 128), BODY_EDGE, (500, 145, 800, 445))
    c.paint(lambda x, y: sd_circle(x, y, 650, 295, 120), WELL, (510, 155, 790, 435))
    c.paint(lambda x, y: sd_circle(x, y, 650, 295, 84), STICK, (550, 195, 750, 395))
    c.paint(lambda x, y: sd_circle(x, y, 650, 295, 62), STICK_TOP, (570, 215, 730, 375))

    # D-pad.
    c.paint(lambda x, y: sd_circle(x, y, 760, 520, 124), BODY_EDGE, (620, 380, 900, 660))
    c.paint(lambda x, y: sd_circle(x, y, 760, 520, 116), WELL, (630, 390, 890, 650))
    c.paint(dpad_sdf(760, 520, 96, 33, 10), DPAD, (650, 410, 870, 630))

    # Right analogue stick.
    c.paint(lambda x, y: sd_circle(x, y, 1230, 520, 128), BODY_EDGE, (1080, 370, 1380, 670))
    c.paint(lambda x, y: sd_circle(x, y, 1230, 520, 120), WELL, (1090, 380, 1370, 660))
    c.paint(lambda x, y: sd_circle(x, y, 1230, 520, 84), STICK, (1130, 420, 1330, 620))
    c.paint(lambda x, y: sd_circle(x, y, 1230, 520, 62), STICK_TOP, (1150, 440, 1310, 600))

    # The jewel: black lens, green ring, the X mark as two crossing strokes.
    c.paint(lambda x, y: sd_circle(x, y, 1000, 255, 146), JEWEL_RING, (840, 95, 1160, 415))
    c.paint(lambda x, y: sd_circle(x, y, 1000, 255, 134), JEWEL, (850, 105, 1150, 405))
    c.paint(lambda x, y: sd_capsule(x, y, 930, 190, 1070, 320, 17), JEWEL_X, (900, 160, 1100, 350))
    c.paint(lambda x, y: sd_capsule(x, y, 1070, 190, 930, 320, 17), JEWEL_X, (900, 160, 1100, 350))

    # White and Black, the two pill-shaped buttons above the face group.
    c.paint(lambda x, y: sd_ellipse(x, y, 1325, 170, 32, 40), BODY_EDGE, (1280, 120, 1370, 220))
    c.paint(lambda x, y: sd_ellipse(x, y, 1325, 170, 27, 35), WHITE_BTN, (1285, 125, 1365, 215))
    c.paint(lambda x, y: sd_ellipse(x, y, 1418, 238, 30, 38), BODY_EDGE, (1375, 190, 1465, 285))
    c.paint(lambda x, y: sd_ellipse(x, y, 1418, 238, 25, 33), BLACK_BTN, (1380, 195, 1460, 280))

    # The face diamond, in the Duke's own tilted arrangement.
    for cx, cy, col in ((1275, 245, FACE_Y), (1370, 315, FACE_B), (1260, 345, FACE_X), (1355, 410, FACE_A)):
        c.paint(lambda x, y, cx=cx, cy=cy: sd_circle(x, y, cx, cy, 48), BODY_EDGE, (cx - 60, cy - 60, cx + 60, cy + 60))
        c.paint(lambda x, y, cx=cx, cy=cy: sd_circle(x, y, cx, cy, 42), col, (cx - 55, cy - 55, cx + 55, cy + 55))

    # Back and Start.
    for bx in (930, 1070):
        c.paint(lambda x, y, bx=bx: sd_ellipse(x, y, bx, 645, 45, 29), BODY_EDGE, (bx - 60, 600, bx + 60, 690))
        c.paint(lambda x, y, bx=bx: sd_ellipse(x, y, bx, 645, 40, 24), SMALL_BTN, (bx - 55, 605, bx + 55, 685))


def write_png(path, pixels, width, height):
    """Write an RGBA PNG. Only used for the archived master, never for game art.

    A master render is 30 to 100 times the size it is ever drawn at, so keeping
    it as an uncompressed .tga costs hundreds of megabytes in the repository for
    a file no build step reads. PNG is stdlib work (zlib), lossless, and keeps
    the alpha, so re-baking later at a different size stays possible without
    tracking the original export.
    """
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0, no prediction: simplest, and zlib still does the work
        raw += pixels[y * width * 4:(y + 1) * width * 4]

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def read_png(path):
    """Read an 8-bit RGB or RGBA PNG. Returns (rgba top-down, w, h).

    Enough of the format to read what this script writes and what a drawing
    program exports: no interlacing, no palettes, no 16-bit channels. Each is
    refused by name rather than half-handled.
    """
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s: not a PNG" % path)
    pos = 8
    width = height = None
    channels = 0
    idat = bytearray()
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", payload)
            if depth != 8:
                raise ValueError("%s: %d-bit channels not supported" % (path, depth))
            if interlace:
                raise ValueError("%s: interlaced PNG not supported" % path)
            if color_type == 2:
                channels = 3
            elif color_type == 6:
                channels = 4
            else:
                raise ValueError("%s: colour type %d not supported (only RGB and RGBA)" % (path, color_type))
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
    if width is None:
        raise ValueError("%s: no IHDR" % path)

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif filter_type != 0:
            raise ValueError("%s: unknown row filter %d" % (path, filter_type))
        prev = line

        dst = y * width * 4
        if channels == 4:
            out[dst:dst + width * 4] = line
        else:
            for x in range(width):
                o, s = dst + x * 4, x * 3
                out[o], out[o + 1], out[o + 2], out[o + 3] = line[s], line[s + 1], line[s + 2], 255
    return out, width, height


def load_rgba(path):
    """Read a master image as top-down RGBA, whichever of the two formats it is."""
    with open(path, "rb") as f:
        signature = f.read(8)
    if signature == b"\x89PNG\r\n\x1a\n":
        return read_png(path)
    return read_tga(path)


def content_bbox(rgba, width, height):
    """Tight bounds of everything with any alpha at all."""
    x0, y0, x1, y1 = width, height, -1, -1
    for y in range(height):
        base = y * width * 4
        alpha = rgba[base + 3:base + width * 4:4]
        if not max(alpha):
            continue
        lo = next(i for i, v in enumerate(alpha) if v)
        hi = width - 1 - next(i for i, v in enumerate(reversed(alpha)) if v)
        if lo < x0:
            x0 = lo
        if hi > x1:
            x1 = hi
        if y < y0:
            y0 = y
        if y > y1:
            y1 = y
    if x1 < 0:
        raise ValueError("the master image is entirely transparent")
    return x0, y0, x1, y1


def bake_from_master(path, target_w, keep_alpha):
    """Area-average a master render down to the size the menu draws it at.

    Area average, not the bilinear four-tap the title bake uses: this is a
    27x reduction, where four taps would sample a fraction of the source
    texels and alias every outline into a dotted line. And premultiplied,
    for the reason the title bake spells out - colour under a transparent
    texel is undefined and drags into soft edges if it is averaged raw.

    The master is cropped to its own content first. A render exported from a
    drawing program carries whatever padding its canvas had (the one this was
    written against is 8600x4800 holding a 4582x3394 controller), and padding
    that survives into the .tga is padding the console pays texture memory for.
    """
    src, src_w, src_h = load_rgba(path)
    x0, y0, x1, y1 = content_bbox(src, src_w, src_h)
    cw, ch = x1 - x0 + 1, y1 - y0 + 1
    dw = max(1, int(target_w))
    dh = max(1, int(round(dw * ch / cw)))
    # This is an area average, which only reduces. Asked to enlarge, every
    # destination row past the source's own height would take no samples at
    # all and come out black - a bake that looks like it worked. Refuse
    # instead: a master that cannot cover the requested size is a missing
    # input, not something to paper over.
    if dw > cw or dh > ch:
        raise ValueError("%s: master content is %dx%d, cannot bake up to %dx%d - supply a larger master"
                         % (path, cw, ch, dw, dh))

    # Horizontal bin edges, computed once: every source column lands in exactly
    # one destination column, so nothing is sampled twice or skipped.
    bins = [((dx * cw) // dw, ((dx + 1) * cw) // dw) for dx in range(dw)]
    bins = [(a, b if b > a else a + 1) for a, b in bins]

    acc = [[0] * (dw * 4) for _ in range(dh)]
    counts = [0] * dh

    for cy in range(ch):
        base = (y0 + cy) * src_w * 4 + x0 * 4
        row = src[base:base + cw * 4]

        alpha = row[3::4]
        pr = [row[i * 4] * alpha[i] // 255 for i in range(cw)]
        pg = [row[i * 4 + 1] * alpha[i] // 255 for i in range(cw)]
        pb = [row[i * 4 + 2] * alpha[i] // 255 for i in range(cw)]

        dy = (cy * dh) // ch
        target = acc[dy]
        counts[dy] += 1
        for dx, (a, b) in enumerate(bins):
            o = dx * 4
            target[o] += sum(pr[a:b])
            target[o + 1] += sum(pg[a:b])
            target[o + 2] += sum(pb[a:b])
            target[o + 3] += sum(alpha[a:b])

    canvas = Canvas(dw, dh, 1.0, (0, 0, 0, 0 if keep_alpha else 255))
    for dy in range(dh):
        rows = max(1, counts[dy])
        target = acc[dy]
        for dx in range(dw):
            o = dx * 4
            span = bins[dx][1] - bins[dx][0]
            n = rows * span
            pr = target[o] / n
            pg = target[o + 1] / n
            pb = target[o + 2] / n
            pa = target[o + 3] / n
            d = (dy * dw + dx) * 4
            if keep_alpha:
                # Unpremultiply back to straight alpha, which is what the TGA
                # stores and what imagelib hands the renderer.
                if pa <= 0.0:
                    canvas.px[d] = canvas.px[d + 1] = canvas.px[d + 2] = canvas.px[d + 3] = 0
                    continue
                scale = 255.0 / pa
                canvas.px[d] = min(255, int(pr * scale + 0.5))
                canvas.px[d + 1] = min(255, int(pg * scale + 0.5))
                canvas.px[d + 2] = min(255, int(pb * scale + 0.5))
                canvas.px[d + 3] = min(255, int(pa + 0.5))
            else:
                # Flattened onto black, which IS the premultiplied value - the
                # menu background behind this diagram is solid black (measured
                # from the HL25 tiles), so this is not an approximation.
                canvas.px[d] = min(255, int(pr + 0.5))
                canvas.px[d + 1] = min(255, int(pg + 0.5))
                canvas.px[d + 2] = min(255, int(pb + 0.5))
                canvas.px[d + 3] = 255
    return canvas, (cw, ch)


def build_duke(scale, alpha):
    width = int(round(DESIGN_W * scale))
    height = int(round(DESIGN_H * scale))
    # Opaque by default. The HL25 background tiles are solid black (measured
    # from the real files), so an opaque diagram is indistinguishable from a
    # transparent one on screen and costs no second full-resolution alpha
    # buffer in ref_soft (r_image.c:254-255 allocates one for any texture
    # kept with TF_HAS_ALPHA).
    background = (0, 0, 0, 0) if alpha else (0, 0, 0, 255)
    canvas = Canvas(width, height, scale, background)
    draw_duke(canvas)
    return canvas


# ---------------------------------------------------------------------------
# The legend button
# ---------------------------------------------------------------------------

# The tracked glyph's edge is soft, not a one-pixel antialias ramp: measured
# off the file itself, alpha is solid out to r = 12.5 and only reaches zero at
# r = 17.2, which is why it reads as a glow rather than a hard token at 20 px
# on a CRT. Reproduced here rather than replaced with a crisp disc, so
# --regen-legend cannot quietly restyle something that already shipped.
LEGEND_SOLID_R = 12.5
LEGEND_FADE_R = 17.2


def build_legend_button(size=32):
    """A white disc with the tracked glyph's soft edge, tinted at draw time."""
    canvas = Canvas(size, size, size / 32.0, (255, 255, 255, 0))
    c = (size - 1) / 2.0
    s = size / 32.0
    for y in range(size):
        for x in range(size):
            d = math.hypot(x - c, y - c) / s
            if d <= LEGEND_SOLID_R:
                a = 255
            elif d >= LEGEND_FADE_R:
                a = 0
            else:
                t = (d - LEGEND_SOLID_R) / (LEGEND_FADE_R - LEGEND_SOLID_R)
                a = int(round(255 * (1.0 - t * t * (3.0 - 2.0 * t))))
            o = (y * size + x) * 4
            canvas.px[o] = canvas.px[o + 1] = canvas.px[o + 2] = 255
            canvas.px[o + 3] = a
    return canvas


def check_legend_button(path):
    """Validate the tracked glyph's shape without demanding byte equality.

    It shipped on a verified disc and was not made by this script, so a byte
    difference here would mean "different tool", not "wrong file". What has to
    hold is what Legend.cpp relies on when it tints this one image into six
    different glyphs: square, pure white wherever it is visible, and a single
    radially symmetric alpha island that never grows outward again.
    """
    rgba, w, h = read_tga(path)
    problems = []
    if w != h:
        problems.append("not square: %dx%d" % (w, h))
        return problems
    if w < 16:
        problems.append("%dx%d is too small to be this glyph" % (w, h))
        return problems

    for i in range(0, len(rgba), 4):
        if rgba[i + 3] and (rgba[i], rgba[i + 1], rgba[i + 2]) != (255, 255, 255):
            problems.append("a visible texel is not pure white, so a tint would come out off-colour")
            break

    c = (w - 1) / 2.0
    if rgba[3] != 0 or rgba[(w - 1) * 4 + 3] != 0:
        problems.append("the corners are not transparent, so the glyph would draw as a square")

    # Per-radius minimum AND maximum, not the maximum alone. Codex review
    # round 2: the earlier version checked two corners, the centre texel and
    # each radius's maximum, all of which a single opaque centre texel on an
    # otherwise empty image satisfies - it is square, its corners are clear,
    # its centre is opaque, and its radial maxima never climb. That is not a
    # disc, and blessing it would bless an invisible legend glyph.
    lo = {}
    peak = {}
    mass = 0
    for y in range(h):
        for x in range(w):
            a = rgba[(y * w + x) * 4 + 3]
            mass += a
            r = int(round(math.hypot(x - c, y - c)))
            lo[r] = min(lo.get(r, 255), a)
            peak[r] = max(peak.get(r, 0), a)

    # The solid core: the largest radius out to which EVERY texel is opaque.
    solid = -1
    for r in sorted(lo):
        if lo[r] != 255:
            break
        solid = r

    # Thresholds are fractions of the glyph's own size, and every one of them
    # is measured off the tracked asset with room to spare rather than picked:
    # legend_button.tga is 32x32 with a solid core out to radius 12 (0.375 of
    # its width), fading to nothing at 17, and 0.689 of the box's alpha mass.
    # Fractions rather than an exact 32x32 so a higher-resolution glyph still
    # validates, and they are what actually closes the hole above - a size
    # check alone would not have.
    if solid < 0.25 * w:
        problems.append("its solid core reaches radius %d, under the %d a disc this size needs - this is not a filled disc"
                        % (solid, int(0.25 * w)))
    if mass < 0.40 * w * h * 255:
        problems.append("only %.1f%% of the glyph's alpha mass is present, well under a disc's own - it would draw close to nothing"
                        % (100.0 * mass / (w * h * 255)))

    # Alpha must fall off with radius and never climb back. A ring, a square
    # or a stray texel all break this; antialiasing does not.
    previous = 255
    for r in sorted(peak):
        if peak[r] > previous:
            problems.append("alpha rises again at radius %d, so this is not one clean disc" % r)
            break
        previous = peak[r]
    return problems


# ---------------------------------------------------------------------------

def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    default_out = os.path.join(repo, "assets", "gfx", "shell")

    default_master = os.path.join(repo, "assets", "resource", "XboxDuke.png")

    ap = argparse.ArgumentParser(description="Generate this fork's Xbox menu art.")
    ap.add_argument("--output", default=default_out, help="directory to write into (default: assets/gfx/shell)")
    ap.add_argument("--master", default=default_master,
                    help="master render of the controller to bake down (default: assets/resource/XboxDuke.png)")
    ap.add_argument("--width", type=int, default=280,
                    help="width of the baked diagram in real pixels (default 280, of the 640 the console draws)")
    ap.add_argument("--scale", type=float, default=0.16,
                    help="synthetic fallback only: design-space scale, 0.16 gives 320x178")
    ap.add_argument("--synthetic", action="store_true", help="draw the diagram from geometry even if a master exists")
    ap.add_argument("--alpha", action="store_true", help="keep the diagram's alpha instead of flattening onto black")
    ap.add_argument("--archive-master", metavar="PX",
                    help="also write the master cropped and reduced to this width, as PNG, next to the master")
    ap.add_argument("--regen-legend", action="store_true", help="also rewrite legend_button.tga (off by default: it already shipped)")
    ap.add_argument("--check", action="store_true", help="verify the tracked files instead of writing anything")
    args = ap.parse_args(argv)

    duke_path = os.path.join(args.output, "duke.tga")
    legend_path = os.path.join(args.output, "legend_button.tga")

    def render_duke():
        """The master if there is one, geometry if there is not. Returns (canvas, note)."""
        if not args.synthetic and os.path.exists(args.master):
            canvas, content = bake_from_master(args.master, args.width, args.alpha)
            return canvas, "from %s, master content %dx%d" % (os.path.basename(args.master), content[0], content[1])
        if not args.synthetic:
            print("xbox-menu-art: no master at %s, drawing the diagram from geometry instead." % args.master, file=sys.stderr)
        return build_duke(args.scale, args.alpha), "drawn from geometry"

    if args.check:
        failures = 0
        if not os.path.exists(duke_path):
            print("xbox-menu-art: missing %s - run this script without --check to produce it." % duke_path, file=sys.stderr)
            failures += 1
        elif not args.synthetic and not os.path.exists(args.master):
            # Refusing beats falling back. Without the master this would compare
            # the tracked asset against the vector fallback, which can never
            # match, and report it as "edited by hand" - a verdict about the
            # asset for what is really a missing input.
            print("xbox-menu-art: cannot verify %s - its master is missing (%s), so there is nothing to re-render from."
                  % (duke_path, args.master), file=sys.stderr)
            failures += 1
        else:
            canvas, _ = render_duke()
            fresh = encode_tga(canvas.px, canvas.w, canvas.h, args.alpha)
            with open(duke_path, "rb") as f:
                tracked = f.read()
            if fresh != tracked:
                print("xbox-menu-art: %s does not match a fresh render - it was edited by hand or the geometry changed." % duke_path, file=sys.stderr)
                failures += 1
            else:
                print("xbox-menu-art: duke.tga matches a fresh render, byte for byte.")
        if os.path.exists(legend_path):
            problems = check_legend_button(legend_path)
            if problems:
                for p in problems:
                    print("xbox-menu-art: legend_button.tga: %s" % p, file=sys.stderr)
                failures += 1
            else:
                print("xbox-menu-art: legend_button.tga is a clean tintable disc.")
        else:
            print("xbox-menu-art: missing %s" % legend_path, file=sys.stderr)
            failures += 1
        return 1 if failures else 0

    if not os.path.isdir(args.output):
        os.makedirs(args.output)

    canvas, note = render_duke()
    size = write_tga(duke_path, canvas.px, canvas.w, canvas.h, args.alpha)
    resident = canvas.w * canvas.h * 2 * 4 // 3
    if args.alpha:
        resident += canvas.w * canvas.h * 2
    print("xbox-menu-art: wrote %s, %dx%d, %s, %d bytes on disc, about %d KiB resident in ref_soft."
          % (duke_path, canvas.w, canvas.h, note, size, resident // 1024))

    if args.archive_master:
        if not os.path.exists(args.master):
            print("xbox-menu-art: --archive-master needs a master and there is none at %s." % args.master, file=sys.stderr)
            return 1
        archive_w = int(args.archive_master)
        reduced, content = bake_from_master(args.master, archive_w, True)
        archive_path = os.path.splitext(args.master)[0] + ".png"
        size = write_png(archive_path, reduced.px, reduced.w, reduced.h)
        print("xbox-menu-art: archived the master as %s, %dx%d, %d KiB - cropped from %dx%d of content."
              % (archive_path, reduced.w, reduced.h, size // 1024, content[0], content[1]))
        print("                 It is %.1fx the baked width, so a bigger bake stays possible without the original export."
              % (archive_w / float(args.width)))

    if args.regen_legend:
        lc = build_legend_button()
        size = write_tga(legend_path, lc.px, lc.w, lc.h, True)
        print("xbox-menu-art: rewrote %s, %dx%d, %d bytes." % (legend_path, lc.w, lc.h, size))

    print("xbox-menu-art: stage it with tools/xbox-xiso.sh the same way legend_button.tga is staged.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
