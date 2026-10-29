#!/usr/bin/env python3
"""Render the Google Material Symbols VoidOS uses into assets/micons/*.png.

Every icon on the watch is a real Material Symbol (Outlined family,
FILL 1, weight 500, grade 0, optical size matched to the pixel size),
rendered from Google's own variable font:

  https://github.com/google/material-design-icons/tree/master/variablefont
  MaterialSymbolsOutlined[FILL,GRAD,opsz,wght].ttf  (Apache License 2.0)

The font is ~10 MB, so it is not committed: the rendered PNGs are, and
tools/gen_assets.py bakes those into gen/micons.c at build time. Re-run
this only to add an icon or change a size:

  python3 tools/render_micons.py /path/to/MaterialSymbolsOutlined[...].ttf

The icon list lives in tools/micons_list.py, shared with gen_assets.py.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(__file__))
from micons_list import ICONS, OUT_DIR  # noqa: E402


def codepoints(font_path):
    cp = {}
    side = os.path.splitext(font_path)[0] + ".codepoints"
    if os.path.exists(side):
        for line in open(side):
            name, hexcp = line.split()
            cp.setdefault(name, int(hexcp, 16))
    return cp


def render(font_path, symbol, px, fill, cp):
    # supersample 4x, then box-filter down: crisper than FreeType's own
    # hinting at 16-24 px on a 240x240 panel
    ss = 4
    font = ImageFont.truetype(font_path, px * ss)
    # axis order of the Google font: FILL, GRAD, opsz, wght
    font.set_variation_by_axes([fill, 0, max(20, min(48, px)), 500])
    ch = chr(cp[symbol]) if symbol in cp else symbol   # ligature fallback
    img = Image.new("L", (px * ss, px * ss), 0)
    # Material Symbols sit on the baseline and fill the em box exactly
    ImageDraw.Draw(img).text((0, px * ss), ch, font=font, fill=255,
                             anchor="ls", features=["liga"])
    if img.getbbox() is None:
        raise SystemExit(f"{symbol}: empty glyph (not in this font?)")
    return img.resize((px, px), Image.BOX)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    font_path = sys.argv[1]
    cp = codepoints(font_path)
    os.makedirs(OUT_DIR, exist_ok=True)
    n = 0
    for ident, symbol, fill, sizes in ICONS:
        for px in sizes:
            a = render(font_path, symbol, px, fill, cp)
            out = Image.new("RGBA", (px, px), (255, 255, 255, 0))
            out.putalpha(a)
            out.save(os.path.join(OUT_DIR, f"{symbol}_{px}.png"))
            n += 1
    print(f"wrote {n} PNGs to {OUT_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
