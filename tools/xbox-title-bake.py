#!/usr/bin/env python3
"""Bake the retail animated title down to console scale.

    python3 tools/xbox-title-bake.py --game-dir <valve> --output <dir> [--width 640]
    python3 tools/xbox-title-bake.py --check <dir>

What this is
------------
`tools/asset-studio --bake-menu-art` already bakes the menu BACKGROUND
(fork-plan.md XM item 0 stage B). The animated title was left out of it
on purpose, and `UiPacker.cpp`'s own comment above `resizeBilinear` says
why: the background is opaque, so a straight RGBA resize is correct
there, while the title's eleven layers carry real, non-uniform alpha,
where an unpremultiplied resize bleeds colour out of fully transparent
texels into every soft edge. That is the whole reason this is a separate
tool rather than another mode of that one: it resizes in premultiplied
space and converts back, which is the part that had to be got right.

What it costs and saves
-----------------------
Measured from a real HL25 install, not estimated: the eleven layers are
1.18 Mpixel at mip 0 (three at 800x86, four around 450x270 and their
four blip twins), all with alpha. Under `ref_soft`'s uploader a texture
costs `w*h*2*1.33` plus another `w*h*2` when its alpha is kept
(`ref/soft/r_image.c:235-255`), so retail's layers are about 5.2 MiB
resident. At 640x480 the banner draws everything through
`scale = ScreenWidth / 1024` = 0.625, so baking at that factor is a
texel-for-texel match on screen and costs 0.39 of that, about 2.0 MiB.

The code change this bake REQUIRES
----------------------------------
`CMenuAnimatedBanner` reads each layer's true size out of the TGA header
(`UI_GetTargaImageDimensions`, AnimatedBanner.cpp:30-47) and then
multiplies by `scale` when drawing. Baked layers are ALREADY at scale, so
the fork has to skip that multiply for them, or the title draws at 0.39
of its intended size. This tool does not and cannot do that half; it
writes `title-bake.json` next to the art recording the factor it used, so
the code side has a value to check rather than a constant to guess.

Dependencies: none, standard library only.
"""

import argparse
import hashlib
import json
import os
import struct
import sys

# The eleven layers CMenuAnimatedBanner loads, in the order it loads them.
# `TryLoad` is all-or-nothing (AnimatedBanner.cpp:49-84): a missing layer
# turns the whole animated title off rather than dropping one element, so a
# partial bake is a silent regression and this list is checked, not globbed.
LAYERS = (
    ["resource/logo.tga", "resource/logo_blip.tga", "resource/logo_blip2.tga"]
    + ["resource/logo_big_blurred_%d.tga" % i for i in range(4)]
    + ["resource/logo_big_blurred_blip_%d.tga" % i for i in range(4)]
)


def read_tga(path):
    """Read an uncompressed 24/32-bit truecolor TGA. Returns (rgba, w, h)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 18:
        raise ValueError("%s: shorter than a TGA header" % path)
    id_len, _, image_type = data[0], data[1], data[2]
    width, height = struct.unpack("<HH", data[12:16])
    bpp, descriptor = data[16], data[17]
    if image_type != 2:
        raise ValueError("%s: image type %d not supported (only uncompressed truecolor)" % (path, image_type))
    if bpp not in (24, 32):
        raise ValueError("%s: %d bits per pixel not supported" % (path, bpp))
    stride = bpp // 8
    off = 18 + id_len
    if len(data) - off < width * height * stride:
        raise ValueError("%s: truncated" % path)
    top_down = bool(descriptor & 0x20)
    rgba = bytearray(width * height * 4)
    for row in range(height):
        y = row if top_down else height - 1 - row
        src = off + row * width * stride
        dst = y * width * 4
        for x in range(width):
            s, d = src + x * stride, dst + x * 4
            rgba[d], rgba[d + 1], rgba[d + 2] = data[s + 2], data[s + 1], data[s]
            rgba[d + 3] = data[s + 3] if stride == 4 else 255
    return rgba, width, height


def write_tga(path, rgba, width, height):
    """Write 32-bit uncompressed TGA, bottom-up, descriptor 0x08 - retail's own.

    Verified rather than assumed: `img_tga.c:137-148` fills the target buffer
    backwards when bit 5 is clear, so the file's first row is the image's
    bottom row. Every retail layer this tool reads uses exactly this, so a
    baked file drops into the same load path unchanged.
    """
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, width, height, 32, 0x08)
    out = bytearray(header)
    for y in range(height - 1, -1, -1):
        row = rgba[y * width * 4:(y + 1) * width * 4]
        for x in range(width):
            o = x * 4
            out += bytes((row[o + 2], row[o + 1], row[o], row[o + 3]))
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def resize_premultiplied(src, sw, sh, dw, dh):
    """Bilinear resize in premultiplied-alpha space, then unpremultiply.

    This is the one thing this tool does that a naive resize gets wrong.
    Colour in a fully transparent texel is undefined - retail's layers carry
    black there - so averaging raw RGB across a soft edge drags that black
    into the visible pixels and rims every glow with a dark halo. Weighting
    colour by alpha first, then dividing the result back out, keeps the edge
    the colour the artist put there.
    """
    dst = bytearray(dw * dh * 4)
    x_ratio = sw / dw
    y_ratio = sh / dh
    for dy in range(dh):
        fy = (dy + 0.5) * y_ratio - 0.5
        y0 = int(fy) if fy >= 0 else -1
        wy = fy - y0
        y0c = min(max(y0, 0), sh - 1)
        y1c = min(max(y0 + 1, 0), sh - 1)
        for dx in range(dw):
            fx = (dx + 0.5) * x_ratio - 0.5
            x0 = int(fx) if fx >= 0 else -1
            wx = fx - x0
            x0c = min(max(x0, 0), sw - 1)
            x1c = min(max(x0 + 1, 0), sw - 1)

            acc_r = acc_g = acc_b = acc_a = 0.0
            for (yc, wgt_y) in ((y0c, 1.0 - wy), (y1c, wy)):
                if wgt_y == 0.0:
                    continue
                base = yc * sw * 4
                for (xc, wgt_x) in ((x0c, 1.0 - wx), (x1c, wx)):
                    w = wgt_y * wgt_x
                    if w == 0.0:
                        continue
                    o = base + xc * 4
                    a = src[o + 3] / 255.0
                    acc_r += src[o] * a * w
                    acc_g += src[o + 1] * a * w
                    acc_b += src[o + 2] * a * w
                    acc_a += a * w

            o = (dy * dw + dx) * 4
            if acc_a <= 0.0:
                dst[o] = dst[o + 1] = dst[o + 2] = dst[o + 3] = 0
            else:
                dst[o] = min(255, int(acc_r / acc_a + 0.5))
                dst[o + 1] = min(255, int(acc_g / acc_a + 0.5))
                dst[o + 2] = min(255, int(acc_b / acc_a + 0.5))
                dst[o + 3] = min(255, int(acc_a * 255 + 0.5))
    return dst


def soft_cost(w, h, has_alpha=True):
    """Resident bytes under ref_soft: 16-bit texels, four mip levels, plus alpha."""
    cost = w * h * 2 * 4 // 3
    if has_alpha:
        cost += w * h * 2
    return cost


def bake(game_dir, out_dir, width):
    factor = width / 1024.0
    out_resource = os.path.join(out_dir, "resource")
    if not os.path.isdir(out_resource):
        os.makedirs(out_resource)

    missing = [rel for rel in LAYERS if not os.path.exists(os.path.join(game_dir, rel))]
    if missing:
        print("xbox-title-bake: this game directory has no animated title - missing %s." % ", ".join(missing), file=sys.stderr)
        print("xbox-title-bake: that is a pre-anniversary content profile; it uses media/logo.avi, which is dead on Xbox. Nothing to bake.", file=sys.stderr)
        return None

    manifest = {"width": width, "factor": factor, "layers": []}
    src_total = dst_total = 0
    for rel in LAYERS:
        src_path = os.path.join(game_dir, rel)
        rgba, sw, sh = read_tga(src_path)
        dw = max(1, int(round(sw * factor)))
        dh = max(1, int(round(sh * factor)))
        out_path = os.path.join(out_dir, rel)
        baked = resize_premultiplied(rgba, sw, sh, dw, dh)
        write_tga(out_path, baked, dw, dh)
        src_total += soft_cost(sw, sh)
        dst_total += soft_cost(dw, dh)
        with open(out_path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        manifest["layers"].append({
            "path": rel,
            "source": [sw, sh],
            "baked": [dw, dh],
            # Codex review: --check compared dimensions only, so replacing every
            # layer's pixels with transparent black while keeping its size
            # reached the success message - the false-clean measurement this
            # project keeps catching. The digest is of the written file.
            "sha256": digest,
        })
        print("xbox-title-bake: %-42s %4dx%-4d -> %4dx%-4d" % (rel, sw, sh, dw, dh))

    manifest["resident_bytes"] = {"retail": src_total, "baked": dst_total}
    with open(os.path.join(out_dir, "title-bake.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    # The same factor again, in the only shape the consumer can actually read.
    # `mainui` has no JSON parser - it reads token streams through
    # `EngFuncs::COM_ParseFile`, which is how `colors.lst` is parsed
    # (`BaseMenu.cpp:1003-1020`, a KEY line followed by its value). The .json
    # above is this tool's own manifest, for `--check` and for a human; this
    # file is what ships on the disc and what `CMenuAnimatedBanner` can read to
    # learn that its layers are already at scale.
    with open(os.path.join(out_dir, "resource", "title_bake.txt"), "w") as f:
        f.write("// Written by tools/xbox-title-bake.py. Layers in this gamedir are\n")
        f.write("// already reduced by this factor, so do not scale them again.\n")
        f.write("TITLE_BAKE_SCALE\n")
        f.write("%.6f\n" % factor)

    print("xbox-title-bake: resident under ref_soft %.2f MiB retail -> %.2f MiB baked, saving %.2f MiB."
          % (src_total / 1048576.0, dst_total / 1048576.0, (src_total - dst_total) / 1048576.0))
    print("xbox-title-bake: these layers are ALREADY at scale %.4f. CMenuAnimatedBanner multiplies" % factor)
    print("                 trueLogoSz by ScreenWidth/1024 when it draws (AnimatedBanner.cpp:86-122),")
    print("                 so the fork must skip that multiply for a baked disc or the title renders")
    print("                 at %.2f of its intended size. title-bake.json carries the factor to check against." % (factor * factor))
    print("xbox-title-bake: overlay it with tools/xbox-xiso.sh --menu-bake %s" % out_dir)
    return manifest


def check(out_dir):
    manifest_path = os.path.join(out_dir, "title-bake.json")
    if not os.path.exists(manifest_path):
        print("xbox-title-bake: %s has no title-bake.json - not a title bake." % out_dir, file=sys.stderr)
        return 1
    with open(manifest_path) as f:
        manifest = json.load(f)

    failures = 0
    listed = {entry["path"] for entry in manifest["layers"]}
    for rel in LAYERS:
        if rel not in listed:
            print("xbox-title-bake: %s is not in the manifest - TryLoad is all-or-nothing, so a partial bake turns the title off entirely." % rel, file=sys.stderr)
            failures += 1

    for entry in manifest["layers"]:
        path = os.path.join(out_dir, entry["path"])
        if not os.path.exists(path):
            print("xbox-title-bake: missing %s" % path, file=sys.stderr)
            failures += 1
            continue
        rgba, w, h = read_tga(path)
        if [w, h] != entry["baked"]:
            print("xbox-title-bake: %s is %dx%d, the manifest says %dx%d." % (entry["path"], w, h, entry["baked"][0], entry["baked"][1]), file=sys.stderr)
            failures += 1
            continue

        # Content, not just shape. Dimensions alone let a blanked or truncated
        # layer through, and every layer here carries the title's own artwork.
        with open(path, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        expected = entry.get("sha256")
        if not expected:
            print("xbox-title-bake: %s has no sha256 in the manifest - re-run the bake; a manifest that only records sizes cannot detect a blanked layer."
                  % entry["path"], file=sys.stderr)
            failures += 1
        elif digest != expected:
            print("xbox-title-bake: %s does not match its recorded digest - the file changed after the bake." % entry["path"], file=sys.stderr)
            failures += 1

        # A digest only proves the file is the one the bake wrote. It cannot
        # prove the bake itself produced anything visible, since regenerating
        # both file and manifest from a broken source would agree with itself.
        # So the content is measured independently.
        #
        # Codex review round 2: "is any texel non-transparent" was one byte
        # away from the blank layer it was meant to catch - a layer with a
        # single visible pixel passed. The floor is a coverage FRACTION, and a
        # measured one: across the real eleven HL25 layers baked at 640, the
        # least covered is logo.tga at 0.094 of its texels carrying any alpha,
        # and the most sparse of the blurred strips is 0.198. A floor of 0.03
        # sits three times under the real minimum, so it cannot fail a genuine
        # bake, while a layer reduced to a handful of pixels cannot reach it.
        # A fraction rather than a count so it holds at any --width.
        alpha = rgba[3::4]
        covered = sum(1 for v in alpha if v) / float(len(alpha))
        if covered < 0.03:
            print("xbox-title-bake: %s has visible content on only %.2f%% of its texels - the title would draw next to nothing."
                  % (entry["path"], covered * 100.0), file=sys.stderr)
            failures += 1

    marker = os.path.join(out_dir, "resource", "title_bake.txt")
    if not os.path.exists(marker):
        print("xbox-title-bake: %s is missing - the disc would carry baked layers with nothing to tell the menu so." % marker, file=sys.stderr)
        failures += 1
    else:
        with open(marker) as f:
            words = f.read().split()
        try:
            written = float(words[words.index("TITLE_BAKE_SCALE") + 1])
        except (ValueError, IndexError):
            print("xbox-title-bake: %s has no readable TITLE_BAKE_SCALE value." % marker, file=sys.stderr)
            failures += 1
        else:
            if abs(written - manifest["factor"]) > 1e-6:
                print("xbox-title-bake: %s says %.6f, the manifest says %.6f." % (marker, written, manifest["factor"]), file=sys.stderr)
                failures += 1

    if failures:
        return 1
    print("xbox-title-bake: %d layers present and the right size, factor %.4f, marker agrees."
          % (len(manifest["layers"]), manifest["factor"]))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Bake the retail animated title to console scale.")
    ap.add_argument("--game-dir", help="a gamedir holding resource/logo*.tga, e.g. .../Half-Life/valve")
    ap.add_argument("--output", help="directory to write the bake into")
    ap.add_argument("--width", type=int, default=640, help="target screen width (default 640)")
    ap.add_argument("--check", metavar="DIR", help="verify an existing bake instead of making one")
    args = ap.parse_args(argv)

    if args.check:
        return check(args.check)
    if not args.game_dir or not args.output:
        ap.error("--game-dir and --output are both required unless --check is used")
    if not os.path.isdir(args.game_dir):
        print("xbox-title-bake: --game-dir does not exist: %s" % args.game_dir, file=sys.stderr)
        return 1
    return 0 if bake(args.game_dir, args.output, args.width) else 1


if __name__ == "__main__":
    sys.exit(main())
