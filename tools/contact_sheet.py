#!/usr/bin/env python3
"""Labelled contact sheet of uisnap renders.

usage: contact_sheet.py OUT.png TITLE DIR [DIR ...]
Every NN_name.ppm in the dirs becomes one 2x tile with its name under it.
A tile whose name starts with 'before_' is drawn with a muted label so
before/after pairs read at a glance."""
import glob, os, sys
from PIL import Image, ImageDraw, ImageFont

out, title, dirs = sys.argv[1], sys.argv[2], sys.argv[3:]
files = []
for d in dirs:
    files += sorted(glob.glob(os.path.join(d, '*.ppm')))
S, PAD, LBL, COLS = 480, 24, 34, 4

def font(sz):
    for p in ('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
              '/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf'):
        if os.path.exists(p):
            return ImageFont.truetype(p, sz)
    return ImageFont.load_default()

f_lbl, f_title = font(22), font(34)
rows = (len(files) + COLS - 1) // COLS
W = COLS * S + (COLS + 1) * PAD
H = 80 + rows * (S + LBL + PAD) + PAD
sheet = Image.new('RGB', (W, H), (14, 16, 22))
dr = ImageDraw.Draw(sheet)
dr.text((PAD, 22), title, fill=(70, 216, 224), font=f_title)
for i, f in enumerate(files):
    r, c = divmod(i, COLS)
    x = PAD + c * (S + PAD)
    y = 80 + r * (S + LBL + PAD)
    im = Image.open(f).convert('RGB').resize((S, S), Image.NEAREST)
    sheet.paste(im, (x, y))
    dr.rectangle([x - 1, y - 1, x + S, y + S], outline=(40, 48, 64))
    name = os.path.splitext(os.path.basename(f))[0]
    before = name.startswith('before_')
    if not before and name[:2].isdigit():
        name = name[3:]
    name = name.replace('_', ' ')
    dr.text((x, y + S + 6), name, font=f_lbl,
            fill=(130, 140, 160) if before else (230, 236, 244))
sheet.save(out)
print(out, sheet.size, len(files), 'screens')
