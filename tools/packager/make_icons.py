#!/usr/bin/env python3
"""Render the packager's icon assets from the Xash3D icon. Needs Pillow; the
packager itself does not (it ships the PNG/PGM this writes).

    python tools/packager/make_icons.py

Writes into tools/packager/assets/:
  icon-xash-128.png   128x128, the dashboard tile (default.tbn, $$XTIMAGE)
  icon-xash-64.png    64x64, the save image ($$XSIMAGE)
  titleimage.xbx      the 128 image as the dashboard's XPR (the $$XTIMAGE section,
  saveimage.xbx       and UDATA/<title id>/TitleImage.xbx); the 64 image likewise
  boot-logo.pgm       100x17 binary PGM, the XBE boot logo
"""

import io
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from r3dpack import xbepatch  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "game_launch", "icon-xash-material.png")
OUT = os.path.join(HERE, "assets")


def main():
    icon = Image.open(SRC).convert("RGBA")
    for n in (128, 64):
        bg = Image.new("RGBA", (n, n), (0, 0, 0, 255))
        bg.alpha_composite(icon.resize((n, n), Image.LANCZOS))
        rgb = bg.convert("RGB")
        rgb.save(os.path.join(OUT, "icon-xash-%d.png" % n))
        buf = io.BytesIO()
        rgb.save(buf, "DDS", pixel_format="DXT1")  # 128-byte DDS header, then the blocks
        xpr = xbepatch.make_xpr(buf.getvalue()[128:], n)
        with open(os.path.join(OUT, {128: "titleimage.xbx", 64: "saveimage.xbx"}[n]), "wb") as f:
            f.write(xpr)

    # boot logo: 100x17, light on black. The mark at the left, the title beside it.
    w, h = 100, 17
    logo = Image.new("L", (w, h), 0)
    mark = Image.new("L", icon.size, 0)
    mark.paste(icon.convert("L"), mask=icon.split()[3])
    logo.paste(mark.resize((15, 15), Image.LANCZOS), (1, 1))
    d = ImageDraw.Draw(logo)
    try:
        font = ImageFont.truetype(os.path.join(HERE, "..", "..", "assets", "fonts", "FiraSansCondensed-Regular.ttf"), 13)
    except OSError:
        font = ImageFont.load_default()
    d.text((20, 1), "Half-Life", fill=255, font=font)
    with open(os.path.join(OUT, "boot-logo.pgm"), "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h) + logo.tobytes())
    print("wrote the icon PNGs, titleimage.xbx, saveimage.xbx and boot-logo.pgm")


if __name__ == "__main__":
    main()
