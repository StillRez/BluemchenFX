#!/usr/bin/env python3
"""Render the UI host tests' screens as one PNG contact sheet.

    mkdir -p /tmp/shots && UI_SHOTS=/tmp/shots make -C tests run ARGS=ui_
    python3 tools/ui_shots.py /tmp/shots ui_screens.png

Every distinct 64x32 screen the UI tests drew is written as a PBM file;
this scales them up (OLED-style, white on dark grey) into a grid.
Needs Pillow (pip install pillow).
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

SCALE, PAD, COLS = 5, 12, 6


def read_pbm(path):
    tokens = path.read_text().split()
    assert tokens[0] == "P1"
    w, h = int(tokens[1]), int(tokens[2])
    bits = "".join(tokens[3:])
    return w, h, bits


def main(src, out, only=None):
    files = sorted(Path(src).glob("screen_*.pbm"))
    if only:
        files = [f for f in files if int(f.stem.split("_")[1]) in only]
    if not files:
        sys.exit(f"no screen_*.pbm in {src}")
    w, h = 64, 32
    cw, ch = w * SCALE + PAD, h * SCALE + PAD + 14
    rows = (len(files) + COLS - 1) // COLS
    sheet = Image.new("RGB", (COLS * cw + PAD, rows * ch + PAD), (235, 235, 235))
    draw = ImageDraw.Draw(sheet)
    for i, f in enumerate(files):
        w, h, bits = read_pbm(f)
        img = Image.new("RGB", (w, h))
        img.putdata([(240, 248, 255) if b == "1" else (18, 18, 24) for b in bits])
        img = img.resize((w * SCALE, h * SCALE), Image.NEAREST)
        x, y = PAD + (i % COLS) * cw, PAD + (i // COLS) * ch
        sheet.paste(img, (x, y + 14))
        draw.text((x, y), f.stem.split("_")[1], fill=(80, 80, 80))
    sheet.save(out)
    print(f"{len(files)} screens -> {out}")


if __name__ == "__main__":
    pick = {int(n) for n in sys.argv[3].split(",")} if len(sys.argv) > 3 else None
    main(sys.argv[1], sys.argv[2], pick)
