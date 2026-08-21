#!/usr/bin/env python3
"""Turn the prover's frame dump into one contact sheet you can actually look at.

Each tile is a real 128x64 panel bitmap as the firmware would push it, drawn at
3x with the anti-burn-in safe area marked. Seeing them all at once is the point:
a layout that is technically legal but ugly, cramped or unbalanced is obvious in
a grid and invisible in a pass/fail line.
"""
import sys, os
from PIL import Image, ImageDraw

W, H, SCALE = 128, 64, 3
SAFE = 6                      # must match SHIFT_MAX in display.h
PAD, LABEL_H, COLS = 10, 26, 6

def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "out"
    dest = sys.argv[2] if len(sys.argv) > 2 else os.path.join(outdir, "sheet.png")
    binp, txtp = os.path.join(outdir, "frames.bin"), os.path.join(outdir, "frames.txt")
    if not (os.path.exists(binp) and os.path.exists(txtp)):
        print("no frames dumped"); return
    labels = [l.rstrip("\n") for l in open(txtp)]
    raw = open(binp, "rb").read()
    stride = (W + 7) // 8 * H
    n = min(len(labels), len(raw) // stride)
    if not n:
        print("no frames dumped"); return

    tw, th = W * SCALE, H * SCALE
    rows = (n + COLS - 1) // COLS
    sheet = Image.new("RGB", (COLS * (tw + PAD) + PAD,
                              rows * (th + LABEL_H + PAD) + PAD), (18, 18, 20))
    d = ImageDraw.Draw(sheet)

    for i in range(n):
        frame = raw[i * stride:(i + 1) * stride]
        tile = Image.new("RGB", (W, H), (0, 0, 0))
        px = tile.load()
        for y in range(H):
            for x in range(W):
                if frame[y * ((W + 7) // 8) + (x >> 3)] >> (7 - (x & 7)) & 1:
                    px[x, y] = (235, 245, 255)      # these panels are all white
        tile = tile.resize((tw, th), Image.NEAREST)
        cx = PAD + (i % COLS) * (tw + PAD)
        cy = PAD + (i // COLS) * (th + LABEL_H + PAD)
        sheet.paste(tile, (cx, cy))
        # The safe area, so a layout hugging the edge is visible at a glance.
        d.rectangle([cx + SAFE * SCALE, cy + SAFE * SCALE,
                     cx + (W - SAFE) * SCALE - 1, cy + (H - SAFE) * SCALE - 1],
                    outline=(70, 90, 70))
        d.rectangle([cx, cy, cx + tw - 1, cy + th - 1], outline=(60, 60, 66))
        lab = labels[i]
        for k, part in enumerate([lab[:46], lab[46:92]]):
            if part:
                d.text((cx + 2, cy + th + 3 + k * 11), part, fill=(150, 160, 175))

    sheet.save(dest)
    print(f"contact sheet: {n} frames -> {dest}")

if __name__ == "__main__":
    main()
